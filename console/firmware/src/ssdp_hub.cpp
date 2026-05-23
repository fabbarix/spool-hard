#include "ssdp_hub.h"
#include "config.h"

SsdpListener g_ssdp_1990;
SsdpListener g_ssdp_1900;
SsdpListener g_ssdp_bambu_2021;

void ssdp_hub_begin() {
    g_ssdp_1990.begin(SSDP_PORT,            /*joinMulticast=*/true);
    g_ssdp_1900.begin(SSDP_PORT_UPNP,       /*joinMulticast=*/true);
    g_ssdp_bambu_2021.begin(BAMBU_BCAST_PORT, /*joinMulticast=*/false);
}

void ssdp_hub_probe() {
    // ssdp:all is the "tell me everything" wildcard ST. SpoolHard scales
    // respond on :1990; Chromecast/UPnP devices respond on :1900. Bambu
    // printers don't reliably answer M-SEARCH — they spam unsolicited
    // NOTIFY every ~30 s instead — but we still fire a broadcast probe on
    // :2021 in case some firmware revs do answer, since it's just two
    // bytes-of-the-wire of cost.
    g_ssdp_1990.sendMSearch("ssdp:all");
    g_ssdp_1900.sendMSearch("ssdp:all");
    g_ssdp_bambu_2021.sendMSearch("urn:bambulab-com:device:3dprinter:1");
}
