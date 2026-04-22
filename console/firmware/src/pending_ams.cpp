#include "pending_ams.h"

namespace PendingAms {

namespace {
String   g_spool_id;
uint32_t g_expires_ms = 0;

bool expired() {
    return g_spool_id.isEmpty() || millis() > g_expires_ms;
}
}  // namespace

void arm(const String& spool_id, uint32_t expiry_ms) {
    g_spool_id   = spool_id;
    g_expires_ms = millis() + expiry_ms;
    Serial.printf("[PendingAms] armed for spool %s (%lu ms window)\n",
                  g_spool_id.c_str(), (unsigned long)expiry_ms);
}

bool claim(String& out_spool_id) {
    if (expired()) {
        if (!g_spool_id.isEmpty()) {
            Serial.printf("[PendingAms] pending for %s expired\n", g_spool_id.c_str());
            g_spool_id = "";
        }
        return false;
    }
    out_spool_id = g_spool_id;
    g_spool_id   = "";
    return true;
}

String peek() {
    if (expired()) return "";
    return g_spool_id;
}

void clear() {
    g_spool_id   = "";
    g_expires_ms = 0;
}

}  // namespace PendingAms
