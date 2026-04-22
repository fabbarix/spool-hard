#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// Bambu Lab cloud API authentication + token management.
//
// Mirrors the auth flow of github.com/coelacant1/Bambu-Lab-Cloud-API
// (the Python reference): the user POSTs credentials to
// `/v1/user-service/user/login`. Possible outcomes:
//   • success  — `accessToken` is in the JSON response, store it.
//   • email-code branch — response carries `loginType: "verifyCode"`;
//     server emails a code; user POSTs `{account, code}` back to the
//     same endpoint.
//   • TFA branch — response carries `loginType: "tfa"` and a `tfaKey`;
//     the user POSTs `{tfaKey, tfaCode}` to `/api/sign-in/tfa`. Token
//     comes back either in the response cookies (`token` cookie) or in
//     the JSON (`accessToken` / `token`).
//
// The flow is stateless from the device's perspective: the React UI
// holds the in-progress session details (account, tfaKey) between
// calls. The device only persists the final accepted token + region.
//
// Tokens are JWTs; `decodeExpiry()` extracts the `exp` claim from the
// payload (no signature verification — we just need it to display the
// expiration date).
class BambuCloudAuth {
public:
    enum class Region { Global, China };

    enum class StepStatus {
        Ok,             // login complete; token captured
        NeedEmailCode,  // server sent an email code; ask the user for it
        NeedTfa,        // MFA required; ask user for code (carry tfaKey)
        InvalidCreds,   // 4xx / explicit failure
        NetworkError,   // couldn't reach the API
        ServerError,    // API returned 5xx or unparseable response
    };

    struct StepResult {
        StepStatus status = StepStatus::ServerError;
        String     token;        // populated on Ok
        String     tfaKey;       // populated on NeedTfa
        String     message;      // human-readable detail (UI surfaces it)

        // Raw diagnostics — populated on every call so the UI's
        // "show details" panel can reveal exactly what came back from
        // api.bambulab.{com,cn} when the friendly message isn't
        // enough. URL is the endpoint we hit; httpStatus is the HTTP
        // code (or 0 if we never got that far); responseBody is the
        // verbatim body (truncated to a sensible cap to keep NVS-side
        // memory bounded); responseHeaders is a `Name: value\n…`
        // dump of the few headers we collect (Content-Type,
        // Set-Cookie, Server). On the Ok path the firmware does NOT
        // forward these to the frontend — they may contain the token
        // — but they're still populated for log-side tracing.
        String     requestUrl;
        int        httpStatus = 0;
        String     responseBody;
        String     responseHeaders;
    };

    void begin();    // load persisted state from NVS

    // ── Login flow ──────────────────────────────────────────
    StepResult loginPassword(const String& account, const String& password, Region region);
    StepResult loginEmailCode(const String& account, const String& code,    Region region);
    StepResult loginTfa(const String& tfaKey, const String& tfaCode,        Region region);

    // Stand-alone token check — calls /v1/user-service/my/profile and
    // returns true on HTTP 200. Used both for "verify the token I just
    // got" and "is the stored token still valid".
    bool verifyToken(const String& token, Region region);

    // ── Persistence ─────────────────────────────────────────
    void saveToken(const String& token, Region region, const String& email);
    void clearToken();

    bool          haveToken() const { return _token.length() > 0; }
    const String& token()     const { return _token; }
    const String& email()     const { return _email; }
    Region        region()    const { return _region; }

    // ── JWT helpers ─────────────────────────────────────────
    // Returns the `exp` Unix timestamp from a JWT payload, or 0 if the
    // token is malformed or the claim is missing. No signature check.
    static uint32_t decodeExpiry(const String& jwt);

    // ── Region helpers ──────────────────────────────────────
    static Region        regionFromString(const String& s);
    static const char*   regionToString(Region r);
    static const char*   regionBaseUrl(Region r);

private:
    String  _token;
    String  _email;
    Region  _region = Region::Global;

    // Underlying HTTPS POST/GET against api.bambulab.{com,cn}. Uses
    // WiFiClientSecure with the system cert bundle. Returns an empty
    // String on transport failure; the JSON status_code is captured
    // out-of-band via `lastHttpStatus`. `headersOut` is filled with
    // a "Name: value\n…" dump of the headers we collected (used by
    // the diagnostics-on-error UI path).
    int     _lastHttpStatus = 0;
    String  _post(Region r, const char* path, const String& jsonBody,
                  String* setCookieOut = nullptr,
                  String* headersOut   = nullptr);
    String  _get (Region r, const char* path, const String& bearerToken);

    // Common header helpers (mirror the Python ref's User-Agent etc.).
    void    _applyDefaultHeaders(class HTTPClient& http);
};

extern BambuCloudAuth g_bambu_cloud;
