#include "ssdp_hub.h"
#include "config.h"

SsdpListener g_ssdp_1990;
SsdpListener g_ssdp_1900;

void ssdp_hub_begin() {
    g_ssdp_1990.begin(SSDP_PORT);
    g_ssdp_1900.begin(SSDP_PORT_UPNP);
}

void ssdp_hub_probe() {
    // ssdp:all is the "tell me everything" wildcard ST. Bambu's SSDP
    // implementation isn't strictly UPnP (custom port 1990, custom URN)
    // but it does respond to broad probes — and using ssdp:all means we
    // also catch the SpoolHard scale on 1990 with one packet.
    g_ssdp_1990.sendMSearch("ssdp:all");
    g_ssdp_1900.sendMSearch("ssdp:all");
}
