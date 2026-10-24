#pragma once
#include <Arduino.h>
#include <Print.h>
#include <vector>

// Print sink that appends into a std::vector<uint8_t> by pointer. Lets
// `serializeJson(env, sink)` populate a WsBufferPool slot in-place
// without an intermediate String. Header-only; shared by both products'
// broadcastDebug/broadcastState paths.
struct VectorPrint : public Print {
    std::vector<uint8_t>* v;
    explicit VectorPrint(std::vector<uint8_t>* dst) : v(dst) {}
    size_t write(uint8_t b) override {
        v->push_back(b);
        return 1;
    }
    size_t write(const uint8_t* buffer, size_t size) override {
        v->insert(v->end(), buffer, buffer + size);
        return size;
    }
};
