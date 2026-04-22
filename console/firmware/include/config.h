#pragma once

// ── Product identity ─────────────────────────────────────────
// Stamped into every built image via product_signature.h so the upload
// handler can reject a sibling product's firmware/frontend bundle.
#define PRODUCT_ID   "console"
#define PRODUCT_NAME "SpoolHard Console"

// ── Firmware / frontend versions ────────────────────────────
// Populated from console/VERSION at build time by scripts/patch_version.py.
// The fallbacks below only fire if the pre-script didn't run (ad-hoc build).
#ifndef FW_VERSION
  #define FW_VERSION "dev"
#endif
#ifndef FE_VERSION
  #define FE_VERSION "dev"
#endif

// ── Debug serial ─────────────────────────────────────────────
#define DEBUG_BAUD        115200

// ── Hardware: WT32-SC01-Plus ─────────────────────────────────
//
// Display: 8-bit parallel RGB (ESP32-S3 LCD_CAM), driven by LovyanGFX.
//   WR=47, RS/DC=0, D0..D7=9,46,3,8,18,17,16,15, RESET=4, BACKLIGHT=45.
//   Touch: FT5x06 on I²C — SDA=GPIO 6, SCL=GPIO 5.
// All of the above is configured in display.cpp's LGFX_Console class; listed
// here for reference only. Avoid reusing any of these GPIOs.

// ── Extended 8-pin JST 1.25mm header (PN532 wiring) ──────────
#define PN532_SS_PIN      10   // EXT_IO1
#define PN532_MOSI_PIN    11   // EXT_IO2
#define PN532_MISO_PIN    12   // EXT_IO3
#define PN532_SCK_PIN     13   // EXT_IO4
#define PN532_IRQ_PIN     14   // EXT_IO5
// EXT_IO6 (GPIO 21) is unused.

// ── microSD (onboard slot, per vendor schematic) ─────────────
#define SD_CS_PIN        41
#define SD_SCK_PIN       39
#define SD_MOSI_PIN      40
#define SD_MISO_PIN      38
#define SD_MOUNT         "/sd"

// ── NVS namespaces ──────────────────────────────────────────
#define NVS_NS_WIFI          "wifi_cfg"
#define NVS_KEY_SSID         "ssid"
#define NVS_KEY_PASS         "pass"
#define NVS_KEY_DEVICE_NAME  "device_name"
#define NVS_KEY_FIXED_KEY    "fixed_key"
#define DEFAULT_FIXED_KEY    "Change-Me!"

#define NVS_NS_OTA           "ota_cfg"
#define NVS_KEY_OTA_URL      "url"
#define NVS_KEY_OTA_USE_SSL  "use_ssl"
#define NVS_KEY_OTA_VERIFY   "verify_ssl"

// Scale link: the peer scale we pair with.
#define NVS_NS_SCALE         "scale_cfg"
#define NVS_KEY_SCALE_IP     "ip"          // last known IP (from SSDP)
#define NVS_KEY_SCALE_NAME   "name"        // USN we expect (user-set scale name)
#define NVS_KEY_SCALE_SECRET "secret"      // HMAC shared secret

// Printers (M2). Stored as a single JSON blob.
#define NVS_NS_PRINTERS      "printers_cfg"
#define NVS_KEY_PRINTERS_LIST "list"

// Display.
#define NVS_NS_DISPLAY         "display_cfg"
#define NVS_KEY_DISP_SLEEP_S   "sleep_s"
#define DEFAULT_DISP_SLEEP_S   120   // 2 min idle → screen off; 0 = never

// Empty-spool core-weight DB. One JSON blob keyed by
// "<brand>/<material>/<advertised>" → {grams, updated_ms}. Populated by the
// new-spool wizard when a full/empty measurement is taken; offered as a
// suggestion the next time the same spool class is registered.
#define NVS_NS_CORE_WEIGHTS    "core_weights"
#define NVS_KEY_CORE_MAP       "map"

// Quick-weights shortcuts for the wizard's "Full spool" step. JSON array of
// gram values (e.g. [1000, 2000, 5000]). Stored under console_cfg so it
// lives next to the other generic UI prefs.
#define NVS_NS_CONSOLE         "console_cfg"
#define NVS_KEY_QUICK_WEIGHTS  "quick_weights"
#define QUICK_WEIGHTS_MAX      6

// Generic misc settings.
#define NVS_NS_STORE         "store_cfg"

// ── OTA default URL ─────────────────────────────────────────
// Default OTA pull URL. Points directly at the firmware.bin asset of
// the latest GitHub Release of fabbarix/spool-hard. The device-side OTA
// flow currently fetches a single .bin; manifest-driven discovery (see
// docs/GITHUB_OTA_PLAN.md) will swap this for the manifest URL once
// implemented. Override per-device via Config → OTA URL on the web UI.
#define OTA_DEFAULT_URL   "https://github.com/fabbarix/spool-hard/releases/latest/download/spoolhard-console-firmware.bin"

// ── LittleFS (user data partition) ──────────────────────────
#define USERFS_LABEL         "userfs"
#define USERFS_MOUNT         "/userfs"
#define SPOOLS_DB_PATH       "/userfs/spools.jsonl"
#define TAGS_CACHE_PATH      "/userfs/tags_in_store.txt"

// ── SSDP / discovery ────────────────────────────────────────
#define SSDP_MCAST_OCT_A     239
#define SSDP_MCAST_OCT_B     255
#define SSDP_MCAST_OCT_C     255
#define SSDP_MCAST_OCT_D     250
// Bambu's wiki (https://wiki.bambulab.com/en/general/printer-network-ports) and
// community forum confirm their printers use NON-standard SSDP ports:
//   X1 / X1C / H2D → 1990   (same port the SpoolHard scale uses)
//   P1P            → 1900
// We listen on both so any Bambu on the LAN is caught.
#define SSDP_PORT            1990              // SpoolHard scale + Bambu X1/H2D
#define SSDP_PORT_UPNP       1900              // Bambu P1P (standard UPnP)
#define CONSOLE_SSDP_URN     "urn:spoolhard-io:device:console"
#define SCALE_SSDP_URN       "urn:spoolhard-io:device:spoolscale"
#define BAMBU_SSDP_URN_TAG   "bambulab"        // substring match on NT

// ── Scale WebSocket endpoint (scale is server, console is client) ──
#define SCALE_WS_PORT        81
#define SCALE_WS_PATH        "/ws"

// ── Bambu MQTT (M2) ─────────────────────────────────────────
#define BAMBU_MQTT_PORT      8883
#define BAMBU_MAX_PRINTERS   5
