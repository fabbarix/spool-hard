# SpoolHard tools

Helper scripts that run on a desktop, not on the device.

## bambu_login.py

Logs into Bambu Lab cloud and emits a paste-blob the SpoolHard console
can ingest.

The console firmware can't talk to `api.bambulab.com` directly because
Cloudflare hard-blocks the ESP32's TLS fingerprint (it's not a CAPTCHA,
it's a static "you have been blocked" page). Workaround: do the login
on a real computer (real browser-shaped TLS), then paste the resulting
token into the console.

```bash
python3 tools/bambu_login.py
```

The script asks for region (global / china), email, and password —
plus an email verification code or TFA code if your account requires
one. On success, the last line of output is:

```
SPOOLHARD-TOKEN:<base64-blob>
```

Copy that whole line, open the SpoolHard console's web UI →
**Config** → **Bambu Cloud** → expand **"I already have a token"**, and
paste it into the token field. The console detects the `SPOOLHARD-TOKEN:`
prefix and unpacks token + region + account label automatically.

A raw JWT (e.g. one you grabbed from OrcaSlicer's `~/.bambu_token`)
also works — paste it into the same field; the console tells the two
formats apart by the prefix.

Requires Python 3 + `requests`:

```bash
pip install requests
```
