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

    // Stand-alone token check — calls /v1/user-service/my/profile.
    // Returns Verified on HTTP 200. Returns Rejected on a clean 4xx
    // (the token is genuinely bad). Returns Unreachable when the
    // request never lands a verdict — network failure, timeout, OR a
    // Cloudflare hard-block (the WAF reject page). The Unreachable
    // case lets the manual-paste flow accept a token even when this
    // device's WAN is being blocked, with the UI flagging it as
    // "saved but unverified".
    enum class VerifyResult { Verified, Rejected, Unreachable };
    VerifyResult verifyToken(const String& token, Region region);

    // Generic auth'd HTTP helpers — exposed so sibling modules
    // (BambuCloudFilaments) can hit other api.bambulab.com endpoints
    // without re-implementing the browser-shaped headers + CF detector.
    // All three accept an explicit token + region (no implicit state)
    // and return a pre-classified result. `httpStatus == 0` means the
    // request never landed (transport error). `cfBlocked == true` means
    // the response body matched looksLikeCloudflareBlock.
    struct ApiResult {
        int    httpStatus = 0;
        String body;
        bool   cfBlocked  = false;
    };
    ApiResult apiGet   (Region r, const char* path, const String& bearer);
    ApiResult apiPost  (Region r, const char* path, const String& bearer,
                        const String& jsonBody);
    ApiResult apiDelete(Region r, const char* path, const String& bearer);

    // True if `body` looks like a Cloudflare hard-block reject page
    // (the static "Sorry, you have been blocked" template Cloudflare
    // serves when bot management denies the TLS handshake or request).
    // Extracts the Ray ID into `rayIdOut` if present + non-null.
    static bool looksLikeCloudflareBlock(const String& body, String* rayIdOut = nullptr);

    // Unpack a SPOOLHARD-TOKEN: paste-blob produced by
    // tools/bambu_login.py. Returns true on success and fills out the
    // three reference args; returns false on missing prefix, bad
    // base64, or missing JSON fields.
    static bool unpackTokenBlob(const String& pasted,
                                String& tokenOut,
                                String& regionOut,
                                String& emailOut);

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

    // Underlying HTTPS POST/GET. `useWebsiteHost` switches between the
    // two Bambu hosts: false → api.bambulab.{com,cn} (user-service +
    // profile endpoints), true → bambulab.{com,cn} (website /api routes
    // — TFA lives here, not on the user-service host). Returns an empty
    // String on transport failure; the HTTP status_code is captured
    // out-of-band via `lastHttpStatus`. `headersOut` is filled with
    // a "Name: value\n…" dump of the headers we collected (used by
    // the diagnostics-on-error UI path).
    int     _lastHttpStatus = 0;
    String  _post(Region r, const char* path, const String& jsonBody,
                  bool   useWebsiteHost = false,
                  String* setCookieOut  = nullptr,
                  String* headersOut    = nullptr);
    String  _get (Region r, const char* path, const String& bearerToken);
    static String _baseUrl(Region r, bool useWebsiteHost);

    // Common header helpers (mirror the Python ref's User-Agent etc.).
    void    _applyDefaultHeaders(class HTTPClient& http);
};

extern BambuCloudAuth g_bambu_cloud;
