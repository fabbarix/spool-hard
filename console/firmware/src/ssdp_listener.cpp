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

void SsdpListener::sendMSearch(const char* st) {
    if (!_running || !st) return;
    // RFC-style M-SEARCH. MX=1 keeps the response window short so we don't
    // wait long after the user taps Refresh. HOST must literally name the
    // multicast group + port we're aiming at.
    char pkt[256];
    int n = snprintf(pkt, sizeof(pkt),
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: %u.%u.%u.%u:%u\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 1\r\n"
        "ST: %s\r\n"
        "\r\n",
        SSDP_MCAST_OCT_A, SSDP_MCAST_OCT_B,
        SSDP_MCAST_OCT_C, SSDP_MCAST_OCT_D,
        _port, st);
    if (n <= 0 || n >= (int)sizeof(pkt)) return;
    _udp.writeTo(reinterpret_cast<const uint8_t*>(pkt), n, mcast(), _port);
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

        // NT: comes from device-pushed NOTIFY frames; ST: comes from
        // unicast HTTP/1.1 200 OK replies to our own M-SEARCH probes.
        // Both carry the URN; the rest of the parse is identical.
        if (line.startsWith("NT:")) {
            a.urn = line.substring(3); a.urn.trim();
        } else if (line.startsWith("ST:")) {
            a.urn = line.substring(3); a.urn.trim();
        } else if (line.startsWith("Location:")) {
            // Bambu's NOTIFY puts a bare IPv4 here, but standard SSDP
            // (and our own M-SEARCH responses) put a URL like
            // "http://192.168.1.42:8080/desc.xml". Strip scheme/path so
            // IPAddress::fromString can swallow either form.
            String loc = line.substring(9); loc.trim();
            int s = loc.indexOf("://");
            if (s >= 0) loc = loc.substring(s + 3);
            int slash = loc.indexOf('/');
            if (slash >= 0) loc = loc.substring(0, slash);
            int colon = loc.indexOf(':');
            if (colon >= 0) loc = loc.substring(0, colon);
            a.ip.fromString(loc);
        } else if (line.startsWith("USN:")) {
            a.usn = line.substring(4); a.usn.trim();
        }
    }

    if (a.urn.length() && (uint32_t)a.ip != 0) {
        for (auto& cb : _cbs) cb(a);
    }
}
