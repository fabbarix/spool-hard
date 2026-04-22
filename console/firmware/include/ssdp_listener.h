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
    };

    using Callback = std::function<void(const Announce&)>;

    /// Bind the underlying multicast socket. Idempotent: calling again with
    /// the same port is a no-op, which makes this safe to use as a shared
    /// hub that multiple modules call.
    void begin(uint16_t port = SSDP_PORT);

    /// Add a callback. Every registered callback is invoked for every
    /// received NOTIFY; each callback is expected to filter by URN.
    void subscribe(Callback cb);

    /// Back-compat convenience: begin(port) + subscribe(cb) in one shot.
    void begin(Callback cb, uint16_t port = SSDP_PORT);

    void stop();

private:
    AsyncUDP _udp;
    std::vector<Callback> _cbs;
    uint16_t _port = 0;
    bool _running  = false;

    void _onPacket(AsyncUDPPacket& pkt);
};
