// spider.c - Arachnida Spider (C, libcurl)
// Build: gcc -std=c17 -O2 -Wall -Wextra spider.c -lcurl -o spider

#define _POSIX_C_SOURCE 200809L

#include <curl/curl.h>
#include <string.h>   // strtok_r
#include <strings.h>  // strcasecmp

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <regex.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* ============================ small helpers ============================ */

static void die(const char* msg) {
    fprintf(stderr, "Error: %s\n", msg);
    exit(1);
}

static char* xstrdup(const char* s) {
    size_t n = strlen(s);
    char* p = (char*)malloc(n + 1);
    if (!p) die("malloc failed");
    memcpy(p, s, n + 1);
    return p;
}

static bool starts_with_ci(const char* s, const char* pref) {
    while (*pref && *s) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*pref)) return false;
        s++; pref++;
    }
    return *pref == '\0';
}

static void rtrim(char* s) {
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        s[n - 1] = '\0';
        n--;
    }
}

static char* ltrim(char* s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

static void safe_filename(char* s) {
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (!(isalnum(c) || c == '.' || c == '_' || c == '-')) *s = '_';
    }
}

/* mkdir -p */
static int mkdir_p(const char* path) {
    char tmp[PATH_MAX];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) return -1;

    strcpy(tmp, path);
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char* p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

/* ============================ URL parsing / joining ============================ */

typedef struct {
    char scheme[8];   // "http" or "https"
    char host[256];   // "example.com" or "example.com:8080"
    char path[PATH_MAX]; // "/a/b/index.html"
} UrlParts;

static bool parse_url(const char* url, UrlParts* out) {
    // Very small parser: ^(https?)://([^/]+)(/.*)?$
    regex_t re;
    const char* pat = "^[[:space:]]*(https?)://([^/]+)(/[^[:space:]]*)?[[:space:]]*$";
    if (regcomp(&re, pat, REG_EXTENDED | REG_ICASE) != 0) return false;

    regmatch_t m[4];
    int rc = regexec(&re, url, 4, m, 0);
    regfree(&re);
    if (rc != 0) return false;

    int s1 = m[1].rm_so, e1 = m[1].rm_eo;
    int s2 = m[2].rm_so, e2 = m[2].rm_eo;
    int s3 = m[3].rm_so, e3 = m[3].rm_eo;

    int n1 = e1 - s1, n2 = e2 - s2;

    if (n1 <= 0 || n1 >= (int)sizeof(out->scheme)) return false;
    if (n2 <= 0 || n2 >= (int)sizeof(out->host)) return false;

    strncpy(out->scheme, url + s1, (size_t)n1);
    out->scheme[n1] = '\0';
    for (char* p = out->scheme; *p; ++p) *p = (char)tolower((unsigned char)*p);

    strncpy(out->host, url + s2, (size_t)n2);
    out->host[n2] = '\0';

    if (s3 >= 0 && e3 > s3) {
        int n3 = e3 - s3;
        if (n3 <= 0 || n3 >= (int)sizeof(out->path)) return false;
        strncpy(out->path, url + s3, (size_t)n3);
        out->path[n3] = '\0';
    } else {
        strcpy(out->path, "/");
    }
    if (out->path[0] == '\0') strcpy(out->path, "/");
    return true;
}

static void strip_qf(char* s) {
    char* q = strpbrk(s, "?#");
    if (q) *q = '\0';
}

static void base_dir_from_path(const char* path, char* out, size_t outsz) {
    // "/a/b/index.html" -> "/a/b/"
    char tmp[PATH_MAX];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    strip_qf(tmp);

    size_t n = strlen(tmp);
    if (n == 0) { strncpy(out, "/", outsz); out[outsz-1]='\0'; return; }
    if (tmp[n - 1] == '/') { strncpy(out, tmp, outsz); out[outsz-1]='\0'; return; }

    char* last = strrchr(tmp, '/');
    if (!last) { strncpy(out, "/", outsz); out[outsz-1]='\0'; return; }
    *(last + 1) = '\0';
    strncpy(out, tmp, outsz);
    out[outsz - 1] = '\0';
}

