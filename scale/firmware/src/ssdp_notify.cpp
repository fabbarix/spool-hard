#include "ssdp_notify.h"
#include <WiFi.h>

static const IPAddress SPOOLHARD_MCAST(239, 255, 255, 250);
static constexpr uint16_t SPOOLHARD_PORT = 1990;

void SsdpNotify::begin(const String& deviceName, uint32_t intervalMs) {
    _deviceName = deviceName;
    _intervalMs = intervalMs;
    _lastSend   = 0;
    // AsyncUDP sends multicast without requiring a listener bind.
    Serial.printf("[SSDP] Will advertise as '%s' to %s:%u every %lums\n",
                  _deviceName.c_str(), SPOOLHARD_MCAST.toString().c_str(),
                  SPOOLHARD_PORT, (unsigned long)_intervalMs);
}

void SsdpNotify::loop() {
    if (WiFi.status() != WL_CONNECTED) return;
    uint32_t now = millis();
    if (now - _lastSend < _intervalMs && _lastSend != 0) return;
    _lastSend = now;
    _sendNotify();
}

void SsdpNotify::_sendNotify() {
    IPAddress ip = WiFi.localIP();
    if (ip == IPAddress(0, 0, 0, 0)) return;

    // Headers are parsed by splitting each line on the first ' ' and matching
    // the first token exactly. Keep spellings ("NT:", "Location:", "USN:") — the
    // console's parser is case-sensitive on these three.
    char pkt[256];
    int n = snprintf(pkt, sizeof(pkt),
        "NOTIFY * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:%u\r\n"
        "NT: urn:spoolhard-io:device:spoolscale\r\n"
        "Location: %s\r\n"
        "USN: %s\r\n"
        "\r\n",
        SPOOLHARD_PORT,
        ip.toString().c_str(),
        _deviceName.c_str());
    if (n <= 0 || n >= (int)sizeof(pkt)) {
        Serial.println("[SSDP] packet build failed");
        return;
    }

    AsyncUDPMessage msg(n);
    msg.write((const uint8_t*)pkt, n);
    _udp.sendTo(msg, SPOOLHARD_MCAST, SPOOLHARD_PORT);
}
