#include <exiv2/exiv2.hpp>

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

static std::string human_size(uintmax_t bytes) {
    const char* units[] = {"B","KB","MB","GB","TB"};
    double v = (double)bytes;
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
    std::ostringstream oss;
    if (i == 0) oss << (uintmax_t)v << " " << units[i];
    else oss << std::fixed << std::setprecision(1) << v << " " << units[i];
    return oss.str();
}

static void print_file_info(const fs::path& p) {
    std::cout << "=== " << p.string() << "\n";
    try {
        if (!fs::exists(p)) {
            std::cout << "  !! file not found\n\n";
            return;
        }
        auto sz = fs::file_size(p);
        std::cout << "- Size: " << human_size(sz) << "\n";
    } catch (...) {
        // ignore
    }
}

static void dump_meta(const fs::path& p) {
    try {
        auto image = Exiv2::ImageFactory::open(p.string());
        if (!image.get()) {
            std::cout << "  !! cannot open as image\n\n";
            return;
        }
        image->readMetadata();

        const auto& exif = image->exifData();
        const auto& xmp  = image->xmpData();
        const auto& iptc = image->iptcData();

        // Prefer EXIF dates
        auto print_date = [&](const char* key) -> bool {
            auto it = exif.findKey(Exiv2::ExifKey(key));
            if (it != exif.end()) {
                std::cout << "- EXIF date: " << it->toString() << " (" << key << ")\n";
                return true;
            }
            return false;
        };
        if (!(print_date("Exif.Photo.DateTimeOriginal") ||
              print_date("Exif.Photo.DateTimeDigitized") ||
              print_date("Exif.Image.DateTime"))) {
            std::cout << "- EXIF date: (not found)\n";
        }

        std::cout << "\n[EXIF]\n";
        if (exif.empty()) std::cout << "(no EXIF)\n";
        for (const auto& md : exif) {
            std::cout << md.key() << ": " << md.toString() << "\n";
        }

        std::cout << "\n[XMP]\n";
        if (xmp.empty()) std::cout << "(no XMP)\n";
        for (const auto& md : xmp) {
            std::cout << md.key() << ": " << md.toString() << "\n";
        }

        std::cout << "\n[IPTC]\n";
        if (iptc.empty()) std::cout << "(no IPTC)\n";
        for (const auto& md : iptc) {
            std::cout << md.key() << ": " << md.toString() << "\n";
        }

        std::cout << "\n";
    } catch (const Exiv2::Error& e) {
        std::cout << "  !! metadata read error: " << e.what() << "\n\n";
    } catch (const std::exception& e) {
        std::cout << "  !! error: " << e.what() << "\n\n";
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: ./scorpion FILE1 [FILE2 ...]\n";
        return 1;
    }

    for (int i = 1; i < argc; ++i) {
        fs::path p = argv[i];
        print_file_info(p);
        dump_meta(p);
    }
    return 0;
}