static void normalize_rel_path(const char* in, char* out, size_t outsz) {
    // Normalize /./ and /../ segments (basic)
    char tmp[PATH_MAX];
    strncpy(tmp, in, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    // split by '/'
    const char* segs[1024];
    int segc = 0;

    char* save = NULL;
    for (char* tok = strtok_r(tmp, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        if (strcmp(tok, ".") == 0 || strcmp(tok, "") == 0) continue;
        if (strcmp(tok, "..") == 0) {
            if (segc > 0) segc--;
            continue;
        }
        if (segc < 1024) segs[segc++] = tok;
    }

    out[0] = '/';
    out[1] = '\0';
    for (int i = 0; i < segc; ++i) {
        strncat(out, segs[i], outsz - strlen(out) - 1);
        if (i + 1 < segc) strncat(out, "/", outsz - strlen(out) - 1);
    }
    if (segc == 0) strncpy(out, "/", outsz);
    out[outsz - 1] = '\0';
}

static bool join_url(const UrlParts* base, const char* href, char* out, size_t outsz) {
    char buf[PATH_MAX];
    strncpy(buf, href, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    rtrim(buf);
    char* h = ltrim(buf);
    if (*h == '\0') return false;

    // ignore anchors / js / mailto
    if (*h == '#') return false;
    if (starts_with_ci(h, "javascript:") || starts_with_ci(h, "mailto:")) return false;

    if (starts_with_ci(h, "http://") || starts_with_ci(h, "https://")) {
        strncpy(out, h, outsz);
        out[outsz - 1] = '\0';
        return true;
    }

    // scheme-relative
    if (h[0] == '/' && h[1] == '/') {
        snprintf(out, outsz, "%s:%s", base->scheme, h);
        return true;
    }

    // absolute path
    if (h[0] == '/') {
        snprintf(out, outsz, "%s://%s%s", base->scheme, base->host, h);
        return true;
    }

    // relative path
    char dir[PATH_MAX];
    base_dir_from_path(base->path, dir, sizeof(dir));
    snprintf(buf, sizeof(buf), "%s%s", dir, h);

    char norm[PATH_MAX];
    normalize_rel_path(buf, norm, sizeof(norm));

    snprintf(out, outsz, "%s://%s%s", base->scheme, base->host, norm);
    return true;
}

/* ============================ image helpers ============================ */

static bool is_image_url(const char* url) {
    char u[PATH_MAX];
    strncpy(u, url, sizeof(u) - 1);
    u[sizeof(u) - 1] = '\0';

    // lowercase
    for (char* p = u; *p; ++p) *p = (char)tolower((unsigned char)*p);

    strip_qf(u);
    const char* exts[] = {".jpg", ".jpeg", ".png", ".gif", ".bmp"};
    for (size_t i = 0; i < sizeof(exts)/sizeof(exts[0]); ++i) {
        size_t eu = strlen(u), ee = strlen(exts[i]);
        if (eu >= ee && strcmp(u + (eu - ee), exts[i]) == 0) return true;
    }
    return false;
}

static void filename_from_url(const char* url, char* out, size_t outsz) {
    char tmp[PATH_MAX];
    strncpy(tmp, url, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    strip_qf(tmp);

    const char* last = strrchr(tmp, '/');
    const char* name = (last && *(last + 1)) ? (last + 1) : "image.bin";

    strncpy(out, name, outsz);
    out[outsz - 1] = '\0';
    safe_filename(out);
}

/* ============================ simple sets (linear) ============================ */

typedef struct {
    char** items;
    size_t len;
    size_t cap;
} StrSet;

static void set_init(StrSet* s) { s->items = NULL; s->len = 0; s->cap = 0; }

static void set_free(StrSet* s) {
    for (size_t i = 0; i < s->len; ++i) free(s->items[i]);
    free(s->items);
    s->items = NULL; s->len = 0; s->cap = 0;
}

static bool set_has(const StrSet* s, const char* v) {
    for (size_t i = 0; i < s->len; ++i) {
        if (strcmp(s->items[i], v) == 0) return true;
    }
    return false;
}

static void set_add(StrSet* s, const char* v) {
    if (set_has(s, v)) return;
    if (s->len == s->cap) {
        size_t ncap = s->cap ? s->cap * 2 : 64;
        char** nitems = (char**)realloc(s->items, ncap * sizeof(char*));
        if (!nitems) die("realloc failed");
        s->items = nitems;
        s->cap = ncap;
    }
    s->items[s->len++] = xstrdup(v);
}

/* ============================ libcurl IO ============================ */

typedef struct {
    char* data;
    size_t size;
} MemBuf;

static size_t write_mem_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t total = size * nmemb;
    MemBuf* m = (MemBuf*)userdata;
    char* nd = (char*)realloc(m->data, m->size + total + 1);
    if (!nd) return 0;
    m->data = nd;
    memcpy(m->data + m->size, ptr, total);
    m->size += total;
    m->data[m->size] = '\0';
    return total;
}

static bool http_get_text(const char* url, const char* user_agent, char** out_html) {
    *out_html = NULL;

    CURL* c = curl_easy_init();
    if (!c) return false;

    MemBuf m = {0};
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, user_agent);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_mem_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);

    CURLcode res = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);

    if (res != CURLE_OK || code < 200 || code >= 300) {
        free(m.data);
        return false;
    }
    *out_html = m.data;
    return true;
}

