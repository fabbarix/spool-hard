#pragma once
#include <Arduino.h>
#include <functional>

/**
 * Minimal ZIP central-directory reader. Finds entries by name and exposes
 * their (local-header offset, compressed size, uncompressed size, method).
 *
 * The caller is responsible for seeking into the underlying stream to read
 * the actual bytes. This keeps the reader agnostic to the transport (FTP,
 * HTTP, file) — just feed it the End-of-Central-Directory section and the
 * Central Directory bytes.
 *
 * Supports:
 *   - Stored entries (method=0) — the common case for Bambu's gcode payload
 *     inside a 3MF, where compressing-then-compressing-again would be silly.
 *     This parser exposes `method` on every entry so callers can dispatch;
 *     the console's analyseRemote() path rejects non-stored gcode entries
 *     with a clear error because the ESP32 has no inline inflate.
 *
 * Does NOT support:
 *   - Deflate entries (method=8) — would need a streaming inflate (miniz /
 *     uzlib). Not blocking for the shipped analysis flow: Bambu has always
 *     stored the plate gcode uncompressed. Add if a future 3MF variant or
 *     non-Bambu source ships deflated gcode.
 *   - ZIP64 (files > 4 GB)
 *   - Encrypted entries
 *   - Split archives
 */
class ZipReader {
public:
    struct Entry {
        String   name;
        uint16_t method          = 0;    // 0=stored, 8=deflate
        uint32_t compressed_size = 0;
        uint32_t uncompressed_size = 0;
        uint32_t local_header_offset = 0;
        // Data offset into the archive is local_header_offset + 30 +
        // filename_length + extra_length (computed once the local header
        // is read). Caller fills this via `resolveDataOffset()`.
        uint32_t data_offset = 0;
        uint16_t name_length_in_local = 0;
        uint16_t extra_length_in_local = 0;
    };

    /// Parse the End-of-Central-Directory record (last ~22 bytes of the
    /// archive) from `eocd`. Fills `cd_offset` and `cd_size` on success.
    static bool parseEOCD(const uint8_t* eocd, size_t len,
                          uint32_t& cd_offset, uint32_t& cd_size,
                          uint16_t& entry_count);

    /// Parse the central directory bytes into a list of entries.
    static std::vector<Entry> parseCentralDirectory(
        const uint8_t* cd, size_t len, uint16_t expected_count);

    /// Given the 30-byte (minimum) local-file-header, extract the data
    /// offset from the start of the archive.
    static bool parseLocalHeader(const uint8_t* local_hdr, size_t len,
                                 uint16_t& filename_len, uint16_t& extra_len);
};
