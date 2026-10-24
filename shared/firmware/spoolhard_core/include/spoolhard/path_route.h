// Regex-free parameterised routes for ESPAsyncWebServer.
//
// Replaces `server.on("^\\/api\\/foo\\/(.+)\\/bar$", ...)` + `req->pathArg(0)`
// (which drags std::regex into the link, ~100 KB flash, plus per-request
// std::string/smatch heap churn). A PathRoute matches
//     <prefix><arg><suffix>
// where <arg> is 1..maxLen chars, optionally slash-free / digits-only, and
// the arg is recovered with spoolhard::pathArg(req, prefix, suffix).
//
// canHandle() is a plain predicate, so non-matching URLs fall through to the
// next registered handler exactly like the regex routes did (registration
// order = priority; register specific before generic). Over-long or
// malformed args don't match and end up in the not-found handler (404).
#pragma once
#include <ESPAsyncWebServer.h>

namespace spoolhard {

// Max accepted length of a path argument (ids, serials, seq numbers).
constexpr size_t PATH_ARG_MAX = 64;

inline bool pathArgMatch(const String& url, const char* prefix, const char* suffix,
                         bool allowSlash, bool digitsOnly, size_t maxLen,
                         size_t* argStart = nullptr, size_t* argLen = nullptr) {
    size_t pl = strlen(prefix), sl = strlen(suffix), ul = url.length();
    if (ul < pl + sl + 1) return false;
    if (strncmp(url.c_str(), prefix, pl) != 0) return false;
    if (sl && strcmp(url.c_str() + ul - sl, suffix) != 0) return false;
    size_t n = ul - pl - sl;
    if (n == 0 || n > maxLen) return false;
    const char* a = url.c_str() + pl;
    for (size_t i = 0; i < n; i++) {
        char c = a[i];
        if (!allowSlash && c == '/') return false;
        if (digitsOnly && (c < '0' || c > '9')) return false;
    }
    if (argStart) *argStart = pl;
    if (argLen) *argLen = n;
    return true;
}

// Extract the argument (empty String if the URL doesn't fit the pattern).
inline String pathArg(AsyncWebServerRequest* req, const char* prefix, const char* suffix = "",
                      bool allowSlash = true) {
    size_t s = 0, n = 0;
    if (!pathArgMatch(req->url(), prefix, suffix, allowSlash, false, PATH_ARG_MAX, &s, &n))
        return String();
    return req->url().substring(s, s + n);
}

class PathRoute : public AsyncWebHandler {
public:
    // `allowSlash`: regex `(.+)` -> true, `([^/]+)` -> false.
    PathRoute(WebRequestMethodComposite method, const char* prefix, const char* suffix,
              bool allowSlash, bool digitsOnly,
              ArRequestHandlerFunction onRequest, ArBodyHandlerFunction onBody = nullptr)
        : _method(method), _prefix(prefix), _suffix(suffix),
          _allowSlash(allowSlash), _digitsOnly(digitsOnly),
          _onRequest(onRequest), _onBody(onBody) {}

    bool canHandle(AsyncWebServerRequest* req) const override {
        if (!req->isHTTP() || !_method.matches(req->method())) return false;
        return pathArgMatch(req->url(), _prefix, _suffix, _allowSlash, _digitsOnly, PATH_ARG_MAX);
    }
    void handleRequest(AsyncWebServerRequest* req) override { if (_onRequest) _onRequest(req); }
    void handleBody(AsyncWebServerRequest* req, uint8_t* d, size_t l, size_t i, size_t t) override {
        if (_onBody) _onBody(req, d, l, i, t);
    }
    bool isRequestHandlerTrivial() const override { return !_onRequest; }

private:
    WebRequestMethodComposite _method;
    const char* _prefix;   // string literals only (not copied)
    const char* _suffix;
    bool _allowSlash, _digitsOnly;
    ArRequestHandlerFunction _onRequest;
    ArBodyHandlerFunction _onBody;
};

// Register a PathRoute (the server owns it). Strings must be literals.
inline void addPathRoute(AsyncWebServer& server, WebRequestMethodComposite method,
                         const char* prefix, const char* suffix, bool allowSlash, bool digitsOnly,
                         ArRequestHandlerFunction onRequest, ArBodyHandlerFunction onBody = nullptr) {
    server.addHandler(new PathRoute(method, prefix, suffix, allowSlash, digitsOnly, onRequest, onBody));
}

}  // namespace spoolhard
