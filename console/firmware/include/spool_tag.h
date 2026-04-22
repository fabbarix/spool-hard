#pragma once
#include <Arduino.h>

// Parsed NFC tag as surfaced to the rest of the firmware. Populated by
// nfc_reader.cpp after reading + decoding whatever format the tag is in.
struct SpoolTag {
    String uid_hex;           // e.g. "04A1B2C3D4E5F6"
    String tag_type;          // NTAG215, Mifare1K, …
    String format;             // SpoolHardV1, SpoolHardV2, BambuLab, OpenPrintTag, Unknown
    String ndef_url;           // full URL if NDEF URI record (else "")

    // Optional parsed fields pulled out of the URL query params. Empty if the
    // tag format doesn't expose them.
    String parsed_material;
    String parsed_brand;
    String parsed_color_hex;

    // Populate `format` / `parsed_*` from an NDEF URL. Safe to call with an
    // empty URL (no-op). Shared by the local PN532 reader and the scale-
    // forwarded tag pipeline — both feed raw NDEF URLs; both need the same
    // interpretation.
    static void parseUrl(const String& url, SpoolTag& out);
};
