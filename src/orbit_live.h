#pragma once

#include <Arduino.h>

namespace orbitLive {
void begin();
bool provision(const String& settings);
bool configured();
bool wifiConnected();
int wifiSignal();
bool ready();
bool start(int16_t* output, size_t capacity);
bool listening();
bool failed();
bool canRetryTurn();
bool canAppend();
bool append(const int16_t* samples, size_t count);
bool requestReply();
bool hasSpokenOutput();
size_t available();
bool outputFull();
unsigned long lastReplyActivity();
void close();
void closeAsync();
bool closing();
bool waitingForBackend();
bool timerViewRequested();
String caption();
String inputCaption();
void rememberTurn(String answer);
}
