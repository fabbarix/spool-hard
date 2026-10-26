#include <atomic>
#include "bambu_signer.h"

#include <LittleFS.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crl.h>
#include <mbedtls/x509_crt.h>

#include <algorithm>
#include <vector>

#include "spoolhard/ring_log.h"
#include "spoolhard/serial_mirror.h"

namespace {

constexpr const char* kDir      = "/bambu_sign";
constexpr const char* kCertPath = "/bambu_sign/cert.pem";
constexpr const char* kKeyPath  = "/bambu_sign/key.pem";
constexpr const char* kCrlPath  = "/bambu_sign/crl.pem";
constexpr const char* kOffPath  = "/bambu_sign/disabled";   // exists = switched off
constexpr size_t      kMaxPem   = 8192;

SemaphoreHandle_t    s_mtx = nullptr;
mbedtls_pk_context   s_pk;          // parsed private key, valid while s_loaded
bool                 s_loaded = false;
bool                 s_off    = false;
std::atomic<uint32_t> s_gen{0};
BambuSigner::Status  s_status;

struct Lock {
    Lock()  { if (s_mtx) xSemaphoreTakeRecursive(s_mtx, portMAX_DELAY); }
    ~Lock() { if (s_mtx) xSemaphoreGiveRecursive(s_mtx); }
};

int rng(void*, unsigned char* out, size_t len) {
    esp_fill_random(out, len);
    return 0;
}

String readFile(const char* path) {
    File f = LittleFS.open(path, "r");
    if (!f) return String();
    String s = f.readString();
    f.close();
    return s;
}

bool writeFileAtomic(const char* path, const String& data) {
    String tmp = String(path) + ".tmp";
    File f = LittleFS.open(tmp, "w");
    if (!f) return false;
    size_t n = f.write((const uint8_t*)data.c_str(), data.length());
    f.close();
    if (n != data.length()) { LittleFS.remove(tmp); return false; }
    LittleFS.remove(path);
    return LittleFS.rename(tmp, path);
}

String fmtDate(const mbedtls_x509_time& t) {
    char b[12];
    snprintf(b, sizeof(b), "%04d-%02d-%02d", t.year, t.mon, t.day);
    return b;
}

String mbedErr(const char* what, int rc) {
    char b[64];
    snprintf(b, sizeof(b), "%s (mbedtls -0x%04x)", what, (unsigned)-rc);
    return b;
}

// Parsed + validated view of a cert/key/CRL triple. Fills `st` and, when
// `pkOut` is non-null, moves the parsed key into it. Returns "" or an error.
String parseTriple(const String& certPem, const String& keyPem, const String& crlPem,
                   BambuSigner::Status& st, mbedtls_pk_context* pkOut) {
    if (certPem.isEmpty() || keyPem.isEmpty()) return "certificate and key are required";
    if (certPem.length() > kMaxPem || keyPem.length() > kMaxPem || crlPem.length() > kMaxPem)
        return "file too large";
    if (certPem.indexOf("-----BEGIN CERTIFICATE-----") < 0) return "certificate is not PEM";
    if (keyPem.indexOf("PRIVATE KEY-----") < 0) return "key is not a PEM private key";

    String err;
    mbedtls_x509_crt crt;  mbedtls_x509_crt_init(&crt);
    mbedtls_pk_context pk; mbedtls_pk_init(&pk);
    mbedtls_x509_crl crl;  mbedtls_x509_crl_init(&crl);
    int rc;
    do {
        // PEM parsers need the terminating NUL counted in the length.
        rc = mbedtls_x509_crt_parse(&crt, (const unsigned char*)certPem.c_str(), certPem.length() + 1);
        if (rc < 0) { err = mbedErr("certificate parse failed", rc); break; }
        rc = mbedtls_pk_parse_key(&pk, (const unsigned char*)keyPem.c_str(), keyPem.length() + 1,
                                  nullptr, 0, rng, nullptr);
        if (rc) { err = mbedErr("key parse failed (encrypted keys are not supported)", rc); break; }
        if (mbedtls_pk_get_type(&pk) != MBEDTLS_PK_RSA) { err = "key is not RSA"; break; }
        // First certificate in the file is the leaf; the rest is chain.
        rc = mbedtls_pk_check_pair(&crt.pk, &pk, rng, nullptr);
        if (rc) { err = "key does not match the (first) certificate"; break; }

        // cert_id = leaf serial as lowercase hex (leading zero bytes dropped,
        // left-padded to 32) + issuer DN, e.g. "4a63...a777CN=GLOF....bambulab.com".
        const unsigned char* sp = crt.serial.p;
        size_t sl = crt.serial.len;
        while (sl > 1 && *sp == 0) { ++sp; --sl; }
        String hex;
        static const char* kHex = "0123456789abcdef";
        for (size_t i = 0; i < sl; ++i) { hex += kHex[sp[i] >> 4]; hex += kHex[sp[i] & 15]; }
        while (hex.length() < 32) hex = "0" + hex;
        char dn[160];
        if (mbedtls_x509_dn_gets(dn, sizeof(dn), &crt.issuer) < 0) dn[0] = 0;
        st.issuer = dn;
        st.cert_id = hex + dn;
        if (mbedtls_x509_dn_gets(dn, sizeof(dn), &crt.subject) < 0) dn[0] = 0;
        st.subject    = dn;
        st.not_before = fmtDate(crt.valid_from);
        st.not_after  = fmtDate(crt.valid_to);

        st.crl_present = false; st.revoked = false; st.crl_next_update = "";
        if (crlPem.length()) {
            rc = mbedtls_x509_crl_parse(&crl, (const unsigned char*)crlPem.c_str(), crlPem.length() + 1);
            if (rc) { err = mbedErr("CRL parse failed", rc); break; }
            st.crl_present     = true;
            st.crl_next_update = fmtDate(crl.next_update);
            for (const mbedtls_x509_crl_entry* e = &crl.entry; e; e = e->next) {
                if (e->serial.len == crt.serial.len &&
                    memcmp(e->serial.p, crt.serial.p, crt.serial.len) == 0) {
                    st.revoked = true;
                    break;
                }
            }
        }
    } while (false);

    if (err.isEmpty() && pkOut) {
        // Hand the parsed key over (the context owns heap; swap, don't copy).
        mbedtls_pk_free(pkOut);
        *pkOut = pk;
        mbedtls_pk_init(&pk);
    }
    mbedtls_pk_free(&pk);
    mbedtls_x509_crt_free(&crt);
    mbedtls_x509_crl_free(&crl);
    return err;
}

// JSON string literal with the same escaping ArduinoJson uses for values
// (", \, \b \f \n \r \t, other control chars as \u00XX; UTF-8 kept raw).
void appendJsonString(const char* s, String& out) {
    out += '"';
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", c);
                    out += b;
                } else {
                    out += (char)c;
                }
        }
    }
    out += '"';
}

