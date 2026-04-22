#pragma once
#include <Arduino.h>
#include <vector>

// Empty-spool-core weight database.
//
// When a user registers a full or empty spool through the on-device wizard
// we auto-learn the empty-core weight for that (brand, material, advertised
// filament weight) triple. Next time a spool of the same class is being
// measured as "used", the wizard can pre-fill the core weight from this DB
// instead of making the user enter it by hand.
//
// Storage: a single JSON object in NVS, keyed by
//   "<brand>/<material>/<advertised>"
// value =
//   { "grams": <int>, "updated_ms": <uint32> }
//
// NVS namespace / key are defined in config.h. This mirrors the ScaleSecrets
// helper (scale_secrets.cpp) — one blob, read-modify-write on every change,
// which is plenty for a table that holds at most a few dozen rows.
namespace CoreWeights {

struct Entry {
    String   brand;
    String   material;
    int      advertised;    // grams of filament on the spool as packaged
    int      grams;         // measured empty-core weight
    uint32_t updated_ms;    // millis() at last write (for UI "updated N ago")
};

// Build the lookup key from the triple. Exposed so the web API can
// delete a row by the same key the firmware uses internally.
String keyFor(const String& brand, const String& material, int advertised);

// Look up an entry. Returns the grams value, or -1 if the triple isn't
// present in the DB.
int get(const String& brand, const String& material, int advertised);

// Upsert. grams < 0 removes the entry instead.
void set(const String& brand, const String& material, int advertised, int grams);

// Remove by full key (as produced by keyFor).
bool removeKey(const String& key);

// Return every entry currently stored. Order is unspecified.
std::vector<Entry> list();

} // namespace CoreWeights
