#pragma once

#include <Arduino.h>

namespace wifiPicker {
void open(bool savedNetwork);
void close();
bool active();
void redraw();
bool update(bool next, bool select, bool back);
const String& selected();
}
