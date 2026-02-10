#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <string>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

/* ===================== Utils ===================== */

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return (char)std::tolower(c); });
    return s;
}

static bool starts_with(const std::string& s, const std::string& pref) {
    return s.rfind(pref, 0) == 0;
}

static std::string trim(std::string s) {
    auto isws = [](unsigned char c){ return std::isspace(c); };
    while (!s.empty() && isws((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && isws((unsigned char)s.back())) s.pop_back();
    return s;
}

/* ===================== URL parsing / joining ===================== */

struct UrlParts {
    std::string scheme; // http or https
    std::string host;   // example.com:8080
    std::string path;   // /a/b/index.html  (may be empty -> "/")
};

static std::optional<UrlParts> parse_url(const std::string& url) {
    // Very small parser: scheme://host/path...
    // Accept http(s) only.
    static const std::regex re(R"(^\s*(https?)://([^/]+)(/.*)?\s*$)",
                               std::regex::icase);
    std::smatch m;
    if (!std::regex_match(url, m, re)) return std::nullopt;

    UrlParts p;
    p.scheme = to_lower(m[1].str());
    p.host   = m[2].str();
    p.path   = m[3].matched ? m[3].str() : "/";
    if (p.path.empty()) p.path = "/";
    return p;
}

static std::string url_base_dir(const UrlParts& base) {
    // Return base directory like /a/b/ from /a/b/index.html
    std::string path = base.path;
    if (path.empty()) return "/";
    // strip query/fragment if present (very minimal)
    auto qpos = path.find_first_of("?#");
    if (qpos != std::string::npos) path = path.substr(0, qpos);

    if (path.back() == '/') return path;

    auto slash = path.find_last_of('/');
    if (slash == std::string::npos) return "/";
    return path.substr(0, slash + 1);
}

static std::string normalize_url(const std::string& url) {
    // Minimal normalization: trim spaces.
    return trim(url);
}

static std::string join_url(const UrlParts& base, const std::string& href) {
    std::string h = normalize_url(href);
    if (h.empty()) return "";

    // ignore anchors / javascript / mailto
    std::string hl = to_lower(h);
    if (starts_with(hl, "javascript:") || starts_with(hl, "mailto:")) return "";
    if (h[0] == '#') return "";

    if (starts_with(hl, "http://") || starts_with(hl, "https://")) {
        return h;
    }

    // scheme-relative: //cdn.site/img.png
    if (starts_with(h, "//")) {
        return base.scheme + ":" + h;
    }

    // absolute path: /img/a.png
    if (starts_with(h, "/")) {
        return base.scheme + "://" + base.host + h;
    }

    // relative path: img/a.png or ../img/a.png
    std::string dir = url_base_dir(base);
    std::string combined = dir + h;

    // normalize ./ and ../ (simple, filesystem-like on URL path)
    std::vector<std::string> parts;
    std::string tmp;
    for (size_t i = 0; i < combined.size(); ++i) {
        char c = combined[i];
        if (c == '/') {
            if (!tmp.empty()) {
                parts.push_back(tmp);
                tmp.clear();
            } else {
                // keep empty segments implicit
            }
        } else {
            tmp.push_back(c);
        }
    }
    if (!tmp.empty()) parts.push_back(tmp);

    std::vector<std::string> norm;
    for (auto &seg : parts) {
        if (seg == "." || seg.empty()) continue;
        if (seg == "..") {
            if (!norm.empty()) norm.pop_back();
            continue;
        }
        norm.push_back(seg);
    }

    std::string out = "/";
    for (size_t i = 0; i < norm.size(); ++i) {
        out += norm[i];
        if (i + 1 < norm.size()) out += "/";
    }

    return base.scheme + "://" + base.host + out;
}

/* ===================== HTML extraction ===================== */

static std::vector<std::string> extract_attr_urls(const std::string& html, const std::string& attrName) {
    // Find attrName= "..." or '...' or unquoted
    // This is not a full HTML parser, but good enough for many pages.
    std::vector<std::string> out;
    std::string an = attrName;

    // Regex: attrName\s*=\s*("...")|('...')|([^\s>]+)
    // We'll search across entire HTML.
    std::regex re(
        an + "\\s*=\\s*(\"([^\"]*)\"|'([^']*)'|([^\\s>]+))",
        std::regex::icase
    );

    auto begin = std::sregex_iterator(html.begin(), html.end(), re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        std::smatch m = *it;
        std::string v;
        if (m[2].matched) v = m[2].str();
        else if (m[3].matched) v = m[3].str();
        else if (m[4].matched) v = m[4].str();
        v = trim(v);
        if (!v.empty()) out.push_back(v);
    }
    return out;
}

static std::vector<std::string> extract_img_srcs(const std::string& html) {
    // We only want src in <img ...> tags. We'll do a quick pass:
    std::vector<std::string> out;
    std::regex imgTag(R"(<\s*img\b[^>]*>)", std::regex::icase);
    auto begin = std::sregex_iterator(html.begin(), html.end(), imgTag);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        std::string tag = (*it).str();
        auto srcs = extract_attr_urls(tag, "src");
        for (auto &s : srcs) out.push_back(s);
    }
    return out;
}

static std::vector<std::string> extract_a_hrefs(const std::string& html) {
    std::vector<std::string> out;
    std::regex aTag(R"(<\s*a\b[^>]*>)", std::regex::icase);
    auto begin = std::sregex_iterator(html.begin(), html.end(), aTag);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        std::string tag = (*it).str();
        auto hrefs = extract_attr_urls(tag, "href");
        for (auto &h : hrefs) out.push_back(h);
    }
    return out;
}

