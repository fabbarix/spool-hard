#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

/**
 * Bambu MQTT command signing (post-Jan-2025 firmware, printer NOT in
 * Developer Mode). The user supplies the slicer's app certificate + key
 * (+ optional CRL) as extracted from their own Bambu Studio install; every
 * `print.*` command is then published as
 *
 *   {"header":{"cert_id":<serial hex + issuer DN>,"payload_len":N,
 *    "sign_alg":"RSA_SHA256","sign_string":<b64 sig>,"sign_ver":"v1.0"},
 *    "print":<canonical>}
 *
 * (header first with sorted keys, command object last and byte-identical to
 * the signed bytes -- the printer is byte-sensitive about this layout)
 *
 * where the signature (RSA PKCS#1 v1.5 / SHA-256) covers the exact bytes
 * '{"print":' + <canonical> + '}' and <canonical> is the inner object with
 * keys sorted at every depth and no whitespace (what nlohmann::json::dump()
 * in Bambu Studio produces). Format per OpenBambuAPI cloud-x509-auth.md.
 *
 * One identity for the whole console (the cert is issued to a slicer
 * install, not to a printer). Files live on userfs under /bambu_sign/
 * (survive firmware/frontend updates; excluded from backups because the
 * key is a secret). Thread-safe: config calls come from AsyncTCP, wrap()
 * from loopTask.
 */
namespace BambuSigner {

void begin();   // after LittleFS (userfs) is mounted

struct Status {
    bool   configured = false;   // cert + key present and valid
    bool   enabled    = false;   // configured AND not switched off
    String cert_id;              // what goes in header.cert_id
    String subject, issuer;
    String not_before, not_after; // "YYYY-MM-DD"
    bool   crl_present = false;
    String crl_next_update;      // "YYYY-MM-DD"
    bool   revoked     = false;  // leaf serial listed in the CRL
    int    chain_certs = 0;      // certificates in cert.pem (leaf + CAs)
    String error;                // last load / validation error
};
Status status();

// Validate and store. crlPem may be empty (keeps none). Returns "" on
// success, else a user-facing error; on error nothing is changed.
String install(const String& certPem, const String& keyPem, const String& crlPem);
void   setEnabled(bool on);
void   remove();

bool enabled();
// Bumped on every install / enable / disable / remove (cheap to poll).
uint32_t generation();

// Build the signed MQTT payload for `{"<topKey>": inner}`. Returns false
// (out untouched) when signing is off or fails -> caller publishes the
// unsigned form. `toSign` (optional) receives the signed bytes.
bool wrap(const char* topKey, JsonVariantConst inner, String& out, String* toSign = nullptr);

// The unsigned `security.app_cert_install` message that registers our
// certificate with a printer (it forgets them on every power cycle):
// {"security":{"app_cert":<cert.pem chain>,"command":"app_cert_install",
// "crl":<crl.pem>,"sequence_id":seq}}, compact, keys sorted. False (with a
// reason) when signing is off or no CRL is stored -- the printer needs both
// the full chain and the CRL.
bool buildCertInstall(const String& seq, String& out, String* why = nullptr);

// Canonical JSON (sorted keys, no whitespace). Exposed for the self-test.
void canonicalJson(JsonVariantConst v, String& out);

}  // namespace BambuSigner