static size_t write_file_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    FILE* f = (FILE*)userdata;
    return fwrite(ptr, size, nmemb, f);
}

static bool http_download_file(const char* url, const char* user_agent, const char* out_path) {
    FILE* f = fopen(out_path, "wb");
    if (!f) return false;

    CURL* c = curl_easy_init();
    if (!c) { fclose(f); return false; }

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, user_agent);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_file_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);

    CURLcode res = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);

    curl_easy_cleanup(c);
    fclose(f);

    if (res != CURLE_OK || code < 200 || code >= 300) {
        remove(out_path); // cleanup partial
        return false;
    }
    return true;
}

/* ============================ HTML extraction via regex ============================ */

typedef struct {
    char** items;
    size_t len;
    size_t cap;
} StrVec;

static void vec_init(StrVec* v) { v->items = NULL; v->len = 0; v->cap = 0; }
static void vec_free(StrVec* v) {
    for (size_t i = 0; i < v->len; ++i) free(v->items[i]);
    free(v->items);
    v->items = NULL; v->len = 0; v->cap = 0;
}
static void vec_push(StrVec* v, const char* s) {
    if (v->len == v->cap) {
        size_t ncap = v->cap ? v->cap * 2 : 64;
        char** n = (char**)realloc(v->items, ncap * sizeof(char*));
        if (!n) die("realloc failed");
        v->items = n; v->cap = ncap;
    }
    v->items[v->len++] = xstrdup(s);
}

static void extract_tag_attrs(const char* html, const char* tagname, const char* attr, StrVec* out) {
    // Find <tag ...> blocks then within each, find attr=... and capture value.
    // This is *not* a full parser; it's acceptable for the project.
    char tagpat[256];
    snprintf(tagpat, sizeof(tagpat), "<[[:space:]]*%s\\b[^>]*>", tagname);

    regex_t tagre;
    if (regcomp(&tagre, tagpat, REG_EXTENDED | REG_ICASE) != 0) return;

    // attr regex: attr\s*=\s*("...")|('...')|([^\s>]+)
    char attrpat[256];
    snprintf(attrpat, sizeof(attrpat),
             "%s[[:space:]]*=[[:space:]]*(\"([^\"]*)\"|'([^']*)'|([^[:space:]>]+))",
             attr);
    regex_t attrre;
    if (regcomp(&attrre, attrpat, REG_EXTENDED | REG_ICASE) != 0) {
        regfree(&tagre);
        return;
    }

    const char* p = html;
    regmatch_t mtag[1];
    while (regexec(&tagre, p, 1, mtag, 0) == 0) {
        int so = mtag[0].rm_so;
        int eo = mtag[0].rm_eo;
        if (so < 0 || eo <= so) break;

        size_t tlen = (size_t)(eo - so);
        char* tag = (char*)malloc(tlen + 1);
        if (!tag) break;
        memcpy(tag, p + so, tlen);
        tag[tlen] = '\0';

        regmatch_t ma[5];
        if (regexec(&attrre, tag, 5, ma, 0) == 0) {
            const char* val = NULL;
            int vso = -1, veo = -1;
            // group2 or group3 or group4 holds value
            if (ma[2].rm_so >= 0) { vso = ma[2].rm_so; veo = ma[2].rm_eo; val = tag; }
            else if (ma[3].rm_so >= 0) { vso = ma[3].rm_so; veo = ma[3].rm_eo; val = tag; }
            else if (ma[4].rm_so >= 0) { vso = ma[4].rm_so; veo = ma[4].rm_eo; val = tag; }

            if (val && vso >= 0 && veo > vso) {
                size_t n = (size_t)(veo - vso);
                char* s = (char*)malloc(n + 1);
                if (s) {
                    memcpy(s, val + vso, n);
                    s[n] = '\0';
                    char* ss = ltrim(s);
                    rtrim(ss);
                    if (*ss) vec_push(out, ss);
                    free(s);
                }
            }
        }

        free(tag);
        p = p + eo; // continue after this tag
    }

    regfree(&attrre);
    regfree(&tagre);
}

/* ============================ Spider core ============================ */

typedef struct {
    bool recursive;
    int max_depth; // used when recursive
    char out_dir[PATH_MAX];
} Options;

typedef struct {
    Options opt;
    const char* user_agent;
    StrSet visited_pages;
    StrSet downloaded_images;
} Spider;

static void spider_init(Spider* s, Options opt) {
    s->opt = opt;
    s->user_agent = "Mozilla/5.0 (X11; Linux x86_64) ArachnidaSpider/1.0";
    set_init(&s->visited_pages);
    set_init(&s->downloaded_images);
}

