#pragma once
#include "ssdp_listener.h"

// Shared SSDP listeners owned by main.cpp. AsyncUDP only allows one socket
// per (multicast-group, port), so any module that wants to hear NOTIFY
// packets subscribes to these singletons via SsdpListener::subscribe(cb)
// instead of trying to open its own socket.
//
// Port 1990 catches the SpoolHard scale and Bambu X1/X1C/H2D.
// Port 1900 catches Bambu P1P (which uses standard UPnP).
extern SsdpListener g_ssdp_1990;
extern SsdpListener g_ssdp_1900;

// Call once after WiFi comes up. Safe to call repeatedly; idempotent.
void ssdp_hub_begin();

// Fire an M-SEARCH probe out both ports so devices reply immediately
// instead of waiting for their next periodic NOTIFY (~30 s on Bambu).
// Cheap, but don't spam — call on user action or on a slow timer.
void ssdp_hub_probe();
