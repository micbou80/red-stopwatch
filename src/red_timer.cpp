#include "red_timer.h"

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

namespace redTimer {
namespace {
constexpr uint32_t maxSeconds = 24 * 60 * 60;
constexpr int64_t second = 1000000;
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
State state = State::idle;
int64_t deadline = 0;
bool alarmPending = false;

Snapshot readAt(int64_t now) {
    if (state == State::running && now >= deadline) {
        state = State::finished;
        alarmPending = true;
    }
    const uint32_t left = state == State::running ? (deadline - now + second - 1) / second : 0;
    return {state, left};
}
}

Snapshot snapshot() {
    portENTER_CRITICAL(&lock);
    const Snapshot timer = readAt(esp_timer_get_time());
    portEXIT_CRITICAL(&lock);
    return timer;
}

Result apply(Action action, uint32_t seconds) {
    portENTER_CRITICAL(&lock);
    const int64_t now = esp_timer_get_time();
    const Snapshot before = readAt(now);
    const char* status = "ok";
    switch (action) {
        case Action::start:
            if (state == State::running) status = "already_running";
            else if (!seconds || seconds > maxSeconds) status = "invalid_duration";
            else {
                deadline = now + seconds * second;
                state = State::running;
                alarmPending = false;
            }
            break;
        case Action::extend:
            if (state != State::running) status = "no_running_timer";
            else if (!seconds || seconds > maxSeconds - before.seconds) status = "invalid_duration";
            else deadline += seconds * second;
            break;
        case Action::cancel:
            if (state == State::idle) status = "no_running_timer";
            state = State::idle;
            alarmPending = false;
            break;
        case Action::status:
            break;
        case Action::invalid:
            status = "invalid_action";
            break;
    }
    const Snapshot after = readAt(now);
    portEXIT_CRITICAL(&lock);
    return {status, after};
}

bool takeAlarm() {
    portENTER_CRITICAL(&lock);
    readAt(esp_timer_get_time());
    const bool pending = alarmPending;
    alarmPending = false;
    portEXIT_CRITICAL(&lock);
    return pending;
}
}
