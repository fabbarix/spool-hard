#pragma once

// Redirect mbedtls' calloc/free to PSRAM with a DRAM fallback.
//
// arduino-esp32's mbedtls 2.28 keeps ~32 KB of in+out buffers per ssl_context
// pinned for the lifetime of the connection. With several concurrent TLS
// users (MQTT to each Bambu printer, OTA HTTPS, Bambu Cloud REST, FTPS print
// analysis) that adds up to far more contiguous internal DRAM than the
// ESP32-S3 has free mid-session — onboarding flows that fire a Bambu Cloud
// fetch on top of the always-on MQTT link will tip internal heap over and
// AsyncTCP starts dropping connections.
//
// PSRAM has ~2 MB free, so we route mbedtls allocations through there. AES
// in PSRAM is ~4-5x slower but the throughput numbers are still fine for
// our payload sizes. Override is global and idempotent.
//
// MUST be called before any WiFiClientSecure / mbedtls_*_init — i.e. before
// MQTT/OTA/cloud/FTPS first run. Safe to call multiple times.
void mbedtls_install_psram_alloc();
