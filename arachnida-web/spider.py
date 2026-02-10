#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import os
import re
import sys
import time
import hashlib
import subprocess
from urllib.parse import urljoin, urlparse, urldefrag

import requests
from bs4 import BeautifulSoup


ALLOWED_EXTS = {".jpg", ".jpeg", ".png", ".gif", ".bmp"}
DEFAULT_DEPTH = 5
DEFAULT_OUTDIR = "./data"
DEFAULT_TIMEOUT = 25
DEFAULT_DELAY = 0.6  # polite delay

UA = (
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36 ArachnidaSpider/1.0"
)

BASE_HEADERS = {
    "User-Agent": UA,
    "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8",
    "Accept-Language": "en-US,en;q=0.9,hy;q=0.8,ru;q=0.7",
    "Connection": "keep-alive",
    "Upgrade-Insecure-Requests": "1",
}


def eprint(*args, **kwargs):
    print(*args, file=sys.stderr, **kwargs)


def ensure_dir(path: str) -> None:
    os.makedirs(path, exist_ok=True)


def normalize_url(base: str, link: str) -> str | None:
    if not link:
        return None
    link = link.strip()
    if link.startswith(("javascript:", "mailto:", "tel:")):
        return None
    u = urljoin(base, link)
    u, _ = urldefrag(u)
    return u


def same_host(a: str, b: str) -> bool:
    try:
        return urlparse(a).netloc.lower() == urlparse(b).netloc.lower()
    except Exception:
        return False


def safe_filename(name: str) -> str:
    name = re.sub(r"[^a-zA-Z0-9._-]+", "_", name).strip("._")
    return name or "file"