/* ===================== Image filters ===================== */

static bool is_image_url(const std::string& url) {
    std::string u = to_lower(url);
    // strip query/fragment for extension check
    auto cut = u.find_first_of("?#");
    if (cut != std::string::npos) u = u.substr(0, cut);

    static const std::vector<std::string> exts = {
        ".jpg", ".jpeg", ".png", ".gif", ".bmp"
    };

    for (auto &e : exts) {
        if (u.size() >= e.size() && u.compare(u.size() - e.size(), e.size(), e) == 0)
            return true;
    }
    return false;
}

static std::string filename_from_url(const std::string& url) {
    std::string u = url;
    auto cut = u.find_first_of("?#");
    if (cut != std::string::npos) u = u.substr(0, cut);

    auto slash = u.find_last_of('/');
    if (slash == std::string::npos || slash + 1 >= u.size()) {
        // fallback
        return "image.bin";
    }
    std::string name = u.substr(slash + 1);
    if (name.empty()) name = "image.bin";

    // avoid weird characters in filenames
    for (char& c : name) {
        if (!(std::isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-' )) {
            c = '_';
        }
    }
    return name;
}

/* ===================== libcurl helpers ===================== */

static size_t write_to_string(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    auto* s = static_cast<std::string*>(userp);
    s->append(static_cast<char*>(contents), total);
    return total;
}

static size_t write_to_file(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    auto* ofs = static_cast<std::ofstream*>(userp);
    ofs->write(static_cast<char*>(contents), total);
    return total;
}

static bool http_get_text(const std::string& url, std::string& out, const std::string& user_agent) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);

    CURLcode res = curl_easy_perform(curl);

    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK && code >= 200 && code < 300);
}

static bool http_download_file(const std::string& url, const fs::path& out_path, const std::string& user_agent) {
    fs::create_directories(out_path.parent_path());

    std::ofstream ofs(out_path, std::ios::binary);
    if (!ofs) return false;

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ofs);

    CURLcode res = curl_easy_perform(curl);

    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    ofs.close();

    if (!(res == CURLE_OK && code >= 200 && code < 300)) {
        // cleanup partial file
        std::error_code ec;
        fs::remove(out_path, ec);
        return false;
    }
    return true;
}

/* ===================== Spider core ===================== */

struct Options {
    bool recursive = false;
    int max_depth = 0;          // used only if recursive
    fs::path out_dir = "./data";
};

struct Spider {
    Options opt;
    std::string user_agent = "Mozilla/5.0 (X11; Linux x86_64) ArachnidaSpider/1.0";

