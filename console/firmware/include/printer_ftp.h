#pragma once
#include <Arduino.h>
#include <IPAddress.h>
#include <WiFiClientSecure.h>
#include <functional>
#include <vector>

/**
 * Implicit-TLS FTP client targeting Bambu Lab printers.
 *
 *   - Control + data connection both TLS (implicit, :990).
 *   - Auth: username "bblp", password = printer access code.
 *   - Binary transfers only.
 *   - Passive mode only (servers initiate nothing).
 *
 * Typical use:
 *   PrinterFtp ftp;
 *   if (!ftp.connect(ip, access_code)) { ... }
 *   ftp.retrieveStream("/cache/job.3mf", [](const uint8_t* data, size_t n, size_t total, size_t idx) {
 *       // handle chunk
 *       return true;  // continue
 *   });
 *   ftp.quit();
 *
 * Limitations: no directory listing helper yet, no resume, no upload.
 */
class PrinterFtp {
public:
    using ChunkCb = std::function<bool(const uint8_t* data, size_t len, size_t total_so_far)>;
    using RangeCb = std::function<bool(const uint8_t* data, size_t len)>;

    // Debug tap: fires once per protocol-level event (TCP connect, TLS
    // handshake, banner, each control-channel command + response, PASV,
    // LIST / RETR, per-chunk data progress). `code` is the FTP response
    // code when meaningful, 0 otherwise. `text` is the server's response
    // text or a human-readable status string for non-FTP steps (e.g.
    // "tls handshake ok"). `elapsed_ms` is measured from setTraceCb time.
    using TraceCb = std::function<void(const char* step, int code,
                                       const String& text, uint32_t elapsed_ms)>;
    void setTraceCb(TraceCb cb) { _trace = std::move(cb); _traceStart = millis(); }

    PrinterFtp();
    ~PrinterFtp();

    // Connect to the printer's FTPS on `ip:port`, using `sni_hostname`
    // (typically the printer's serial, which matches the CN of its self-
    // signed cert) as the TLS SNI extension. Bambu's FTPS silently drops
    // post-banner commands when SNI doesn't match — same behaviour the
    // Rust SpoolHard reference sidesteps by wrapping an already-connected
    // socket in a TLS session with an explicit servername.
    bool connect(const IPAddress& ip, const String& access_code,
                 const String& sni_hostname, uint16_t port = 990);
    bool quit();
    bool isOpen() { return _control.connected(); }

    /// Read `length` bytes starting at `offset` of `path` from the server,
    /// calling `cb` with each chunk. Implemented with FTP REST + RETR when
    /// the server supports it.
    bool retrieveRange(const String& path, uint32_t offset, uint32_t length, RangeCb cb);

    /// Full-file retrieval streamed via callback. `cb` returns false to abort.
    bool retrieveStream(const String& path, ChunkCb cb);

    /// Fetch bytes [offset, offset+length) into a buffer. Caller pre-sizes.
    bool retrieveInto(const String& path, uint32_t offset, uint32_t length, uint8_t* dst);

    /// Query file size via FTP SIZE command. Returns -1 on error.
    int32_t size(const String& path);

    /// PASV + LIST on `path`. Raw CRLF-separated server lines go into
    /// `out` (one element per listing entry). Each entry line is
    /// whatever Bambu's FTP daemon emits — typically BusyBox-style
    /// "drwxr-xr-x ... name" or bare filenames.
    bool listDir(const String& path, std::vector<String>& out);

    const String& lastError() const { return _lastError; }

private:
    WiFiClientSecure _control;
    IPAddress        _ip;
    String           _sni;   // cached for the data connection (PASV)

    String           _lastError;

    TraceCb          _trace;
    uint32_t         _traceStart = 0;
    void             _emit(const char* step, int code, const String& text);

    // Low-level control: send one command, drain one response line, return
    // the 3-digit code (or -1 on timeout).
    int  _sendCmd(const String& cmd);
    int  _readResponse(String* body = nullptr, uint32_t timeout_ms = 5000);
    // Parse PASV response into (ip, port).
    bool _parsePasv(const String& line, IPAddress& ip, uint16_t& port);

    bool _openDataAndRetrieve(const String& path, uint32_t offset,
                              std::function<bool(WiFiClientSecure&)> reader);
};
