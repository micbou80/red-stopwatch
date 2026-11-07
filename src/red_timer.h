#pragma once

#include <cstdint>

namespace redTimer {
enum class State { idle, running, finished };
enum class Action { status, start, extend, cancel, invalid };

struct Snapshot {
    State state;
    uint32_t seconds;
};

struct Result {
    const char* status;
    Snapshot timer;
};

Snapshot snapshot();
Result apply(Action action, uint32_t seconds);
bool takeAlarm();
}
