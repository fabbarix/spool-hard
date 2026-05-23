#pragma once
#include <Arduino.h>
#include <AsyncUDP.h>
#include <IPAddress.h>
#include <functional>
#include <vector>
#include "config.h"     // SSDP_PORT default for begin()

// Listener for SSDP-style NOTIFY packets on UDP 239.255.255.250:<port>. Port
// varies by vendor — SpoolHard uses 1990, Bambu X1/X1C/H2D use 1990 too,
// Bambu P1P and standard UPnP use 1900. Multiple consumers can subscribe
// to the same listener; each callback is invoked for every packet and is
// expected to filter by URN.
class SsdpListener {
public:
    struct Announce {
        String urn;      // value after "NT: "
        IPAddress ip;    // value after "Location: " (bare IPv4)
        String usn;      // value after "USN: " (device name / id)
        // Bambu-specific extras (empty for non-Bambu announcements):
        String dev_name; // value after "DevName.bambu.com: "  (user-set, e.g. "Bambozzo")
        String dev_model;// value after "DevModel.bambu.com: " (e.g. "O1S", "X1C", "H2D")
    };

    using Callback = std::function<void(const Announce&)>;

    /// Bind the underlying UDP socket. Idempotent: calling again with the
    /// same port is a no-op, which makes this safe to use as a shared hub
    /// that multiple modules call.
    ///
    /// `joinMulticast=true` (default) joins 239.255.255.250 so we hear
    /// classic SSDP NOTIFY frames (SpoolHard scale on :1990, standard UPnP
    /// on :1900). `joinMulticast=false` skips the group join and listens
    /// only for broadcast/unicast on the port — needed for Bambu X1/P1/H2D
    /// which broadcast NOTIFY to 255.255.255.255:2021 rather than to the
    /// SSDP multicast group.
    void begin(uint16_t port = SSDP_PORT, bool joinMulticast = true);

    /// Add a callback. Every registered callback is invoked for every
    /// received NOTIFY; each callback is expected to filter by URN.
    void subscribe(Callback cb);

    /// Back-compat convenience: begin(port, true) + subscribe(cb) in one shot.
    void begin(Callback cb, uint16_t port = SSDP_PORT);

    /// Send an SSDP M-SEARCH probe out the bound multicast socket. Devices
    /// reply unicast back to the source port (which is the same port we're
    /// listening on), so the existing `_onPacket` path catches the responses.
    /// `st` is the search target — pass "ssdp:all" for a broad probe or a
    /// vendor-specific URN for a narrow one. No-op if the listener isn't
    /// running yet.
    void sendMSearch(const char* st);

    void stop();

private:
    AsyncUDP _udp;
    std::vector<Callback> _cbs;
    uint16_t _port      = 0;
    bool     _running   = false;
    bool     _multicast = true;

    void _onPacket(AsyncUDPPacket& pkt);
};
