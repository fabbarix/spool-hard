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


# ── Patch 2: bound the request line / header line length ───────────────────
# The parser accumulates a line in a heap String (_temp) until it sees '\n',
# with no limit: an endless request line or header line grows internal DRAM
# until reserve() fails. Abort (clean connection drop) once a single line
# exceeds SPOOLHARD_MAX_REQLINE (request line) / SPOOLHARD_MAX_HDRLINE
# (header lines; cookies / Authorization fit comfortably). Checked only in
# the "no newline in this chunk yet" branch, so overshoot is <= one TCP
# segment. The check is per TCP segment, not per
# byte: lines up to roughly limit + one segment (~1.4 KB) can still pass
# (observed: ~2.9 KB request line OK, 4 KB reset). Good enough as a DoS bound. Body parsing is untouched.
LINE_MARKER = "SPOOLHARD_MAX_REQLINE"
LINE_DEFINE = """
// ── SpoolHard patch: cap request/header line length ──
#ifndef SPOOLHARD_MAX_REQLINE
#define SPOOLHARD_MAX_REQLINE 2048
#endif
#ifndef SPOOLHARD_MAX_HDRLINE
#define SPOOLHARD_MAX_HDRLINE 8192
#endif
"""
LINE_OLD = """        if (!_temp.reserve(_temp.length() + len)) {"""
LINE_NEW = """        if (_temp.length() + len > (_parseState == PARSE_REQ_START ? SPOOLHARD_MAX_REQLINE : SPOOLHARD_MAX_HDRLINE)) {
          async_ws_log_e("Request/header line too long");
          _parseState = PARSE_REQ_FAIL;
          abort();
          return;
        }
        if (!_temp.reserve(_temp.length() + len)) {"""


def patch_line_limits():
    lib_dir = env.subst("$PROJECT_LIBDEPS_DIR/$PIOENV/ESPAsyncWebServer")
    path = os.path.join(lib_dir, "src", "WebRequest.cpp")
    if not os.path.isfile(path):
        return
    with open(path) as f:
        src = f.read()
    if LINE_MARKER in src:
        return
    if src.count(LINE_OLD) != 1:
        raise SystemExit(
            "[patch_asyncwebserver] upstream code changed — line-accumulate anchor not found exactly once:\n  "
            + LINE_OLD
        )
    src = src.replace(LINE_OLD, LINE_NEW)
    lines = src.split("\n")
    last_inc = max(i for i, l in enumerate(lines[:80]) if l.startswith("#include"))
    lines.insert(last_inc + 1, LINE_DEFINE)
    with open(path, "w") as f:
        f.write("\n".join(lines))
    log("[patch_asyncwebserver] capped request line at SPOOLHARD_MAX_REQLINE / header line at SPOOLHARD_MAX_HDRLINE")


patch_asyncwebserver()
patch_line_limits()
