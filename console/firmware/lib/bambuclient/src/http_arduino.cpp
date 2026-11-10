#include "bambuclient/appcert.hpp"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

namespace bambuclient {

bool arduino_http_get(const std::string& url, long* status,
                      std::string* body, std::string* err)
{
    WiFiClientSecure client;
    client.setInsecure();   // no Bambu CA bundle shipped; same as bambu_cloud.cpp
    HTTPClient http;
    http.setTimeout(15000);
    http.setReuse(false);
    if (!http.begin(client, url.c_str())) {
        if (err) *err = "cannot start request";
        return false;
    }
    http.addHeader("User-Agent", "OBN/02.06.00.99");
    http.addHeader("Accept", "application/json");
    const int code = http.GET();
    if (code <= 0) {
        if (err) *err = std::string(HTTPClient::errorToString(code).c_str());
        http.end();
        return false;
    }
    *status = code;
    String b = http.getString();
    body->assign(b.c_str(), b.length());
    http.end();
    return true;
}

} // namespace bambuclient