void load() {
    Lock l;
    s_gen.fetch_add(1);
    if (s_loaded) { mbedtls_pk_free(&s_pk); mbedtls_pk_init(&s_pk); }
    s_loaded = false;
    s_status = BambuSigner::Status();
    s_off    = LittleFS.exists(kOffPath);
    String cert = readFile(kCertPath), key = readFile(kKeyPath);
    if (cert.isEmpty() && key.isEmpty()) return;   // not configured
    String err = parseTriple(cert, key, readFile(kCrlPath), s_status, &s_pk);
    if (err.length()) {
        s_status.error = err;
        dlog("Sign", "stored certificate unusable: %s", err.c_str());
        return;
    }
    for (int i = 0; (i = cert.indexOf("BEGIN CERTIFICATE", i)) >= 0; ++i) ++s_status.chain_certs;
    s_loaded = true;
    s_status.configured = true;
    s_status.enabled    = !s_off;
    dlog("Sign", "loaded cert_id=%s valid to %s%s%s", s_status.cert_id.c_str(),
         s_status.not_after.c_str(), s_status.revoked ? " REVOKED" : "",
         s_off ? " (disabled)" : "");
}

}  // namespace

namespace BambuSigner {

void begin() {
    if (!s_mtx) s_mtx = xSemaphoreCreateRecursiveMutex();
    mbedtls_pk_init(&s_pk);
    load();
}

Status status() { Lock l; return s_status; }

bool enabled() { Lock l; return s_loaded && !s_off; }

uint32_t generation() { return s_gen.load(); }

String install(const String& certPem, const String& keyPem, const String& crlPem) {
    Status probe;
    String err = parseTriple(certPem, keyPem, crlPem, probe, nullptr);
    if (err.length()) return err;
    Lock l;
    if (!LittleFS.exists(kDir)) LittleFS.mkdir(kDir);
    if (!writeFileAtomic(kCertPath, certPem) || !writeFileAtomic(kKeyPath, keyPem))
        return "write to userfs failed";
    if (crlPem.length()) {
        if (!writeFileAtomic(kCrlPath, crlPem)) return "write to userfs failed";
    } else {
        LittleFS.remove(kCrlPath);
    }
    LittleFS.remove(kOffPath);   // a fresh install is enabled
    load();
    return "";
}

void setEnabled(bool on) {
    Lock l;
    if (on) LittleFS.remove(kOffPath);
    else {
        if (!LittleFS.exists(kDir)) LittleFS.mkdir(kDir);
        File f = LittleFS.open(kOffPath, "w");
        if (f) f.close();
    }
    load();
}

void remove() {
    Lock l;
    LittleFS.remove(kCertPath);
    LittleFS.remove(kKeyPath);
    LittleFS.remove(kCrlPath);
    LittleFS.remove(kOffPath);
    load();
    dlog("Sign", "certificate removed");
}

bool buildCertInstall(const String& seq, String& out, String* why) {
    Lock l;
    if (!s_loaded || s_off) { if (why) *why = "signing is not enabled"; return false; }
    String cert = readFile(kCertPath), crl = readFile(kCrlPath);
    if (crl.isEmpty()) { if (why) *why = "no CRL installed (the printer requires it)"; return false; }
    // Keys inserted in sorted order; ArduinoJson keeps insertion order and
    // escapes the PEM newlines.
    JsonDocument doc;
    JsonObject s = doc["security"].to<JsonObject>();
    s["app_cert"]    = cert;
    s["command"]     = "app_cert_install";
    s["crl"]         = crl;
    s["sequence_id"] = seq;
    out = "";
    serializeJson(doc, out);
    return true;
}

void canonicalJson(JsonVariantConst v, String& out) {
    if (v.is<JsonObjectConst>()) {
        JsonObjectConst o = v.as<JsonObjectConst>();
        std::vector<const char*> keys;
        for (JsonPairConst kv : o) keys.push_back(kv.key().c_str());
        std::sort(keys.begin(), keys.end(),
                  [](const char* a, const char* b) { return strcmp(a, b) < 0; });
        out += '{';
        bool first = true;
        for (const char* k : keys) {
            if (!first) out += ',';
            first = false;
            appendJsonString(k, out);
            out += ':';
            canonicalJson(o[k], out);
        }
        out += '}';
    } else if (v.is<JsonArrayConst>()) {
        out += '[';
        bool first = true;
        for (JsonVariantConst e : v.as<JsonArrayConst>()) {
            if (!first) out += ',';
            first = false;
            canonicalJson(e, out);
        }
        out += ']';
    } else if (v.is<const char*>()) {
        appendJsonString(v.as<const char*>(), out);
    } else {
        String s;   // numbers / bools / null
        serializeJson(v, s);
        out += s;
    }
}

bool wrap(const char* topKey, JsonVariantConst inner, String& out, String* toSign) {
    Lock l;
    if (!s_loaded || s_off) return false;
    uint32_t t0 = millis();
    String canon;
    canonicalJson(inner, canon);
    String msg = String("{\"") + topKey + "\":" + canon + "}";

    unsigned char hash[32];
    if (mbedtls_sha256((const unsigned char*)msg.c_str(), msg.length(), hash, 0) != 0) return false;
    unsigned char sig[MBEDTLS_PK_SIGNATURE_MAX_SIZE];
    size_t sigLen = 0;
    int rc = mbedtls_pk_sign(&s_pk, MBEDTLS_MD_SHA256, hash, sizeof(hash),
                             sig, sizeof(sig), &sigLen, rng, nullptr);
    if (rc) { dlog("Sign", "sign failed -0x%04x", (unsigned)-rc); return false; }
    unsigned char b64[((MBEDTLS_PK_SIGNATURE_MAX_SIZE + 2) / 3) * 4 + 1];
    size_t b64Len = 0;
    if (mbedtls_base64_encode(b64, sizeof(b64), &b64Len, sig, sigLen) != 0) return false;
    b64[b64Len] = 0;

    out.reserve(msg.length() + b64Len + s_status.cert_id.length() + 128);
    // Wire layout as Bambu Studio publishes it: header first (keys sorted),
    // then the command object byte-identical to the signed bytes, so the
    // message ends with exactly msg minus its leading '{'. The printer
    // verifies against those raw bytes; header-last is refused (84033545).
    out  = "{\"header\":{\"cert_id\":\"";
    out += s_status.cert_id;
    out += "\",\"payload_len\":";
    out += String((unsigned)msg.length());
    out += ",\"sign_alg\":\"RSA_SHA256\",\"sign_string\":\"";
    out += (const char*)b64;
    out += "\",\"sign_ver\":\"v1.0\"},\"";
    out += topKey;
    out += "\":";
    out += canon;
    out += "}";
    if (toSign) *toSign = msg;
    dlog("Sign", "signed %s payload_len=%u in %lu ms", topKey, (unsigned)msg.length(),
         (unsigned long)(millis() - t0));
    return true;
}

}  // namespace BambuSigner
