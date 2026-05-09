// Wi-Fi air sniffer for the console<->scale link — PCAP streaming variant.
//
// Locks to 2.4 GHz channel 6 in promiscuous mode and emits PCAP-formatted
// (linktype 105 = LINKTYPE_IEEE802_11) frames straight to USB CDC. Host
// reads /dev/ttyACM0 as a binary stream into a .pcap file and opens it
// in Wireshark — with the SSID + PSK configured under Edit > Preferences
// > Protocols > IEEE 802.11, decryption "Enabled" — to get the cleartext
// TCP layer.
//
// Filter:
//   - Always include beacons of frames we can see (so Wireshark can map
//     the BSSID -> SSID for decryption key derivation).
//   - Include all DATA / MGMT frames where any address slot equals the
//     scale or console MAC (catches association, EAPOL 4-way handshake,
//     and all unicast traffic).
//   - Drop probe-requests / probe-responses (noise).
//
// To decrypt, both clients' PTKs must be derivable. That requires the
// EAPOL 4-way handshake of each. For scale/console that have been
// associated for hours, that handshake happened long before this capture
// — so the host script should trigger a fresh association by POSTing
// /api/restart to each device while this sniffer is already running.

#include <Arduino.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

static const uint8_t SCALE[6]   = {0x1c, 0xdb, 0xd4, 0x47, 0x04, 0x0c};
static const uint8_t CONSOLE[6] = {0xa8, 0x46, 0x74, 0xb6, 0x23, 0x10};
static const uint8_t SNIFF_CHANNEL = 6;

struct __attribute__((packed)) PcapHeader {
    uint32_t magic;       // 0xa1b2c3d4 = µs-resolution timestamps
    uint16_t v_major;
    uint16_t v_minor;
    int32_t  thiszone;
    uint32_t sigfigs;
    uint32_t snaplen;
    uint32_t linktype;
};

struct __attribute__((packed)) PcapRecord {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t incl_len;
    uint32_t orig_len;
};

// Producer (Wi-Fi promisc CB) → Consumer (writer task) ring buffer.
// Each entry: PcapRecord (16) + frame bytes (≤ 2300). Oversized to absorb
// USB CDC stalls; if we overrun, we count drops and continue.
static RingbufHandle_t  g_rb        = nullptr;
static const size_t     RB_SIZE     = 64 * 1024;
static volatile uint32_t g_dropped  = 0;
static volatile uint32_t g_kept     = 0;
// Streaming flag — host must send any byte before we begin emitting.
// Until then, the promisc CB drops frames so we don't fill the ring with
// pre-arm garbage.
static volatile bool     g_streaming = false;

static inline bool macEq(const uint8_t* a, const uint8_t* b) {
    return memcmp(a, b, 6) == 0;
}
static inline bool involves(const uint8_t* a1, const uint8_t* a2,
                            const uint8_t* a3, const uint8_t* m) {
    return macEq(a1, m) || macEq(a2, m) || macEq(a3, m);
}

static void IRAM_ATTR sniff_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_DATA && type != WIFI_PKT_MGMT) return;

    const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;
    int len = pkt->rx_ctrl.sig_len;
    if (len < 24 || len > 2300) return;

    const uint8_t* p = pkt->payload;
    uint16_t fc       = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    uint8_t  ftype    = (fc >> 2) & 0x3;
    uint8_t  fsubtype = (fc >> 4) & 0xF;
    const uint8_t* a1 = p + 4;
    const uint8_t* a2 = p + 10;
    const uint8_t* a3 = p + 16;

    bool keep = false;
    if (ftype == 0) {
        if (fsubtype == 4 || fsubtype == 5) return;   // probe req/resp
        if (fsubtype == 8) keep = true;               // beacon (BSS context)
        else keep = involves(a1, a2, a3, SCALE) || involves(a1, a2, a3, CONSOLE);
    } else if (ftype == 2) {
        keep = involves(a1, a2, a3, SCALE) || involves(a1, a2, a3, CONSOLE);
    }
    if (!keep) return;
    if (!g_streaming) return;

    int64_t now_us = esp_timer_get_time();
    PcapRecord r;
    r.ts_sec   = (uint32_t)(now_us / 1000000);
    r.ts_usec  = (uint32_t)(now_us % 1000000);
    r.incl_len = (uint32_t)len;
    r.orig_len = (uint32_t)len;

    // Stage record + payload into a contiguous buffer, then ringbuf-send
    // as a single item so the writer task gets the whole frame atomically.
    uint8_t scratch[sizeof(PcapRecord) + 2300];
    memcpy(scratch, &r, sizeof(r));
    memcpy(scratch + sizeof(r), p, len);
    BaseType_t ok = xRingbufferSend(g_rb, scratch, sizeof(r) + len, 0);
    if (ok == pdTRUE) g_kept++;
    else              g_dropped++;
}

static void writerTask(void*) {
    // Wait for host to send any byte — a deterministic "GO" — before we
    // emit the pcap global header. Without this rendezvous, the host has
    // no way to anchor on the magic if it happened to attach mid-stream.
    while (!Serial.available()) vTaskDelay(pdMS_TO_TICKS(20));
    while (Serial.available()) Serial.read();

    PcapHeader h{};
    h.magic    = 0xa1b2c3d4;
    h.v_major  = 2;
    h.v_minor  = 4;
    h.thiszone = 0;
    h.sigfigs  = 0;
    h.snaplen  = 65535;
    h.linktype = 105;   // LINKTYPE_IEEE802_11
    Serial.write((const uint8_t*)&h, sizeof(h));
    g_streaming = true;

    for (;;) {
        size_t  itemSize = 0;
        uint8_t* item = (uint8_t*)xRingbufferReceive(g_rb, &itemSize, portMAX_DELAY);
        if (!item) continue;
        Serial.write(item, itemSize);
        vRingbufferReturnItem(g_rb, item);
    }
}

void setup() {
    // Native USB CDC at the OS-default speed; the speed argument is ignored
    // for native USB but kept for the API.
    Serial.begin(921600);
    // No setup-time output — writerTask blocks until the host says GO,
    // then writes the pcap header. Wi-Fi can come up immediately; the
    // promisc CB drops frames until streaming is armed.

    g_rb = xRingbufferCreate(RB_SIZE, RINGBUF_TYPE_NOSPLIT);
    xTaskCreatePinnedToCore(writerTask, "pcap_writer", 4096, nullptr, 5,
                            nullptr, 1);

    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_NULL);
    esp_wifi_start();

    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_DATA | WIFI_PROMIS_FILTER_MASK_MGMT
    };
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(&sniff_cb);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(SNIFF_CHANNEL, WIFI_SECOND_CHAN_NONE);
}

void loop() {
    // No textual output — would corrupt the binary pcap stream.
    delay(60000);
}
