#include "bambuclient/appcert.hpp"

#include <ArduinoJson.h>
#include <esp_random.h>
#include <mbedtls/gcm.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>

#include <cctype>
#include <cstring>
#include <memory>

#include "spoolhard/psram_json_alloc.h"

namespace bambuclient {

namespace {

constexpr std::size_t kGcmIvLen  = 12;
constexpr std::size_t kGcmTagLen = 16;

int rng(void*, unsigned char* out, std::size_t len)
{
    esp_fill_random(out, len);
    return 0;
}

std::string body_prefix(const std::string& s, std::size_t n)
{
    return s.size() <= n ? s : s.substr(0, n) + "...";
}

// AES-256-GCM seal: out = iv(12) || ciphertext || tag(16).
bool aes_gcm_seal(const unsigned char key[32], const std::string& plain,
                  std::string* out, std::string* err)
{
    unsigned char iv[kGcmIvLen];
    esp_fill_random(iv, sizeof(iv));
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    std::string ct(plain.size(), '\0');
    unsigned char tag[kGcmTagLen];
    int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0)
        rc = mbedtls_gcm_crypt_and_tag(
            &gcm, MBEDTLS_GCM_ENCRYPT, plain.size(), iv, sizeof(iv), nullptr, 0,
            reinterpret_cast<const unsigned char*>(plain.data()),
            reinterpret_cast<unsigned char*>(&ct[0]), sizeof(tag), tag);
    mbedtls_gcm_free(&gcm);
    if (rc != 0) {
        if (err) *err = "GCM seal failed (mbedtls " + std::to_string(rc) + ")";
        return false;
    }
    out->assign(reinterpret_cast<const char*>(iv), sizeof(iv));
    out->append(ct);
    out->append(reinterpret_cast<const char*>(tag), sizeof(tag));
    return true;
}

// RSA-PKCS#1 v1.5 encrypt of the session key to the embedded server key.
bool rsa_wrap_session_key(const std::string& session_key, std::string* out,
                          std::string* err)
{
    const std::string pem(server_wrap_key_pem());
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int rc = mbedtls_pk_parse_public_key(
        &pk, reinterpret_cast<const unsigned char*>(pem.c_str()), pem.size() + 1);
    if (rc != 0) {
        mbedtls_pk_free(&pk);
        if (err) *err = "embedded server wrap key does not parse";
        return false;
    }
    unsigned char buf[512];   // RSA-2048 -> 256 bytes
    std::size_t olen = 0;
    rc = mbedtls_pk_encrypt(
        &pk, reinterpret_cast<const unsigned char*>(session_key.data()),
        session_key.size(), buf, &olen, sizeof(buf), rng, nullptr);
    mbedtls_pk_free(&pk);
    if (rc != 0) {
        if (err) *err = "RSA wrap failed (mbedtls " + std::to_string(rc) + ")";
        return false;
    }
    out->assign(reinterpret_cast<const char*>(buf), olen);
    return true;
}

// Lowercase hex of the serial without ASN.1 sign-padding zeros (what the
// OpenSSL BN_bn2hex original produced).
std::string serial_hex(const mbedtls_x509_buf& sn)
{
    static const char kHex[] = "0123456789abcdef";
    std::size_t i = 0;
    while (i + 1 < sn.len && sn.p[i] == 0) ++i;
    std::string out;
    for (; i < sn.len; ++i) {
        out.push_back(kHex[sn.p[i] >> 4]);
        out.push_back(kHex[sn.p[i] & 15]);
    }
    return out;
}

} // namespace

