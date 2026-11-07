#pragma once

#include <M5Unified.h>

namespace uiFont {
void begin();
const lgfx::IFont* label();
const lgfx::IFont* title();
const lgfx::IFont* timer();
const lgfx::IFont* body(const String& message);
}
