#include "ssdp_hub.h"
#include "config.h"

SsdpListener g_ssdp_1990;
SsdpListener g_ssdp_1900;

void ssdp_hub_begin() {
    g_ssdp_1990.begin(SSDP_PORT);
    g_ssdp_1900.begin(SSDP_PORT_UPNP);
}
