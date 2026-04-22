#include "zip_reader.h"

namespace {
inline uint16_t rd16(const uint8_t* p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
inline uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
constexpr uint32_t EOCD_SIG         = 0x06054b50;
constexpr uint32_t CD_SIG           = 0x02014b50;
constexpr uint32_t LOCAL_HDR_SIG    = 0x04034b50;
}

bool ZipReader::parseEOCD(const uint8_t* eocd, size_t len,
                          uint32_t& cd_offset, uint32_t& cd_size,
                          uint16_t& entry_count) {
    // EOCD record starts with signature PK\5\6 (0x06054b50).
    // Minimum length is 22 bytes; a ZIP comment may follow.
    if (len < 22) return false;
    // Search backward for the signature — the comment is variable length.
    for (int i = (int)len - 22; i >= 0; --i) {
        if (rd32(eocd + i) == EOCD_SIG) {
            entry_count = rd16(eocd + i + 10);
            cd_size     = rd32(eocd + i + 12);
            cd_offset   = rd32(eocd + i + 16);
            return true;
        }
    }
    return false;
}

std::vector<ZipReader::Entry> ZipReader::parseCentralDirectory(
    const uint8_t* cd, size_t len, uint16_t expected_count) {

    std::vector<Entry> out;
    out.reserve(expected_count);
    size_t p = 0;
    while (p + 46 <= len) {
        if (rd32(cd + p) != CD_SIG) break;
        Entry e;
        e.method            = rd16(cd + p + 10);
        e.compressed_size   = rd32(cd + p + 20);
        e.uncompressed_size = rd32(cd + p + 24);
        uint16_t name_len   = rd16(cd + p + 28);
        uint16_t extra_len  = rd16(cd + p + 30);
        uint16_t comment_len= rd16(cd + p + 32);
        e.local_header_offset = rd32(cd + p + 42);

        size_t name_pos = p + 46;
        if (name_pos + name_len > len) break;
        e.name.reserve(name_len);
        for (size_t i = 0; i < name_len; ++i) e.name += (char)cd[name_pos + i];

        out.push_back(std::move(e));
        p = name_pos + name_len + extra_len + comment_len;
    }
    return out;
}

bool ZipReader::parseLocalHeader(const uint8_t* hdr, size_t len,
                                 uint16_t& filename_len, uint16_t& extra_len) {
    if (len < 30) return false;
    if (rd32(hdr) != LOCAL_HDR_SIG) return false;
    filename_len = rd16(hdr + 26);
    extra_len    = rd16(hdr + 28);
    return true;
}