bool fetch_appcert(AppCertCredentials* out,
                   const std::string& client_auth_secret,
                   std::string* err,
                   const std::string& api_host,
                   HttpGetFn http_get)
{
    auto fail = [err](const std::string& e) -> bool {
        if (err) *err = e;
        return false;
    };

    if (!out) return fail("null output pointer");
    if (client_auth_secret.empty())
        return fail("client_auth_secret must not be empty");
    if (!http_get) return fail("no HTTP transport");

    // 1. Session key + encrypted secret
    unsigned char session_key[32];
    esp_fill_random(session_key, sizeof(session_key));
    std::string gcm_err;
    std::string enc_raw;
    if (!aes_gcm_seal(session_key, client_auth_secret, &enc_raw, &gcm_err))
        return fail("secret encryption failed: " + gcm_err);
    const std::string enc_secret = detail::b64url_encode(enc_raw);

    // 2. Wrap session key
    std::string wrap_err;
    std::string wrapped;
    if (!rsa_wrap_session_key(
            std::string(reinterpret_cast<const char*>(session_key),
                        sizeof(session_key)),
            &wrapped, &wrap_err))
        return fail("session-key wrap failed: " + wrap_err);
    const std::string aes256 = detail::b64url_encode(wrapped);

    // 3. Make HTTP request
    std::string host = api_host.empty() ? kDefaultCloudApiHost : api_host;
    while (!host.empty() && host.back() == '/') host.pop_back();

    const std::string url =
        host + "/v1/iot-service/api/user/applications/" + enc_secret +
        "/cert?aes256=" + aes256 + "&ver=1";

    long status = 0;
    std::string body, http_err;
    if (!http_get(url, &status, &body, &http_err))
        return fail("transport error: " + http_err);
    if (status != 200)
        return fail("HTTP " + std::to_string(status) + ": " + body_prefix(body, 300));

    // 4. Parse response JSON
    JsonDocument root(&g_psramJsonAlloc);
    if (DeserializationError je = deserializeJson(root, body))
        return fail(std::string("response is not JSON (") + je.c_str() + "): " +
                    body_prefix(body, 200));

    const JsonVariantConst code_v = root["code"];
    bool code_ok = true;
    if (code_v.is<long>()) code_ok = code_v.as<long>() == 0;
    else if (code_v.is<const char*>()) {
        const char* c = code_v.as<const char*>();
        code_ok = !c[0] || std::strcmp(c, "0") == 0;
    }
    const char* msg = root["message"] | "";
    if (!code_ok && std::strcmp(msg, "success") != 0)
        return fail("API error: " + body_prefix(body, 300));

    // cert / crl arrive either as one string or as an array of PEM pieces.
    auto join_field = [&root](const char* name) -> std::string {
        const JsonVariantConst v = root[name];
        std::string res;
        if (v.is<const char*>()) res = v.as<const char*>();
        else if (v.is<JsonArrayConst>())
            for (JsonVariantConst e : v.as<JsonArrayConst>())
                res += e.as<const char*>() ? e.as<const char*>() : "";
        return res;
    };

    const std::string cert_pem = join_field("cert");
    const std::string crl_pem  = join_field("crl");
    const std::string key_blob = root["key"] | "";
    if (cert_pem.empty())
        return fail("response carries no cert: " + body_prefix(body, 300));
    if (crl_pem.empty())
        return fail("response carries no crl: " + body_prefix(body, 300));
    if (key_blob.empty())
        return fail("response carries no key: " + body_prefix(body, 300));

    // 5. Unwrap key and cross check against the leaf certificate
    std::array<std::string, 5> limbs;
    std::string unwrap_err;
    if (!detail::decode_key_blob(key_blob, &limbs, &unwrap_err))
        return fail("key unwrap failed: " + unwrap_err);

    std::string pem_err;
    const std::string key_pem = detail::rsa_pem_from_crt(limbs, &pem_err);
    if (key_pem.empty())
        return fail("RSA rebuild failed: " + pem_err);

    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    std::string result_err, serial;
    do {
        int rc = mbedtls_x509_crt_parse(
            &crt, reinterpret_cast<const unsigned char*>(cert_pem.c_str()),
            cert_pem.size() + 1);
        if (rc < 0) { result_err = "response cert is not a valid X.509 PEM chain"; break; }
        rc = mbedtls_pk_parse_key(
            &pk, reinterpret_cast<const unsigned char*>(key_pem.c_str()),
            key_pem.size() + 1, nullptr, 0, rng, nullptr);
        if (rc != 0) { result_err = "rebuilt private key does not parse"; break; }
        if (mbedtls_pk_check_pair(&crt.pk, &pk, rng, nullptr) != 0) {
            result_err = "reconstructed key does not match leaf certificate";
            break;
        }
        serial = serial_hex(crt.serial);
    } while (false);
    mbedtls_pk_free(&pk);
    mbedtls_x509_crt_free(&crt);
    if (!result_err.empty()) return fail(result_err);

    out->cert_pem = cert_pem;
    out->crl_pem = crl_pem;
    out->key_pem = key_pem;
    out->leaf_serial = serial;
    return true;
}

} // namespace bambuclient
