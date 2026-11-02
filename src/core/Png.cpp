#include "Png.h"

#include <zlib.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace xs {
namespace {

void put32(std::string& out, uint32_t v) {
    out += static_cast<char>(v >> 24);
    out += static_cast<char>(v >> 16);
    out += static_cast<char>(v >> 8);
    out += static_cast<char>(v);
}

void chunk(std::string& out, const char* type, const std::string& data) {
    put32(out, static_cast<uint32_t>(data.size()));
    const size_t start = out.size();
    out.append(type, 4);
    out += data;
    put32(out, static_cast<uint32_t>(
                   crc32(0, reinterpret_cast<const Bytef*>(out.data() + start), static_cast<uInt>(out.size() - start))));
}

}  // namespace

bool writePng(const std::string& path, const Canvas& c, std::string& error) {
    std::string raw;
    raw.reserve(static_cast<size_t>(c.width() + 1) * c.height());
    for (int y = 0; y < c.height(); ++y) {
        raw += '\0';  // filter: none
        raw.append(reinterpret_cast<const char*>(c.row(y)), c.width());
    }
    uLongf zlen = compressBound(static_cast<uLong>(raw.size()));
    std::string z(zlen, '\0');
    if (compress2(reinterpret_cast<Bytef*>(z.data()), &zlen, reinterpret_cast<const Bytef*>(raw.data()),
                  static_cast<uLong>(raw.size()), 6) != Z_OK) {
        error = "compression failed";
        return false;
    }
    z.resize(zlen);

    std::string ihdr;
    put32(ihdr, static_cast<uint32_t>(c.width()));
    put32(ihdr, static_cast<uint32_t>(c.height()));
    ihdr += std::string("\x08\x00\x00\x00\x00", 5);  // 8-bit, grayscale, deflate, no filter, no interlace

    std::string png("\x89PNG\r\n\x1a\n", 8);
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", "");

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        error = "can't write " + path + ": " + std::strerror(errno);
        return false;
    }
    const bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
    if (std::fclose(f) != 0 || !ok) {
        error = "can't write " + path;
        return false;
    }
    return true;
}

}  // namespace xs
