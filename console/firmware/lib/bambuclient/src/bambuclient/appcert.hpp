#pragma once

#include <array>
#include <string>

// Vendored from /opt/mrwho/Projects/bambulab/bambuclient (appcert_fetch),
// ported from OpenSSL + libcurl to mbedtls + Arduino HTTPClient. The
// request/response protocol, the custom-S-box AES-256-CTR unwrap and the
// S-box tables are unchanged; see README.md here for the differences.

namespace bambuclient {

struct AppCertCredentials {
    std::string cert_pem;    // PEM certificate chain (slicer_cert.pem)
    std::string crl_pem;     // PEM certificate revocation list (slicer_crl.pem)
    std::string key_pem;     // Reconstructed PEM RSA private key (slicer_key.pem)
    std::string leaf_serial; // Lowercase hex serial number of the leaf certificate
};

// Default Bambu Cloud API endpoint
inline constexpr const char* kDefaultCloudApiHost = "https://api.bambulab.com";

// PEM SubjectPublicKeyInfo of CN=service.bambulab.com (server wrap key).
const char* server_wrap_key_pem();

// One HTTPS GET. Returns false (with *err) only for transport failures; an
// HTTP error status is reported through *status with the body still filled.
using HttpGetFn = bool (*)(const std::string& url, long* status,
                           std::string* body, std::string* err);

// Default transport: Arduino WiFiClientSecure + HTTPClient (certificate not
// verified, like the rest of the console's cloud code).
bool arduino_http_get(const std::string& url, long* status,
                      std::string* body, std::string* err);

// Fetches the shared Bambu slicer app credentials from the cloud endpoint in
// memory. The caller supplies the client_auth_secret; nothing is embedded.
// Needs ~10 KB of stack and a TLS handshake worth of internal DRAM, so run it
// from a task, never from an AsyncTCP handler. Returns true on success, false
// on failure (filling *err if non-null).
bool fetch_appcert(AppCertCredentials* out,
                   const std::string& client_auth_secret,
                   std::string* err = nullptr,
                   const std::string& api_host = kDefaultCloudApiHost,
                   HttpGetFn http_get = arduino_http_get);

namespace detail {

// Custom S-box AES-256-CTR encryption/decryption
std::string ctr_xor(const unsigned char key[32], const std::string& nonce,
                    const std::string& data);

// Base64-URL encode with '=' padding (RFC 4648 §5)
std::string b64url_encode(const std::string& data);

// Base64 decode accepting standard and URL-safe characters, optional padding, and whitespace
bool b64_decode_any(const std::string& in, std::string* out, std::string* err = nullptr);

// Unwraps the response `key` field: base64 blob -> 5 CRT limbs (p, q, dp, dq, qinv)
bool decode_key_blob(const std::string& blob_b64,
                     std::array<std::string, 5>* limbs, std::string* err = nullptr);

// Rebuilds an unencrypted PEM RSA private key from CRT limbs
std::string rsa_pem_from_crt(const std::array<std::string, 5>& limbs,
                             std::string* err = nullptr);

} // namespace detail
} // namespace bambuclient
