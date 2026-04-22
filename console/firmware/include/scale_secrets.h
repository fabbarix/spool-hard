#pragma once
#include <Arduino.h>

/**
 * Per-scale shared secret storage. Each paired scale can have its own
 * HMAC key (the user sets it on the scale's own Security config page and
 * mirrors it in the console). Secrets live as a JSON map keyed by scale
 * name, stored under a single NVS entry in NVS_NS_SCALE to avoid the 15-
 * character NVS-key limit.
 */
namespace ScaleSecrets {
  /// Returns the stored secret for `scaleName`, or "" if none.
  String get(const String& scaleName);

  /// Stores the secret for `scaleName`. Pass an empty string to remove it.
  void set(const String& scaleName, const String& secret);

  /// Returns a two-character-plus-stars preview ("ab****yz") or "" if the
  /// secret is empty. Useful for UI previews.
  String preview(const String& scaleName);

  /// `true` if a non-empty secret is stored for this scale.
  bool configured(const String& scaleName);
}
