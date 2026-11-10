// Minimal Arduino.h for host tests of code that only needs String.
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
struct String : std::string {
    String() = default;
    String(const char* s) : std::string(s ? s : "") {}
    String(const std::string& s) : std::string(s) {}
};