def content_hash(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()[:16]


def guess_ext(url: str, content_type: str | None) -> str | None:
    ct = (content_type or "").lower()
    if "image/jpeg" in ct:
        return ".jpg"
    if "image/png" in ct:
        return ".png"
    if "image/gif" in ct:
        return ".gif"
    if "image/bmp" in ct:
        return ".bmp"

    path = urlparse(url).path.lower()
    for ext in ALLOWED_EXTS:
        if path.endswith(ext):
            return ext
    return None


def extract_from_srcset(base_url: str, srcset: str) -> set[str]:
    urls = set()
    parts = [p.strip() for p in srcset.split(",") if p.strip()]
    for p in parts:
        token = p.split()[0].strip()
        u = normalize_url(base_url, token)
        if u:
            urls.add(u)
    return urls


def extract_image_urls(base_url: str, html: str) -> set[str]:
    soup = BeautifulSoup(html, "html.parser")
    found: set[str] = set()

    for img in soup.find_all("img"):
        for attr in ("src", "data-src", "data-original", "data-lazy-src", "data-zoom-image"):
            val = img.get(attr)
            u = normalize_url(base_url, val) if val else None
            if u:
                found.add(u)

        srcset = img.get("srcset")
        if srcset:
            found |= extract_from_srcset(base_url, srcset)

    for a in soup.find_all("a"):
        href = a.get("href")
        u = normalize_url(base_url, href) if href else None
        if u:
            found.add(u)

    # script/JSON blobs
    regex = re.compile(
        r"https?://[^\s\"'<>\\]+?\.(?:png|jpe?g|gif|bmp)(?:\?[^\s\"'<>\\]*)?",
        re.IGNORECASE,
    )
    for m in regex.findall(html):
        found.add(m)

    filtered = set()
    for u in found:
        path = urlparse(u).path.lower()
        if any(path.endswith(ext) for ext in ALLOWED_EXTS):
            filtered.add(u)
    return filtered


def extract_page_links(base_url: str, html: str) -> set[str]:
    soup = BeautifulSoup(html, "html.parser")
    links: set[str] = set()

    for a in soup.find_all("a"):
        href = a.get("href")
        u = normalize_url(base_url, href) if href else None
        if u:
            links.add(u)

    for link in soup.find_all("link"):
        rel = link.get("rel") or []
        if isinstance(rel, str):
            rel = [rel]
        rel = [r.lower() for r in rel]
        if any(r in ("next", "prev", "canonical") for r in rel):
            href = link.get("href")
            u = normalize_url(base_url, href) if href else None
            if u:
                links.add(u)

    return links


# -------- Fetch backends --------

def fetch_with_requests(session: requests.Session, url: str, timeout: int, referer: str | None = None):
    headers = dict(BASE_HEADERS)
    if referer:
        headers["Referer"] = referer
    try:
        r = session.get(url, headers=headers, timeout=timeout)
        return r.status_code, r.headers, r.content
    except requests.RequestException as ex:
        eprint(f"[fetch:req] failed: {url} ({ex})")
        return None, {}, b""


def fetch_with_curl(url: str, timeout: int, referer: str | None = None):
    # curl -L follows redirects; -s silent; --max-time seconds; -A user agent
    cmd = ["curl", "-L", "-sS", "--max-time", str(timeout), "-A", UA]
    cmd += ["-H", f"Accept: {BASE_HEADERS['Accept']}"]
    cmd += ["-H", f"Accept-Language: {BASE_HEADERS['Accept-Language']}"]
    cmd += ["-H", "Upgrade-Insecure-Requests: 1"]
    if referer:
        cmd += ["-e", referer]
    cmd += [url]

    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        if p.returncode != 0:
            eprint(f"[fetch:curl] failed rc={p.returncode}: {url}")
            if p.stderr:
                eprint(p.stderr.decode("utf-8", errors="ignore").strip())
            return None, {}, b""
        # curl doesn't give us headers here; for extension we can fallback to URL path
        return 200, {}, p.stdout
    except FileNotFoundError:
        eprint("[fetch:curl] curl not found. Install: sudo apt install curl")
        return None, {}, b""


def fetch(url: str, session: requests.Session, timeout: int, referer: str | None = None):
    status, headers, body = fetch_with_requests(session, url, timeout, referer=referer)
    if status in (403, 429) or status is None:
        # rawpixel often blocks requests TLS fingerprint; curl usually passes
        status2, headers2, body2 = fetch_with_curl(url, timeout, referer=referer)
        if status2 == 200 and body2:
            return 200, headers2, body2
        return status, headers, body
    return status, headers, body


def download_image(session: requests.Session, img_url: str, outdir: str, seen_hashes: set[str], timeout: int, referer: str):
    status, headers, data = fetch(img_url, session, timeout, referer=referer)
    if status != 200 or not data:
        eprint(f"[download] status {status}: {img_url}")
        return False

    ext = guess_ext(img_url, headers.get("Content-Type"))
    if not ext:
        # fallback by URL path
        path = urlparse(img_url).path.lower()
        if not any(path.endswith(x) for x in ALLOWED_EXTS):
            return False
        ext = os.path.splitext(path)[1] or ".img"

    h = content_hash(data)
    if h in seen_hashes:
        return False
    seen_hashes.add(h)

    base = os.path.basename(urlparse(img_url).path) or "image"
    base = safe_filename(base)
    if not base.lower().endswith(ext):
        base = base + ext

    filename = f"{os.path.splitext(base)[0]}_{h}{ext}"
    path = os.path.join(outdir, filename)

    try:
        with open(path, "wb") as f:
            f.write(data)
    except OSError as ex:
        eprint(f"[download] cannot write {path} ({ex})")
        return False

    print(f"[ok] {img_url} -> {path}")
    return True


def crawl(start_url: str, recursive: bool, max_depth: int, outdir: str, timeout: int, delay: float) -> int:
    ensure_dir(outdir)

    session = requests.Session()

    visited_pages: set[str] = set()
    seen_img_urls: set[str] = set()
    seen_hashes: set[str] = set()
    queue: list[tuple[str, int]] = [(start_url, 0)]

    downloaded = 0

    while queue:
        page_url, depth = queue.pop(0)
        if page_url in visited_pages:
            continue
        visited_pages.add(page_url)

        if recursive and depth > max_depth:
            continue

        status, _headers, body = fetch(page_url, session, timeout, referer="https://www.rawpixel.com/")
        if status != 200 or not body:
            eprint(f"[fetch] status {status}: {page_url}")
            continue

        # decode html
        try:
            html = body.decode("utf-8", errors="ignore")
        except Exception:
            html = str(body)

        time.sleep(delay)

        imgs = extract_image_urls(page_url, html)
        for img_url in imgs:
            if img_url in seen_img_urls:
                continue
            seen_img_urls.add(img_url)
            if download_image(session, img_url, outdir, seen_hashes, timeout, referer=page_url):
                downloaded += 1
            time.sleep(delay)

        if recursive and depth < max_depth:
            links = extract_page_links(page_url, html)
            for link in links:
                scheme = urlparse(link).scheme.lower()
                if scheme not in ("http", "https"):
                    continue
                if not same_host(start_url, link):
                    continue
                if link not in visited_pages:
                    queue.append((link, depth + 1))

    return downloaded


def main():
    parser = argparse.ArgumentParser(prog="spider")
    parser.add_argument("-r", action="store_true", help="recursively download images from linked pages")
    parser.add_argument("-l", type=int, default=DEFAULT_DEPTH, help="max recursion depth (default: 5)")
    parser.add_argument("-p", type=str, default=DEFAULT_OUTDIR, help="output directory (default: ./data)")
    parser.add_argument("url", type=str, help="start URL")
    args = parser.parse_args()

    url = args.url.strip()
    if not url.startswith(("http://", "https://")):
        eprint("URL must start with http:// or https://")
        sys.exit(1)

    downloaded = crawl(
        start_url=url,
        recursive=args.r,
        max_depth=args.l,
        outdir=args.p,
        timeout=DEFAULT_TIMEOUT,
        delay=DEFAULT_DELAY,
    )
    print(f"\nDone. Downloaded: {downloaded} image(s) into {os.path.abspath(args.p)}")


if __name__ == "__main__":
    main()

