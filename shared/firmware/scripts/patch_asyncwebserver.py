"""Pre-build patch: cap ESPAsyncWebServer's form-body parser.

For an `application/x-www-form-urlencoded` (or param-looking text/plain)
POST, ESPAsyncWebServer does NOT hand the body to our body callback — it
feeds it byte-by-byte into `_parsePlainPostChar`, which grows a String one
char at a time. This happens while the body is still arriving, before any
handler, auth check or middleware runs. A few hundred KB of form-encoded
body (e.g. `curl --data-binary @backup.tar` without a Content-Type) pins
the async_tcp task long enough for the task watchdog to panic the device.
Unauthenticated, any POST route, LAN-reachable. Present in 3.11.0 and
still in 3.12.1.

None of our routes read form-encoded POST params (uploads are multipart,
JSON bodies use the raw body callback), so we only let small bodies
(<= SPOOLHARD_MAX_FORM_BODY, default 8 KB) take the param-parsing path.
Anything larger goes to the handler's raw body callback like any other
content type, where our handlers enforce their own limits.

Idempotent. Fails the build if the upstream code it targets has moved, so
a library bump can't silently drop the protection.
"""
Import("env")
import os
import sys

# stderr, not stdout: CI runs `pio run --silent`, which swallows script stdout.
def log(msg):
    print(msg, file=sys.stderr)

MARKER = "SPOOLHARD_MAX_FORM_BODY"

DEFINE = """
// ── SpoolHard patch (shared/firmware/scripts/patch_asyncwebserver.py) ──
// Cap form-encoded POST bodies routed to the char-by-char param parser.
#ifndef SPOOLHARD_MAX_FORM_BODY
#define SPOOLHARD_MAX_FORM_BODY 8192
#endif
"""

REPLACEMENTS = [
    (
        "if (_contentType.startsWith(T_app_xform_urlencoded)) {",
        "if (_contentType.startsWith(T_app_xform_urlencoded) && _contentLength <= SPOOLHARD_MAX_FORM_BODY) {",
    ),
    (
        "} else if (_contentType == T_text_plain && isParamChar(((char *)buf)[0])) {",
        "} else if (_contentType == T_text_plain && _contentLength <= SPOOLHARD_MAX_FORM_BODY && isParamChar(((char *)buf)[0])) {",
    ),
]


def patch_asyncwebserver():
    lib_dir = env.subst("$PROJECT_LIBDEPS_DIR/$PIOENV/ESPAsyncWebServer")
    path = os.path.join(lib_dir, "src", "WebRequest.cpp")
    if not os.path.isfile(path):
        log("[patch_asyncwebserver] WebRequest.cpp not found (lib not installed yet?) — skipped")
        return
    with open(path) as f:
        src = f.read()
    if MARKER in src:
        return  # already patched

    for old, new in REPLACEMENTS:
        if src.count(old) != 1:
            raise SystemExit(
                "[patch_asyncwebserver] upstream code changed — anchor not found exactly once:\n  "
                + old
                + "\nRe-check the form-body DoS (see script header) and update the patch."
            )
        src = src.replace(old, new)

    # Insert the define after the last top-of-file #include.
    lines = src.split("\n")
    last_inc = max(i for i, l in enumerate(lines[:80]) if l.startswith("#include"))
    lines.insert(last_inc + 1, DEFINE)
    with open(path, "w") as f:
        f.write("\n".join(lines))
    log("[patch_asyncwebserver] capped form-body param parsing at SPOOLHARD_MAX_FORM_BODY")


patch_asyncwebserver()
