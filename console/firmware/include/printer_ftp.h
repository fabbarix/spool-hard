#pragma once
#include <Arduino.h>
#include <IPAddress.h>
#include <functional>
#include <vector>

#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/net_sockets.h>

/**
 * Implicit-TLS FTPS client targeting Bambu Lab printers.
 *
 *   - Control + data connection both TLS (implicit, :990).
 *   - **TLS session reuse between control and data is required** —
 *     Bambu's FTPS daemon enforces RFC 4217 §10.1 (data handshake must
 *     resume the control session). We use raw mbedtls so we can call
 *     `mbedtls_ssl_get_session()` on the control after login and
 *     `mbedtls_ssl_set_session()` on each data context before its
 *     handshake. WiFiClientSecure can't do this — its mbedtls session
 *     is internal and not exposed.
 *   - Auth: username "bblp", password = printer access code.
 *   - Binary transfers only (`TYPE I`).
 *   - Passive mode only; PROT P negotiated post-login.
 *
 * Typical use:
 *   PrinterFtp ftp;
 *   if (!ftp.connect(ip, access_code, serial)) { ... }
 *   ftp.retrieveStream("/cache/job.gcode", [](...) { ... });
 *   ftp.quit();
 */
class PrinterFtp {
public:
    using ChunkCb = std::function<bool(const uint8_t* data, size_t len, size_t total_so_far)>;
    using RangeCb = std::function<bool(const uint8_t* data, size_t len)>;

    // Debug tap: fires once per protocol-level event.
    using TraceCb = std::function<void(const char* step, int code,
                                       const String& text, uint32_t elapsed_ms)>;
    void setTraceCb(TraceCb cb) { _trace = std::move(cb); _traceStart = millis(); }

    PrinterFtp();
    ~PrinterFtp();

    bool connect(const IPAddress& ip, const String& access_code,
                 const String& sni_hostname, uint16_t port = 990);
    bool quit();
    bool isOpen() const { return _ctrl_open; }

    bool retrieveRange(const String& path, uint32_t offset, uint32_t length, RangeCb cb);
    bool retrieveStream(const String& path, ChunkCb cb);
    bool retrieveInto(const String& path, uint32_t offset, uint32_t length, uint8_t* dst);

    // Stream the entire file via RETR and retain only the trailing
    // `tail_size` bytes in `dst`. Used as a fallback when SIZE is
    // unreliable (Bambu's H2D firmware reports SIZE 0 for the active
    // 3MF mid-print, but RETR still returns the real bytes).
    bool retrieveTrailing(const String& path, uint32_t tail_size,
                          uint8_t* dst, uint32_t* streamed_total,
                          uint32_t* tail_actual);

    int32_t size(const String& path);

    bool listDir(const String& path, std::vector<String>& out);

    const String& lastError() const { return _lastError; }

private:
    // ── Raw mbedtls state ──────────────────────────────────────────
    mbedtls_entropy_context  _entropy;
    mbedtls_ctr_drbg_context _drbg;
    mbedtls_ssl_config       _conf;
    bool                     _mbed_inited = false;

    // Control channel: persistent for the FTP session.
    mbedtls_ssl_context _ctrl_ssl;
    mbedtls_net_context _ctrl_net;
    bool                _ctrl_open = false;

    // Saved control session — used to resume on every data channel
    // handshake. Populated right after the control TLS handshake
    // completes.
    mbedtls_ssl_session _ctrl_session;
    bool                _ctrl_session_saved = false;

    // For the data channel's TLS handshake — the SNI hostname is the
    // printer's serial (cert CN), kept around between login and each
    // data-channel open.
    IPAddress _ip;
    String    _sni;

    String    _lastError;
    TraceCb   _trace;
    uint32_t  _traceStart = 0;
    void      _emit(const char* step, int code, const String& text);

    // Read the FTP response off the control TLS — single 3-digit code,
    // possibly multiline (terminated by "<code> " on its own line).
    int  _readResponse(String* body = nullptr, uint32_t timeout_ms = 5000);
    int  _sendCmd(const String& cmd);
    bool _parsePasv(const String& line, IPAddress& ip, uint16_t& port);

    // Resource lifecycle helpers.
    bool _initMbedtlsOnce();
    void _resetCtrl();          // clear control ssl/net contexts (no resource leak)

    // Open one data-channel TLS connection by reissuing PASV and
    // resuming `_ctrl_session`. On success, `data_net` and `data_ssl`
    // are connected and TLS-handshaked; caller is responsible for
    // tearing them down via `_closeData()` once the data transfer is
    // done. Returns false on any failure with `_lastError` set.
    bool _openDataChannel(mbedtls_net_context* data_net,
                          mbedtls_ssl_context* data_ssl);
    void _closeData(mbedtls_net_context* data_net,
                    mbedtls_ssl_context* data_ssl);

    bool _openDataAndRetrieve(const String& path, uint32_t offset,
                              std::function<bool(mbedtls_ssl_context&)> reader);
};