static void spider_free(Spider* s) {
    set_free(&s->visited_pages);
    set_free(&s->downloaded_images);
}

static void spider_crawl(Spider* s, const char* url, int depth_left) {
    UrlParts base;
    if (!parse_url(url, &base)) return;

    if (set_has(&s->visited_pages, url)) return;
    set_add(&s->visited_pages, url);

    fprintf(stderr, "[PAGE] %s (depth_left=%d)\n", url, depth_left);

    char* html = NULL;
    if (!http_get_text(url, s->user_agent, &html)) {
        fprintf(stderr, "  !! failed to fetch\n");
        return;
    }

    // Images
    StrVec srcs;
    vec_init(&srcs);
    extract_tag_attrs(html, "img", "src", &srcs);

    for (size_t i = 0; i < srcs.len; ++i) {
        char imgUrl[PATH_MAX];
        if (!join_url(&base, srcs.items[i], imgUrl, sizeof(imgUrl))) continue;
        if (!is_image_url(imgUrl)) continue;

        if (set_has(&s->downloaded_images, imgUrl)) continue;
        set_add(&s->downloaded_images, imgUrl);

        char fname[256];
        filename_from_url(imgUrl, fname, sizeof(fname));

        char outpath[PATH_MAX];
        snprintf(outpath, sizeof(outpath), "%s/%s", s->opt.out_dir, fname);

        bool ok = http_download_file(imgUrl, s->user_agent, outpath);
        if (ok) fprintf(stderr, "  [IMG] %s -> %s\n", imgUrl, outpath);
        else fprintf(stderr, "  !! failed img: %s\n", imgUrl);
    }
    vec_free(&srcs);

    // Links recurse
    if (s->opt.recursive && depth_left > 0) {
        StrVec hrefs;
        vec_init(&hrefs);
        extract_tag_attrs(html, "a", "href", &hrefs);

        for (size_t i = 0; i < hrefs.len; ++i) {
            char nextUrl[PATH_MAX];
            if (!join_url(&base, hrefs.items[i], nextUrl, sizeof(nextUrl))) continue;

            // stay on same host
            UrlParts np;
            if (!parse_url(nextUrl, &np)) continue;
            if (strcasecmp(np.host, base.host) != 0) continue;

            spider_crawl(s, nextUrl, depth_left - 1);
        }
        vec_free(&hrefs);
    }

    free(html);
}

/* ============================ CLI ============================ */

static void usage(void) {
    printf("Usage: ./spider [-r] [-l N] [-p PATH] URL\n"
           "  -r        recursive crawl\n"
           "  -l N      max depth (only with -r), default 5\n"
           "  -p PATH   output directory (default ./data/)\n");
}

static bool is_uint(const char* s) {
    if (!s || !*s) return false;
    for (; *s; ++s) if (!isdigit((unsigned char)*s)) return false;
    return true;
}

int main(int argc, char** argv) {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    Options opt;
    opt.recursive = false;
    opt.max_depth = 0;
    strncpy(opt.out_dir, "./data", sizeof(opt.out_dir));
    opt.out_dir[sizeof(opt.out_dir) - 1] = '\0';

    const char* url = NULL;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (strcmp(a, "-r") == 0) {
            opt.recursive = true;
        } else if (strcmp(a, "-l") == 0) {
            if (i + 1 >= argc) { usage(); return 1; }
            const char* n = argv[++i];
            if (!is_uint(n)) { fprintf(stderr, "Invalid -l value: %s\n", n); return 1; }
            opt.max_depth = atoi(n);
        } else if (strcmp(a, "-p") == 0) {
            if (i + 1 >= argc) { usage(); return 1; }
            strncpy(opt.out_dir, argv[++i], sizeof(opt.out_dir));
            opt.out_dir[sizeof(opt.out_dir) - 1] = '\0';
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage();
            return 0;
        } else {
            url = a;
        }
    }

    if (!url) { usage(); return 1; }

    UrlParts tmp;
    if (!parse_url(url, &tmp)) {
        fprintf(stderr, "Invalid URL (only http/https supported): %s\n", url);
        return 1;
    }

    if (opt.recursive && opt.max_depth == 0) opt.max_depth = 5;

    if (mkdir_p(opt.out_dir) != 0) {
        fprintf(stderr, "Failed to create output dir: %s\n", opt.out_dir);
        return 1;
    }

    Spider s;
    spider_init(&s, opt);

    int depth = opt.recursive ? opt.max_depth : 0;
    spider_crawl(&s, url, depth);

    fprintf(stderr, "\nDone.\nVisited pages: %zu\nDownloaded images: %zu\n",
            s.visited_pages.len, s.downloaded_images.len);

    spider_free(&s);
    curl_global_cleanup();
    return 0;
}