    std::unordered_set<std::string> visited_pages;
    std::unordered_set<std::string> downloaded_images;

    void crawl(const std::string& url, int depth_left) {
        if (url.empty()) return;

        auto partsOpt = parse_url(url);
        if (!partsOpt) return;

        // avoid revisiting
        if (visited_pages.find(url) != visited_pages.end()) return;
        visited_pages.insert(url);

        std::string html;
        std::cerr << "[PAGE] " << url << " (depth_left=" << depth_left << ")\n";

        if (!http_get_text(url, html, user_agent)) {
            std::cerr << "  !! failed to fetch\n";
            return;
        }

        // Extract and download images
        auto srcs = extract_img_srcs(html);
        for (auto &src : srcs) {
            std::string imgUrl = join_url(*partsOpt, src);
            if (imgUrl.empty()) continue;
            if (!is_image_url(imgUrl)) continue;

            if (downloaded_images.find(imgUrl) != downloaded_images.end()) continue;
            downloaded_images.insert(imgUrl);

            fs::path out_path = opt.out_dir / filename_from_url(imgUrl);
            bool ok = http_download_file(imgUrl, out_path, user_agent);
            if (ok) {
                std::cerr << "  [IMG] " << imgUrl << " -> " << out_path.string() << "\n";
            } else {
                std::cerr << "  !! failed img: " << imgUrl << "\n";
            }
        }

        // Recurse into links if enabled
        if (!opt.recursive) return;
        if (depth_left <= 0) return;

        auto hrefs = extract_a_hrefs(html);
        for (auto &href : hrefs) {
            std::string nextUrl = join_url(*partsOpt, href);
            if (nextUrl.empty()) continue;

            // Optional: stay on same host to avoid crawling entire web
            auto nextParts = parse_url(nextUrl);
            if (!nextParts) continue;
            if (to_lower(nextParts->host) != to_lower(partsOpt->host)) continue;

            crawl(nextUrl, depth_left - 1);
        }
    }
};

/* ===================== CLI parsing ===================== */

static void usage() {
    std::cout
        << "Usage: ./spider [-r] [-l N] [-p PATH] URL\n"
        << "  -r        recursive crawl\n"
        << "  -l N      max depth (only with -r). default 5\n"
        << "  -p PATH   output directory (default ./data/)\n";
}

static bool is_number(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (!std::isdigit((unsigned char)c)) return false;
    return true;
}

int main(int argc, char** argv) {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    Options opt;
    std::string url;

    // Simple manual parsing (fits project constraints)
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-r") {
            opt.recursive = true;
        } else if (a == "-l") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for -l\n";
                usage();
                return 1;
            }
            std::string n = argv[++i];
            if (!is_number(n)) {
                std::cerr << "Invalid depth for -l: " << n << "\n";
                return 1;
            }
            opt.max_depth = std::stoi(n);
        } else if (a == "-p") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for -p\n";
                usage();
                return 1;
            }
            opt.out_dir = argv[++i];
        } else if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else {
            // last argument as URL
            url = a;
        }
    }

    if (url.empty()) {
        usage();
        return 1;
    }

    // If -r is set and -l not provided => default depth 5
    if (opt.recursive && opt.max_depth == 0) {
        opt.max_depth = 5;
    }

    // Ensure output directory exists
    try {
        fs::create_directories(opt.out_dir);
    } catch (...) {
        std::cerr << "Failed to create output directory: " << opt.out_dir << "\n";
        return 1;
    }

    // Validate URL
    auto p = parse_url(url);
    if (!p) {
        std::cerr << "Invalid URL (only http/https supported): " << url << "\n";
        return 1;
    }

    Spider s;
    s.opt = opt;

    int depth = opt.recursive ? opt.max_depth : 0;
    s.crawl(url, depth);

    std::cerr << "\nDone.\n"
              << "Visited pages: " << s.visited_pages.size() << "\n"
              << "Downloaded images: " << s.downloaded_images.size() << "\n";

    curl_global_cleanup();
    return 0;
}

