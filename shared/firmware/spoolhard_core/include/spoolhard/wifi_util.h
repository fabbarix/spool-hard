#pragma once
#include <Arduino.h>

// WiFi helpers shared by both products' WifiProvisioning classes. The
// state machine itself stays per-product (timeouts, discovery hooks and
// the captive-portal HTML differ); everything here was byte-identical
// between them. NVS schema: namespace "wifi_cfg", keys per auth.h plus
// "pinned_bssid" (kNvsKeyPinnedBssid below).
namespace SpoolhardWifi {

constexpr const char* kNvsKeyPinnedBssid = "pinned_bssid";

// WIFI_REASON_* -> short tag for the ring log (diagnostic only).
const char* reasonTag(uint8_t r);

// Register the WiFi event logger (CONNECTED/DISCONNECTED with BSSID +
// reason, GOT_IP). Call before the first WiFi.begin().
void installEventLogger();

// Parse "AA:BB:CC:DD:EE:FF" into 6 raw bytes. Returns true on success.
bool parseBssid(const String& s, uint8_t out[6]);

// Start the provisioning AP "<prefix><MAC tail>" in AP_STA mode and kick
// an async scan. Returns the AP SSID.
String startProvisioningAp(const char* prefix);

// Tear down the provisioning AP, back to STA-only.
void stopProvisioningAp();

// Read wifi_cfg/pinned_bssid ("" if none); logs when set.
String loadPinnedBssid();

// Persist ssid/pass (+ optional name / pinned BSSID) to wifi_cfg. A
// non-empty `name` updates `deviceName`; a non-null `pinnedBssid`
// updates `pinnedBssidOut` (empty string clears the pin).
void saveCredentials(const String& ssid, const String& pass, const String& name,
                     const String* pinnedBssid, String& deviceName, String& pinnedBssidOut);

}  // namespace SpoolhardWifi
