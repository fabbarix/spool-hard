#include "ssdp_listener.h"
#include "config.h"

static IPAddress mcast() { return IPAddress(SSDP_MCAST_OCT_A, SSDP_MCAST_OCT_B,
                                             SSDP_MCAST_OCT_C, SSDP_MCAST_OCT_D); }

void SsdpListener::begin(uint16_t port) {
    if (_running) {
        if (_port != port) {
            Serial.printf("[SSDP] begin(port=%u) ignored — already listening on %u\n",
                          port, _port);
        }
        return;
    }
    _port = port;
    if (_udp.listenMulticast(mcast(), port)) {
        Serial.printf("[SSDP] Listening on %s:%u\n", mcast().toString().c_str(), port);
        _udp.onPacket([this](AsyncUDPPacket pkt) { _onPacket(pkt); });
        _running = true;
    } else {
        Serial.printf("[SSDP] Failed to start listener on port %u\n", port);
    }
}

void SsdpListener::subscribe(Callback cb) {
    if (cb) _cbs.push_back(std::move(cb));
}

void SsdpListener::begin(Callback cb, uint16_t port) {
    begin(port);
    subscribe(std::move(cb));
}

void SsdpListener::stop() {
    _udp.close();
    _running = false;
    _cbs.clear();
}

void SsdpListener::_onPacket(AsyncUDPPacket& pkt) {
    if (_cbs.empty()) return;

    String text;
    text.reserve(pkt.length() + 1);
    for (size_t i = 0; i < pkt.length(); i++) text += (char)pkt.data()[i];

    Announce a;
    int start = 0;
    while (start < (int)text.length()) {
        int eol = text.indexOf('\n', start);
        if (eol < 0) eol = text.length();
        String line = text.substring(start, eol);
        line.trim();
        start = eol + 1;

        if (line.startsWith("NT:")) {
            a.urn = line.substring(3); a.urn.trim();
        } else if (line.startsWith("Location:")) {
            String loc = line.substring(9); loc.trim();
            a.ip.fromString(loc);
        } else if (line.startsWith("USN:")) {
            a.usn = line.substring(4); a.usn.trim();
        }
    }

    if (a.urn.length() && (uint32_t)a.ip != 0) {
        for (auto& cb : _cbs) cb(a);
    }
}
