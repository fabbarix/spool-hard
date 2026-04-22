#include "bambu_cloud.h"
#include "config.h"
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <mbedtls/base64.h>

BambuCloudAuth g_bambu_cloud;

// Headers the Bambu Lab API expects. The shape matters more than any
// single value: Cloudflare in front of api.bambulab.com 403s the
// "bambu_network_agent/…" UA from non-residential IPs while accepting
// a regular browser UA + Origin/Referer pointing at bambulab.com plus
// the minimal OrcaSlicer-style X-BBL-* triple. This set mirrors
// temp/bambu_login.py — confirmed working from the same WAN.
//
// Crucially: only the X-BBL-Client-{Name,Type,Version} + X-BBL-OS-Type
// are actually checked; the longer "agent" set we used to send
// (X-BBL-OS-Version, X-BBL-Agent-*, X-BBL-Executable-info) was the
// fingerprint that was getting us blocked.
static constexpr const char* kUserAgent     =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/123.0.0.0 Safari/537.36";
static constexpr const char* kAccept        = "application/json, text/plain, */*";
static constexpr const char* kAcceptLang    = "en-US,en;q=0.9";
static constexpr const char* kOrigin        = "https://bambulab.com";
static constexpr const char* kReferer       = "https://bambulab.com/";
static constexpr const char* kClientName    = "OrcaSlicer";
static constexpr const char* kClientType    = "slicer";
static constexpr const char* kClientVersion = "01.09.05.51";
static constexpr const char* kClientOs      = "linux";

// ── Region helpers ─────────────────────────────────────────────

BambuCloudAuth::Region BambuCloudAuth::regionFromString(const String& s) {
    return s == "china" ? Region::China : Region::Global;
}

const char* BambuCloudAuth::regionToString(Region r) {
    return r == Region::China ? "china" : "global";
}

const char* BambuCloudAuth::regionBaseUrl(Region r) {
    return r == Region::China ? "https://api.bambulab.cn"
                              : "https://api.bambulab.com";
}

// ── Persistence ────────────────────────────────────────────────

void BambuCloudAuth::begin() {
    Preferences p;
    p.begin(NVS_NS_BAMBU_CLOUD, true);
    _token  = p.getString(NVS_KEY_BC_TOKEN,  "");
    _email  = p.getString(NVS_KEY_BC_EMAIL,  "");
    String r = p.getString(NVS_KEY_BC_REGION, "global");
    _region = regionFromString(r);
    p.end();
    if (_token.length()) {
        Serial.printf("[BambuCloud] loaded token for %s (%s)\n",
                      _email.c_str(), regionToString(_region));
    }
}

void BambuCloudAuth::saveToken(const String& token, Region r, const String& email) {
    _token  = token;
    _region = r;
    _email  = email;
    Preferences p;
    p.begin(NVS_NS_BAMBU_CLOUD, false);
    p.putString(NVS_KEY_BC_TOKEN,  token);
    p.putString(NVS_KEY_BC_REGION, regionToString(r));
    p.putString(NVS_KEY_BC_EMAIL,  email);
    p.end();
    Serial.printf("[BambuCloud] saved token for %s\n", email.c_str());
}

void BambuCloudAuth::clearToken() {
    _token = "";
    _email = "";
    Preferences p;
    p.begin(NVS_NS_BAMBU_CLOUD, false);
    p.clear();
    p.end();
    Serial.println("[BambuCloud] token cleared");
}

// ── Headers / HTTP plumbing ────────────────────────────────────

void BambuCloudAuth::_applyDefaultHeaders(HTTPClient& http) {
    // Browser-shaped UA + Origin/Referer + minimal X-BBL-* triple.
    // See the top-of-file note for why the longer "agent" set was
    // dropped — it was tripping Cloudflare's WAF.
    http.addHeader("User-Agent",            kUserAgent);
    http.addHeader("Accept",                kAccept);
    http.addHeader("Accept-Language",       kAcceptLang);
    http.addHeader("Origin",                kOrigin);
    http.addHeader("Referer",               kReferer);
    http.addHeader("X-BBL-Client-Name",     kClientName);
    http.addHeader("X-BBL-Client-Type",     kClientType);
    http.addHeader("X-BBL-Client-Version",  kClientVersion);
    http.addHeader("X-BBL-OS-Type",         kClientOs);
    http.addHeader("Content-Type",          "application/json");
}

String BambuCloudAuth::_baseUrl(Region r, bool useWebsiteHost) {
    if (useWebsiteHost) {
        return r == Region::China ? "https://bambulab.cn" : "https://bambulab.com";
    }
    return regionBaseUrl(r);
}

