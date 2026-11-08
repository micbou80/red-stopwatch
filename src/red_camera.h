#pragma once

#include <Arduino.h>
#include <memory>

namespace redCamera {
struct Snapshot {
    ~Snapshot() { free(data); }
    Snapshot() = default;
    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;

    uint8_t* data = nullptr;
    size_t size = 0;
    uint16_t width = 0;
    uint16_t height = 0;
};

const char* fetch(const char* endpoint, const char* token, const char* lookupTool, bool returnsIds,
                  const char* name, const char* area, Snapshot& snapshot);
}
