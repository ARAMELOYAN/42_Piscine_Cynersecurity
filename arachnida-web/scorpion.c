#include <gexiv2/gexiv2.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char* pick_exif_date(GExiv2Metadata* md) {
    // Prefer EXIF DateTimeOriginal -> Digitized -> Image DateTime
    if (gexiv2_metadata_has_exif(md, "Exif.Photo.DateTimeOriginal"))
        return "Exif.Photo.DateTimeOriginal";
    if (gexiv2_metadata_has_exif(md, "Exif.Photo.DateTimeDigitized"))
        return "Exif.Photo.DateTimeDigitized";
    if (gexiv2_metadata_has_exif(md, "Exif.Image.DateTime"))
        return "Exif.Image.DateTime";
    return NULL;
}

static void print_file_info(const char* path) {
    struct stat st;
    printf("=== %s\n", path);
    if (stat(path, &st) != 0) {
        printf("  !! file error: %s\n\n", strerror(errno));
        return;
    }
    printf("- Size: %lld bytes\n", (long long)st.st_size);
}

static void dump_keys(GExiv2Metadata* md, const char* title, char** keys) {
    printf("\n[%s]\n", title);
    if (!keys || !keys[0]) {
        printf("(none)\n");
        return;
    }
    for (int i = 0; keys[i]; ++i) {
        GError* err = NULL;
        char* val = gexiv2_metadata_get_tag_string(md, keys[i], &err);
        if (err) {
            g_error_free(err);
            continue;
        }
        if (val) {
            printf("%s: %s\n", keys[i], val);
            g_free(val);
        }
    }
}

static void scorpion_one(const char* path) {
    print_file_info(path);

    GError* err = NULL;
    GExiv2Metadata* md = gexiv2_metadata_new();

    if (!gexiv2_metadata_open_path(md, path, &err)) {
        printf("  !! cannot read metadata: %s\n\n", err ? err->message : "unknown");
        if (err) g_error_free(err);
        g_object_unref(md);
        return;
    }

    const char* date_key = pick_exif_date(md);
    if (date_key) {
        GError* e2 = NULL;
        char* date_val = gexiv2_metadata_get_tag_string(md, date_key, &e2);
        if (!e2 && date_val) {
            printf("- EXIF date: %s (%s)\n", date_val, date_key);
            g_free(date_val);
        } else {
            printf("- EXIF date: (found key, but failed to read)\n");
            if (e2) g_error_free(e2);
        }
    } else {
        printf("- EXIF date: (not found)\n");
    }

    // Dump EXIF / XMP / IPTC keys
    char** exif_keys = gexiv2_metadata_get_exif_tags(md);
    char** xmp_keys  = gexiv2_metadata_get_xmp_tags(md);
    char** iptc_keys = gexiv2_metadata_get_iptc_tags(md);

    dump_keys(md, "EXIF", exif_keys);
    dump_keys(md, "XMP",  xmp_keys);
    dump_keys(md, "IPTC", iptc_keys);

    if (exif_keys) g_strfreev(exif_keys);
    if (xmp_keys)  g_strfreev(xmp_keys);
    if (iptc_keys) g_strfreev(iptc_keys);

    printf("\n");
    g_object_unref(md);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: ./scorpion_c FILE1 [FILE2 ...]\n");
        return 1;
    }
    for (int i = 1; i < argc; ++i) {
        scorpion_one(argv[i]);
    }
    return 0;
}