String BambuCloudAuth::_post(Region r, const char* path, const String& jsonBody,
                             bool useWebsiteHost,
                             String* setCookieOut, String* headersOut) {
    String url = _baseUrl(r, useWebsiteHost) + path;
    WiFiClientSecure client;
    // setInsecure() — we don't ship a Bambu CA bundle. The token alone
    // is sensitive but it's also what you'd type into OrcaSlicer, which
    // does the same thing. Tighten later by bundling the cert chain.
    client.setInsecure();
    HTTPClient http;
    http.setTimeout(15000);
    http.setReuse(false);
    if (!http.begin(client, url)) {
        _lastHttpStatus = 0;
        if (headersOut) *headersOut = "";
        return "";
    }
    _applyDefaultHeaders(http);
    // Always collect the diagnostic headers; cheap and the UI's
    // error-details panel needs them. Set-Cookie is also where the
    // TFA endpoint hands back the access token.
    static const char* kCollect[] = {"Content-Type", "Set-Cookie", "Server"};
    http.collectHeaders(kCollect, sizeof(kCollect) / sizeof(kCollect[0]));

    int code = http.POST(jsonBody);
    _lastHttpStatus = code;
    String body = (code > 0) ? http.getString() : "";

    if (setCookieOut) *setCookieOut = http.header("Set-Cookie");
    if (headersOut) {
        String h;
        for (size_t i = 0; i < sizeof(kCollect) / sizeof(kCollect[0]); ++i) {
            String v = http.header(kCollect[i]);
            if (v.length()) {
                h += kCollect[i];
                h += ": ";
                h += v;
                h += '\n';
            }
        }
        *headersOut = h;
    }
    http.end();
    return body;
}

String BambuCloudAuth::_get(Region r, const char* path, const String& bearer) {
    String url = String(regionBaseUrl(r)) + path;
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setTimeout(10000);
    http.setReuse(false);
    if (!http.begin(client, url)) {
        _lastHttpStatus = 0;
        return "";
    }
    _applyDefaultHeaders(http);
    http.addHeader("Authorization", String("Bearer ") + bearer);
    int code = http.GET();
    _lastHttpStatus = code;
    String body = (code > 0) ? http.getString() : "";
    http.end();
    return body;
}

// ── Login flow ────────────────────────────────────────────────

// Helper: pull the "token" or "accessToken" value out of an arbitrary
// Bambu cloud JSON response. Different endpoints use different keys.
static String _extractTokenField(const JsonDocument& doc) {
    if (doc["accessToken"].is<const char*>()) return doc["accessToken"].as<String>();
    if (doc["token"].is<const char*>())       return doc["token"].as<String>();
    return "";
}

// Helper: the Set-Cookie header from the TFA endpoint can carry the
// access token as `token=<value>; ...`. Pluck the value if present.
static String _extractCookieToken(const String& setCookieHeader) {
    int idx = setCookieHeader.indexOf("token=");
    if (idx < 0) return "";
    int start = idx + 6;
    int end   = setCookieHeader.indexOf(';', start);
    if (end < 0) end = setCookieHeader.length();
    return setCookieHeader.substring(start, end);
}

// Cap the raw body we forward to the UI so a runaway HTML error page
// doesn't blow the JSON response budget. 4 KB is plenty for any sane
// JSON error and gives a meaningful preview of an HTML one.
static constexpr size_t kRawBodyMax = 4096;

// Fill the diagnostics block on a StepResult — called from every
// login method so the UI's "show details" panel always has data.
// `fullUrl` is the resolved URL we hit (the caller knows whether it
// went to the api or website host).
static void _fillDiag(BambuCloudAuth::StepResult& out, const String& fullUrl,
                      int httpStatus, const String& body, const String& headers) {
    out.requestUrl      = fullUrl;
    out.httpStatus      = httpStatus;
    out.responseBody    = body.length() > kRawBodyMax
                          ? body.substring(0, kRawBodyMax) + "\n…[truncated]"
                          : body;
    out.responseHeaders = headers;
}

BambuCloudAuth::StepResult BambuCloudAuth::loginPassword(
        const String& account, const String& password, Region region) {
    StepResult out;

    JsonDocument req;
    req["account"]  = account;
    req["password"] = password;
    req["apiError"] = "";
    String body;
    serializeJson(req, body);

    String headers;
    String resp = _post(region, "/v1/user-service/user/login", body,
                        /*useWebsiteHost=*/false, nullptr, &headers);
    _fillDiag(out, _baseUrl(region, false) + "/v1/user-service/user/login",
              _lastHttpStatus, resp, headers);

    if (_lastHttpStatus <= 0) {
        out.status  = StepStatus::NetworkError;
        out.message = "couldn't reach the Bambu API";
        return out;
    }
    if (_lastHttpStatus >= 500) {
        out.status  = StepStatus::ServerError;
        out.message = String("API HTTP ") + _lastHttpStatus;
        return out;
    }

    JsonDocument doc;
    if (deserializeJson(doc, resp)) {
        out.status  = StepStatus::ServerError;
        out.message = "couldn't parse API response (HTTP " +
                      String(_lastHttpStatus) + ")";
        return out;
    }

    // Successful first-pass login.
    String tok = _extractTokenField(doc);
    if (tok.length()) {
        out.status = StepStatus::Ok;
        out.token  = tok;
        return out;
    }

    // Multi-step branches.
    String loginType = doc["loginType"] | "";
    if (loginType == "verifyCode") {
        out.status  = StepStatus::NeedEmailCode;
        out.message = "verification code emailed to " + account;
        return out;
    }
    if (loginType == "tfa") {
        out.status  = StepStatus::NeedTfa;
        out.tfaKey  = doc["tfaKey"] | "";
        out.message = "TFA required";
        return out;
    }

    // Anything else with a 4xx is "bad credentials"; otherwise generic.
    if (_lastHttpStatus == 401 || _lastHttpStatus == 403) {
        out.status  = StepStatus::InvalidCreds;
        out.message = doc["message"].as<String>();
        return out;
    }
    out.status  = StepStatus::ServerError;
    out.message = String("unexpected response (HTTP ") + _lastHttpStatus +
                  "): " + (doc["message"] | "");
    return out;
}

