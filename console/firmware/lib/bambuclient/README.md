# bambuclient (vendored)

Source: `/opt/mrwho/Projects/bambulab/bambuclient` (initial commit b9429a7).
Only the `appcert_fetch` part is imported. Differences from upstream:

- OpenSSL -> mbedtls (AES-GCM seal, RSA-PKCS#1 v1.5 wrap, X.509 parse,
  key/cert pair check, RSA key rebuild from p/q via `mbedtls_rsa_complete`).
- libcurl -> `HttpGetFn` transport; default is Arduino `HTTPClient`.
- `json_lite` -> ArduinoJson (PSRAM allocator).
- No file storage helpers and no `fetchconfig` CLI: the console stores the
  result through `BambuSigner::install`.
- No default secret. The caller (the user) always provides it.

Unchanged: request building, S-box tables, `ctr_xor`, base64 helpers,
`decode_key_blob`.
