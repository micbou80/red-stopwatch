#include "ui_font.h"

extern const uint8_t labelStart[] asm("_binary_assets_roboto_16_vlw_start");
extern const uint8_t labelEnd[] asm("_binary_assets_roboto_16_vlw_end");
extern const uint8_t titleStart[] asm("_binary_assets_roboto_24_vlw_start");
extern const uint8_t titleEnd[] asm("_binary_assets_roboto_24_vlw_end");
extern const uint8_t bodyStart[] asm("_binary_assets_roboto_32_vlw_start");
extern const uint8_t bodyEnd[] asm("_binary_assets_roboto_32_vlw_end");
extern const uint8_t timerFontStart[] asm("_binary_assets_roboto_timer_vlw_start");
extern const uint8_t timerFontEnd[] asm("_binary_assets_roboto_timer_vlw_end");

namespace uiFont {
namespace {
struct Font {
    lgfx::PointerWrapper data;
    lgfx::VLWfont face;
    bool ready = false;

    void load(const uint8_t* start, const uint8_t* end) {
        data.set(start, end - start);
        ready = face.loadFont(&data);
    }

    const lgfx::IFont* get(const lgfx::IFont* fallback) const {
        return ready ? &face : fallback;
    }
};

Font labelFont;
Font titleFont;
Font bodyFont;
Font timerFont;

bool hasBodyGlyphs(const String& message) {
    if (!bodyFont.ready) return false;
    const auto* byte = reinterpret_cast<const uint8_t*>(message.c_str());
    while (*byte) {
        uint16_t code = *byte++;
        if (code == '\n' || code == '\r' || code == '\t') continue;
        if (code >= 0x80) {
            if (code < 0xc2 || code >= 0xf0) return false;
            int remaining = code < 0xe0 ? 1 : 2;
            code &= remaining == 1 ? 0x1f : 0x0f;
            while (remaining--) {
                if ((*byte & 0xc0) != 0x80) return false;
                code = (code << 6) | (*byte++ & 0x3f);
            }
        }
        uint16_t index;
        if (!bodyFont.face.getUnicodeIndex(code, &index)) return false;
    }
    return true;
}
}

void begin() {
    labelFont.load(labelStart, labelEnd);
    titleFont.load(titleStart, titleEnd);
    bodyFont.load(bodyStart, bodyEnd);
    timerFont.load(timerFontStart, timerFontEnd);
    Serial.printf("ORBIT UI FONTS 16:%d 24:%d 32:%d timer:%d\n",
                  labelFont.ready, titleFont.ready, bodyFont.ready, timerFont.ready);
}

const lgfx::IFont* label() { return labelFont.get(&fonts::Font2); }
const lgfx::IFont* title() { return titleFont.get(&fonts::Font4); }
const lgfx::IFont* timer() { return timerFont.get(&fonts::Font7); }
const lgfx::IFont* body(const String& message) {
    if (hasBodyGlyphs(message)) return &bodyFont.face;
    return &fonts::efontCN_24;
}
}