BambuCloudAuth::StepResult BambuCloudAuth::loginEmailCode(
        const String& account, const String& code, Region region) {
    StepResult out;

    JsonDocument req;
    req["account"] = account;
    req["code"]    = code;
    String body;
    serializeJson(req, body);

    String headers;
    String resp = _post(region, "/v1/user-service/user/login", body,
                        /*useWebsiteHost=*/false, nullptr, &headers);
    _fillDiag(out, _baseUrl(region, false) + "/v1/user-service/user/login",
              _lastHttpStatus, resp, headers);

    if (_lastHttpStatus <= 0) {
        out.status  = StepStatus::NetworkError;
        out.message = "couldn't reach the Bambu API";
        return out;
    }
    JsonDocument doc;
    if (deserializeJson(doc, resp)) {
        out.status  = StepStatus::ServerError;
        out.message = "couldn't parse API response (HTTP " +
                      String(_lastHttpStatus) + ")";
        return out;
    }
    String tok = _extractTokenField(doc);
    if (tok.length()) {
        out.status = StepStatus::Ok;
        out.token  = tok;
        return out;
    }
    out.status  = (_lastHttpStatus == 401 || _lastHttpStatus == 403)
                  ? StepStatus::InvalidCreds : StepStatus::ServerError;
    out.message = doc["message"].as<String>();
    return out;
}

BambuCloudAuth::StepResult BambuCloudAuth::loginTfa(
        const String& tfaKey, const String& tfaCode, Region region) {
    StepResult out;

    JsonDocument req;
    req["tfaKey"]  = tfaKey;
    req["tfaCode"] = tfaCode;
    String body;
    serializeJson(req, body);

    String setCookie;
    String headers;
    // TFA lives on the website host (bambulab.com/api/...), NOT on the
    // user-service host (api.bambulab.com/...). Mirrors the Python
    // reference's split.
    String resp = _post(region, "/api/sign-in/tfa", body,
                        /*useWebsiteHost=*/true, &setCookie, &headers);
    _fillDiag(out, _baseUrl(region, true) + "/api/sign-in/tfa",
              _lastHttpStatus, resp, headers);

    if (_lastHttpStatus <= 0) {
        out.status  = StepStatus::NetworkError;
        out.message = "couldn't reach the Bambu API";
        return out;
    }

    // TFA endpoint returns the token preferentially in the response
    // cookies; fall back to the JSON body.
    String tok = _extractCookieToken(setCookie);
    if (!tok.length()) {
        JsonDocument doc;
        deserializeJson(doc, resp);
        tok = _extractTokenField(doc);
    }
    if (tok.length()) {
        out.status = StepStatus::Ok;
        out.token  = tok;
        return out;
    }
    out.status  = StepStatus::InvalidCreds;
    out.message = "TFA code rejected";
    return out;
}

bool BambuCloudAuth::verifyToken(const String& token, Region r) {
    _get(r, "/v1/user-service/my/profile", token);
    return _lastHttpStatus == 200;
}

// ── JWT decode ────────────────────────────────────────────────

uint32_t BambuCloudAuth::decodeExpiry(const String& jwt) {
    // JWT is `header.payload.signature`. We want the payload.
    int dot1 = jwt.indexOf('.');
    if (dot1 < 0) return 0;
    int dot2 = jwt.indexOf('.', dot1 + 1);
    if (dot2 < 0) return 0;
    String payload = jwt.substring(dot1 + 1, dot2);

    // base64url → base64: '-'→'+', '_'→'/', then pad to multiple of 4.
    payload.replace('-', '+');
    payload.replace('_', '/');
    while (payload.length() % 4) payload += '=';

    // mbedtls_base64_decode is in ESP-IDF and small. Allocate a buffer
    // big enough for any sane JWT payload (claims rarely exceed a few KB).
    size_t buflen = (payload.length() * 3) / 4 + 4;
    uint8_t* buf  = (uint8_t*)malloc(buflen);
    if (!buf) return 0;
    size_t outlen = 0;
    int rc = mbedtls_base64_decode(buf, buflen, &outlen,
                                   (const uint8_t*)payload.c_str(), payload.length());
    if (rc != 0) { free(buf); return 0; }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, buf, outlen);
    free(buf);
    if (err) return 0;
    if (!doc["exp"].is<uint32_t>()) return 0;
    return doc["exp"].as<uint32_t>();
}
