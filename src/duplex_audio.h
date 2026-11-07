#pragma once

#include <Arduino.h>

namespace duplexAudio {
using Release = void (*)(int16_t*, size_t);

bool begin(Release release);
void end();
bool active();
bool record(int16_t* samples, size_t count, size_t progress = 0);
void clearInput();
bool play(const int16_t* samples, size_t count, uint8_t volume);
unsigned queuedOutput();
bool drained();
bool interruptible();
void stopOutput();
}
