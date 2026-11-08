#include <Arduino.h>
#include <ArduinoJson.h>
#include <M5Unified.h>
#include <M5PM1.h>
#include <Preferences.h>
#include <atomic>
#include <vector>
#include "bluetooth_setup.h"
#include "duplex_audio.h"
#include "orbit_live.h"
#include "red_timer.h"
#include "ui_font.h"
#include "wifi_picker.h"

extern const uint8_t flowerStart[] asm("_binary_assets_orbit_flower_png_start");
extern const uint8_t flowerEnd[] asm("_binary_assets_orbit_flower_png_end");

namespace {
constexpr char firmwareVersion[] = "FW 0.2.2";
constexpr int avatarTop = 86;
constexpr int avatarSize = 252;
constexpr int avatarBounceHeight = 8;
constexpr unsigned long avatarBounceMs = 280;
constexpr size_t maxLine = 768;
constexpr uint32_t sampleRate = 24000;
constexpr size_t captureLimit = sampleRate * 8;
constexpr size_t playbackSeconds = 30;
constexpr size_t playbackLimit = sampleRate * playbackSeconds;
constexpr unsigned long replyQuietMs = 2200;
constexpr unsigned long lookupTimeoutMs = 40000;
constexpr unsigned long replyTimeoutMs = lookupTimeoutMs + playbackSeconds * 1000;
constexpr unsigned long micChunkMs = 20;
constexpr size_t micChunk = sampleRate * micChunkMs / 1000;
constexpr size_t legacyMicBlock = 2 * micChunk;
constexpr size_t usbChunk = 1024;
constexpr size_t speakerChunk = sampleRate / 5;
constexpr size_t speakerPreroll = sampleRate * 80 / 1000;
constexpr unsigned long holdToTalkMs = 180;
constexpr unsigned long holdToHomeMs = 500;
constexpr unsigned long doubleClickMs = 380;
constexpr unsigned long settingsHoldMs = 750;
constexpr unsigned long micWarmupMs = 350;
constexpr unsigned long firstMicWarmupMs = 1100;
constexpr unsigned long speakerHandoffQuietMs = 500;
constexpr unsigned long silenceFallbackMs = 3500;
constexpr size_t standbyFrames = 12;
constexpr int conversationTop = 82;
constexpr int conversationWidth = 360;
constexpr int conversationHeight = 314;
constexpr int textLeft = 25;
constexpr int textTop = 1;
constexpr int textWidth = 310;
constexpr int lineHeight = 41;
constexpr int footerY = 288;
constexpr int visibleLines = (footerY - textTop) / lineHeight;
constexpr int dragLineHeight = 24;

enum class Mood { idle, listening, thinking, answer, error };
enum class Screen { home, transcript, answer, timer };
enum SettingsOption {
    settingsWifi, settingsBluetooth, settingsVolume, settingsDisplay, settingsHandsFree,
    settingsInterrupt, settingsBack, settingsCount
};
struct DisplayTimeout {
    const char* label;
    unsigned long durationMs;
};
constexpr DisplayTimeout displayTimeouts[] = {
    {"30 seconds", 30000}, {"1 minute", 60000}, {"5 minutes", 300000}, {"Never", 0}
};
constexpr uint8_t displayTimeoutCount = sizeof(displayTimeouts) / sizeof(displayTimeouts[0]);
constexpr unsigned long motionSampleMs = 25;
constexpr float pickupAccelG = 0.14f;
constexpr float pickupGyroDps = 25.0f;
Mood mood = Mood::idle;
Screen screen = Screen::home;
M5Canvas flower(&M5.Display);
M5Canvas conversation(&M5.Display);
bool flowerReady = false;
unsigned long avatarBounceAt = 0;
int avatarLift = 0;
int center = 0;
String line;
bool discardLine = false;
bool touchHeld = false;
bool touchConsumed = false;
bool scrollTouch = false;
int touchStartX = 0;
int touchStartY = 0;
int dragAnchorY = 0;
std::vector<String> replyLines;
const lgfx::IFont* replyFont = nullptr;
int firstReplyLine = 0;
bool replyStreaming = false;
String displayedReply;
unsigned long nextReplyCaptionAt = 0;
bool bridgeReady = false;
bool recording = false;
size_t micQueued = 0;
std::atomic<size_t> micCompleted{0};
bool playing = false;
bool directTurn = false;
bool directFinished = false;
bool pendingSession = false;
bool sessionRetried = false;
bool inputDrained = false;
size_t queuedSamples = 0;
int micPeak = 0;
unsigned long directStartedAt = 0;
unsigned long releasedAt = 0;
unsigned long buttonPressedAt = 0;
unsigned long recordingEndedAt = 0;
unsigned long nextSilenceAt = 0;
unsigned long nextInputAt = 0;
unsigned long releaseSeenAt = 0;
unsigned long nextCaptionAt = 0;
bool replyReminderSent = false;
String displayedInput;
const char* displayedInputStatus = nullptr;
int16_t* capture = nullptr;
int16_t* playback = nullptr;
size_t captured = 0;
size_t sent = 0;
size_t discardSamples = 0;
size_t playable = 0;
unsigned long micReadyAt = 0;
unsigned long thinkingSince = 0;
unsigned long speakerDrainSince = 0;
unsigned long onboardingAt = 0;
bool autoScan = true;
bool checkingCloud = false;
unsigned long cloudCheckStarted = 0;
unsigned long cloudNextFrame = 0;
unsigned int cloudFramesSent = 0;
bool hotMic = false;
bool voiceInterruptBeta = false;
bool micInitialized = false;
int16_t standby[standbyFrames][micChunk];
std::atomic<size_t> standbyCompleted{0};
size_t standbyQueued = 0;
size_t standbyRead = 0;
int noiseLevel = 80;
unsigned int voiceRun = 0;
unsigned int voicedFrames = 0;
unsigned int quietFrames = 0;
bool autoRecording = false;
bool autoPrimed = false;
unsigned long nextMicWarmupAt = 0;
unsigned long autoRearmAt = 0;
bool aHandled = false;
bool chordHeld = false;
unsigned long lastClickAt = 0;
bool settingsOpen = false;
int settingsItem = 0;
uint8_t speakerVolume = 100;
uint8_t displayTimeoutIndex = 0;
unsigned long displayActivityAt = 0;
bool displaySleeping = false;
unsigned long lastMotionSample = 0;
m5::IMU_Class::imu_3d_t restingAccel{};
bool restingAccelReady = false;
uint8_t pickupSamples = 0;
bool menuButtonReady = false;
int batteryPercent = -1;
bool batteryCharging = false;
int wifiBars = 0;
unsigned long nextStatusPoll = 0;
redTimer::Snapshot displayedTimer{redTimer::State::idle, 0};
bool timerChimePending = false;

void drawStatus();
void wakeDisplay();

void enterScreen(Screen next) {
    if (screen == next) return;
    screen = next;
    M5.Display.fillScreen(TFT_BLACK);
    drawStatus();
}

std::vector<String> wrapText(String remaining, const lgfx::IFont* font) {
    std::vector<String> lines;
    conversation.setTextSize(1);
    conversation.setFont(font);
    remaining.trim();
    while (!remaining.isEmpty()) {
        unsigned int end = 0;
        int space = -1;
        while (end < remaining.length() && remaining[end] != '\n') {
            unsigned int next = end + 1;
            while (next < remaining.length() && (static_cast<uint8_t>(remaining[next]) & 0xc0) == 0x80) ++next;
            if (end && conversation.textWidth(remaining.substring(0, next)) > textWidth) {
                if (space > 0 && remaining[end] != ' ') end = space;
                break;
            }
            if (remaining[end] == ' ') space = end;
            end = next;
        }
        lines.push_back(remaining.substring(0, end));
        remaining.remove(0, end);
        remaining.trim();
    }
    return lines;
}

int maxReplyScroll() {
    return max(0, static_cast<int>(replyLines.size()) - visibleLines);
}

void drawConversation(uint16_t accent, const std::vector<String>& lines,
                      int first, const char* footer, const lgfx::IFont* font, bool centered = false) {
    auto& panel = conversation;
    const bool isReply = screen == Screen::answer;
    panel.fillScreen(TFT_BLACK);
    panel.setTextSize(1);
    panel.setTextDatum(top_center);
    panel.setFont(font);
    panel.setTextColor(TFT_WHITE, TFT_BLACK);
    if (centered && static_cast<int>(lines.size()) <= visibleLines) {
        const int y = (textTop + footerY - static_cast<int>(lines.size()) * lineHeight) / 2;
        for (size_t row = 0; row < lines.size(); ++row) {
            panel.drawString(lines[row], conversationWidth / 2, y + row * lineHeight);
        }
    } else {
        panel.setTextDatum(top_left);
        const int count = static_cast<int>(lines.size());
        for (int row = 0; row < visibleLines && first + row < count; ++row) {
            panel.drawString(lines[first + row], textLeft, textTop + row * lineHeight);
        }
        if (isReply && count > visibleLines) {
            constexpr int trackHeight = visibleLines * lineHeight - 5;
            const int thumbHeight = max(9, trackHeight * visibleLines / count);
            const int thumbY = textTop + (trackHeight - thumbHeight) * first / (count - visibleLines);
            panel.drawFastVLine(conversationWidth - 14, textTop, trackHeight, TFT_DARKGREY);
            panel.fillRoundRect(conversationWidth - 15, thumbY, 3, thumbHeight, 1, accent);
        }
    }
    panel.setFont(uiFont::label());
    panel.setTextDatum(top_center);
    panel.setTextColor(isReply ? TFT_LIGHTGREY : accent, TFT_BLACK);
    panel.drawString(footer, conversationWidth / 2, footerY);
    panel.pushSprite(center - conversationWidth / 2, conversationTop);
}

void drawTranscript(const String& message, const char* footer, bool hasWords = false) {
    enterScreen(Screen::transcript);
    const auto* font = uiFont::body(message);
    const auto lines = wrapText(message, font);
    const int first = max(0, static_cast<int>(lines.size()) - visibleLines);
    const bool listening = mood == Mood::listening;
    const uint16_t accent = listening ? TFT_GREEN : mood == Mood::error ? TFT_ORANGE : TFT_CYAN;
    drawConversation(accent, lines, first, footer, font, !hasWords);
}

void drawReply() {
    char footer[48];
    const int count = static_cast<int>(replyLines.size());
    if (maxReplyScroll()) {
        snprintf(footer, sizeof(footer), "Swipe %d-%d/%d   %s",
                 firstReplyLine + 1, min(count, firstReplyLine + visibleLines), count,
                 replyStreaming ? "Receiving..." : "Hold B for home");
    } else {
        snprintf(footer, sizeof(footer), "%s", replyStreaming ? "Receiving reply..." : "Hold B for home");
    }
    const uint16_t accent = conversation.color565(255, 104, 123);
    if (replyLines.empty()) {
        const String status = "Red is speaking...";
        drawConversation(accent, {status}, 0, footer, uiFont::body(status), true);
        return;
    }
    drawConversation(accent, replyLines, firstReplyLine, footer, replyFont);
}

void normalizeCaption(String& caption) {
    caption.replace("’", "'");
    caption.replace("‘", "'");
    caption.replace("“", "\"");
    caption.replace("”", "\"");
    caption.replace(" ", " ");
    caption.replace("，", ",");
    caption.replace("﹐", ",");
    caption.replace("．", ".");
    caption.replace("。", ".");
    caption.replace("；", ";");
    caption.replace("﹔", ";");
    caption.replace("\u200B", "");
}

void showReply(String message, bool streaming = false, bool continuing = false) {
    wakeDisplay();
    mood = Mood::answer;
    const bool sameAnswer = continuing && screen == Screen::answer;
    const bool follow = sameAnswer && replyStreaming && !scrollTouch && firstReplyLine == maxReplyScroll();
    if (!sameAnswer) {
        firstReplyLine = 0;
        scrollTouch = false;
    }
    replyStreaming = streaming;
    displayedReply = message;
    normalizeCaption(message);
    replyFont = uiFont::body(message);
    replyLines = wrapText(message, replyFont);
    firstReplyLine = follow ? maxReplyScroll() : min(firstReplyLine, maxReplyScroll());
    if (!settingsOpen && !wifiPicker::active()) {
        enterScreen(Screen::answer);
        drawReply();
    }
    if (!streaming) Serial.printf("ORBIT REPLY LINES %u\n", static_cast<unsigned>(replyLines.size()));
}

bool updateTouch(bool enabled) {
    const auto& point = M5.Touch.getDetail();
    const bool pressed = M5.Touch.getCount() && point.isPressed();
    if (!pressed) {
        const bool tapped = touchHeld && !touchConsumed;
        touchHeld = false;
        scrollTouch = false;
        return tapped;
    }
    if (!touchHeld) {
        touchHeld = true;
        touchStartX = point.x;
        touchStartY = dragAnchorY = point.y;
        scrollTouch = enabled && screen == Screen::answer && maxReplyScroll() &&
                      point.y >= conversationTop + textTop && point.y < conversationTop + footerY &&
                      abs(point.x - center) <= conversationWidth / 2;
        touchConsumed = !enabled || screen != Screen::home;
    }
    if (abs(point.x - touchStartX) > 12 || abs(point.y - touchStartY) > 12) touchConsumed = true;
    if (scrollTouch && enabled) {
        const int steps = (dragAnchorY - point.y) / dragLineHeight;
        if (steps) {
            dragAnchorY -= steps * dragLineHeight;
            const int first = constrain(firstReplyLine + steps, 0, maxReplyScroll());
            if (first != firstReplyLine) {
                firstReplyLine = first;
                drawReply();
            }
        }
    }
    return false;
}

void drawHome(const String& prompt);

void show(Mood state, const String& message) {
    if (state == Mood::answer) {
        showReply(message);
        return;
    }
    if (state != Mood::idle) wakeDisplay();
    mood = state;
    if (mood == Mood::thinking) thinkingSince = millis();
    replyLines.clear();
    replyStreaming = false;
    scrollTouch = false;
    if (mood == Mood::idle) {
        screen = Screen::home;
        drawHome(message);
    } else {
        const char* footer = mood == Mood::listening ? (autoRecording ? "Speak naturally" : "Release A when done") :
                             mood == Mood::error ? "Hold A to retry   Hold B for home" : "Hold A to start over";
        drawTranscript(message, footer);
    }
}

void updateInputCaption(bool force = false) {
    if (!directTurn || directFinished || screen == Screen::answer) return;
    const unsigned long now = millis();
    if (!force && static_cast<long>(now - nextCaptionAt) < 0) return;
    nextCaptionAt = now + 200;
    String input = pendingSession || !orbitLive::listening() ? String() : orbitLive::inputCaption();
    normalizeCaption(input);
    input.trim();
    const char* status = "Waiting for Red...";
    if (recording) {
        if (input.isEmpty()) return;
        status = autoRecording ? "Listening. Speak naturally" : "Listening. Release A when done";
    } else if (pendingSession || !orbitLive::listening()) {
        status = sessionRetried ? "Reconnecting..." : "Connecting...";
    } else if (!inputDrained) {
        status = "Sending your question...";
    } else if (orbitLive::waitingForBackend()) {
        status = "Looking it up...";
    }
    if (!force && input == displayedInput && status == displayedInputStatus) return;
    displayedInputStatus = status;
    displayedInput = input;
    if (input.isEmpty()) drawTranscript(status, "Hold A to start over");
    else drawTranscript(input, status, true);
}

void updateReplyCaption() {
    if (!directTurn || directFinished || recording || pendingSession) return;
    const unsigned long now = millis();
    if (static_cast<long>(now - nextReplyCaptionAt) < 0) return;
    nextReplyCaptionAt = now + 125;
    const String reply = orbitLive::caption();
    if (!reply.isEmpty() && reply != displayedReply) {
        if (displayedReply.isEmpty()) {
            Serial.printf("ORBIT TIMING REPLY TEXT %lu ms\n", now - recordingEndedAt);
        }
        showReply(reply, true, true);
    }
}

const char* networkStatus() {
    if (orbitLive::ready()) return "READY";
    if (orbitLive::wifiConnected()) return orbitLive::hasKey() ? "CLOCK" : "NO KEY";
    return orbitLive::configured() ? "WAIT" : "UNSET";
}

const char* homePrompt() {
    if (bridgeReady || orbitLive::ready()) return hotMic && !bridgeReady ? "Speak to Red" : "Hold A to talk";
    if (orbitLive::wifiConnected()) return orbitLive::hasKey() ? "Wi-Fi joined. Syncing clock" : "Add OpenAI key over USB";
    return orbitLive::configured() ? "Joining Wi-Fi. B: Scan" : "B: Find Wi-Fi";
}

void drawTimer(redTimer::Snapshot timer) {
    displayedTimer = timer;
    const unsigned seconds = timer.seconds;
    char text[9];
    snprintf(text, sizeof(text), "%02u:%02u:%02u", seconds / 3600, seconds / 60 % 60, seconds % 60);
    const bool finished = timer.state == redTimer::State::finished;
    auto& panel = conversation;
    panel.fillScreen(TFT_BLACK);
    panel.setFont(uiFont::timer());
    panel.setTextSize(1);
    panel.setTextDatum(middle_center);
    panel.setTextColor(finished ? TFT_ORANGE : TFT_WHITE, TFT_BLACK);
    panel.drawString(text, conversationWidth / 2, conversationHeight / 2 + 10);

    panel.setFont(uiFont::label());
    panel.setTextSize(1);
    panel.setTextDatum(top_center);
    panel.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    panel.drawString(finished ? "Timer done   /   Hold B for home" : "Hold B for home",
                     conversationWidth / 2, footerY);
    panel.pushSprite(center - conversationWidth / 2, conversationTop);
}

void showTimer() {
    mood = Mood::idle;
    replyLines.clear();
    replyStreaming = false;
    scrollTouch = false;
    enterScreen(Screen::timer);
    drawTimer(redTimer::snapshot());
}

void drawStatus() {
    if (displaySleeping) return;
    auto& display = M5.Display;
    const int x = center - 61;
    constexpr int y = 52;
    display.startWrite();
    display.fillRect(center - 78, y - 6, 156, 27, TFT_BLACK);
    display.drawRoundRect(x, y + 2, 23, 12, 2, TFT_WHITE);
    display.fillRect(x + 23, y + 5, 2, 6, TFT_WHITE);
    if (batteryCharging) {
        display.fillTriangle(x + 12, y + 3, x + 7, y + 9, x + 13, y + 9, TFT_WHITE);
        display.fillTriangle(x + 11, y + 13, x + 17, y + 7, x + 11, y + 7, TFT_WHITE);
    } else if (batteryPercent > 0) {
        display.fillRect(x + 3, y + 5, max(1, 17 * batteryPercent / 100), 6, TFT_WHITE);
    }
    display.setFont(uiFont::label());
    display.setTextDatum(top_left);
    display.setTextSize(1);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.drawString(batteryPercent < 0 ? String("--%") : String(batteryPercent) + "%", x + 30, y);
    const int wifiX = center + 47;
    const int wifiY = y + 13;
    const uint16_t dim = display.color565(56, 56, 56);
    display.fillCircle(wifiX, wifiY, 2, wifiBars ? TFT_WHITE : dim);
    for (int band = 1; band <= 3; ++band) {
        const int radius = 4 * band + 3;
        display.fillArc(wifiX, wifiY, radius, radius - 2, 224, 316,
                        wifiBars >= band ? TFT_WHITE : dim);
    }
    if (!wifiBars) display.drawLine(wifiX - 10, y - 1, wifiX + 9, y + 15, TFT_WHITE);
    display.endWrite();
}

void pollStatus() {
    const unsigned long now = millis();
    if (static_cast<long>(now - nextStatusPoll) < 0) return;
    nextStatusPoll = now + 2000;
    const int percent = M5.Power.getBatteryLevel();
    const bool charging = M5.Power.isCharging();
    const int rssi = orbitLive::wifiSignal();
    const int bars = rssi >= 0 ? 0 : rssi >= -60 ? 3 : rssi >= -72 ? 2 : 1;
    if (percent == batteryPercent && charging == batteryCharging && bars == wifiBars) return;
    if (percent != batteryPercent || charging != batteryCharging) {
        Serial.printf("ORBIT BATTERY %d %s\n", percent, charging ? "CHARGING" : "IDLE");
    }
    batteryPercent = percent;
    batteryCharging = charging;
    wifiBars = bars;
    if (!wifiPicker::active() && !bluetoothSetup::active()) drawStatus();
}

void drawAvatar(int lift) {
    auto& display = M5.Display;
    const int left = center - avatarSize / 2;
    const int top = avatarTop - lift;
    const int oldTop = avatarTop - avatarLift;
    display.startWrite();
    if (flowerReady) {
        if (top != oldTop) {
            const int exposedY = top < oldTop ? top + avatarSize : oldTop;
            display.fillRect(left, exposedY, avatarSize, abs(top - oldTop), TFT_BLACK);
        }
        flower.pushSprite(left, top);
    } else {
        display.fillRect(left, min(top, oldTop), avatarSize, avatarSize + abs(top - oldTop), TFT_BLACK);
        const int y = top + avatarSize / 2;
        const uint16_t red = display.color565(251, 82, 101);
        for (int petal = 0; petal < 8; ++petal) {
            const float angle = petal * PI / 4;
            display.fillCircle(center + 70 * cosf(angle), y + 70 * sinf(angle), 46, red);
        }
        display.fillCircle(center, y, 67, red);
        display.fillCircle(center - 23, y - 4, 6, TFT_BLACK);
        display.fillCircle(center + 23, y - 4, 6, TFT_BLACK);
        display.fillArc(center, y + 9, 18, 15, 15, 165, TFT_BLACK);
    }
    display.endWrite();
    avatarLift = lift;
}

void updateAvatarBounce(unsigned long now) {
    if (!avatarBounceAt || screen != Screen::home) return;
    const unsigned long elapsed = min(now - avatarBounceAt, avatarBounceMs);
    const int lift = 4 * avatarBounceHeight * elapsed * (avatarBounceMs - elapsed) /
                     (avatarBounceMs * avatarBounceMs);
    if (lift != avatarLift) drawAvatar(lift);
    if (elapsed == avatarBounceMs) avatarBounceAt = 0;
}

void drawHome(const String& prompt) {
    if (displaySleeping) return;
    auto& display = M5.Display;
    display.fillScreen(TFT_BLACK);
    drawStatus();
    avatarBounceAt = 0;
    avatarLift = 0;
    drawAvatar(0);
    display.setTextDatum(top_center);
    display.setFont(uiFont::title());
    display.setTextSize(1);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.drawString("RED", center, 342);
    display.setFont(uiFont::label());
    display.setTextColor(hotMic && !bridgeReady ? TFT_GREEN : TFT_LIGHTGREY, TFT_BLACK);
    display.drawString(prompt, center, 382);
}

void showHome() {
    show(Mood::idle, homePrompt());
}

void returnToReplyOrHome() {
    if (screen == Screen::timer) {
        M5.Display.fillScreen(TFT_BLACK);
        drawStatus();
        drawTimer(redTimer::snapshot());
    } else if (mood == Mood::answer && !replyLines.empty()) {
        screen = Screen::answer;
        M5.Display.fillScreen(TFT_BLACK);
        drawStatus();
        drawReply();
    } else {
        showHome();
    }
}

void releaseMicBuffer(int16_t* samples, size_t count) {
    const auto address = reinterpret_cast<uintptr_t>(samples);
    const auto standbyAddress = reinterpret_cast<uintptr_t>(standby);
    if (address >= standbyAddress && address < standbyAddress + sizeof(standby)) {
        ++standbyCompleted;
    } else {
        micCompleted = samples - capture + count;
    }
}

bool micRunning() {
    return duplexAudio::active() || M5.Mic.isRunning();
}

bool queueMic(int16_t* samples, size_t count) {
    if (duplexAudio::active()) return duplexAudio::record(samples, count);
    return M5.Mic.record(samples, count, sampleRate);
}

void stopMic() {
    if (duplexAudio::active()) duplexAudio::clearInput();
    if (M5.Mic.isRunning()) M5.Mic.end();
    micReadyAt = 0;
    standbyCompleted = 0;
    standbyQueued = 0;
    standbyRead = 0;
    voiceRun = 0;
}

bool startMic() {
    if (duplexAudio::active()) return true;
    if (M5.Mic.isRunning()) return true;
    if (!M5.Mic.begin()) return false;
    micReadyAt = millis() + (micInitialized ? micWarmupMs : firstMicWarmupMs);
    micInitialized = true;
    return true;
}

void openPicker() {
    autoScan = false;
    stopMic();
    duplexAudio::end();
    wifiPicker::open(orbitLive::configured());
}

void checkCloud() {
    if (!checkingCloud) return;
    const unsigned long now = millis();
    const bool accepted = orbitLive::listening();
    bool passed = false;
    if (!orbitLive::failed() && now - cloudCheckStarted < 14000) {
        if (!accepted) return;
        if (cloudFramesSent == 50) {
            if (static_cast<long>(now - cloudNextFrame) < 300) return;
            passed = true;
        } else {
            if (static_cast<long>(now - cloudNextFrame) < 0 || !orbitLive::canAppend()) return;
            static const int16_t silence[micChunk]{};
            if (orbitLive::append(silence, micChunk)) {
                ++cloudFramesSent;
                cloudNextFrame = millis() + micChunkMs;
                return;
            }
        }
    }
    orbitLive::close();
    checkingCloud = false;
    Serial.println(passed ? "ORBIT CLOUD READY" : "ORBIT CLOUD FAILED");
}

bool prepareSpeaker() {
    if (duplexAudio::active()) return true;
    if (M5.Speaker.isRunning()) return true;
    stopMic();
    M5.Speaker.setVolume(speakerVolume * 255 / 100);
    speakerDrainSince = 0;
    // The mic and speaker share the codec's clocks. Give the DAC a quiet
    // lead-in and re-arm its analog stage after I2S is running.
    static const int16_t leadIn[sampleRate / 10]{};
    if (!M5.Speaker.playRaw(leadIn, sampleRate / 10, sampleRate, false, 1, 0)) {
        M5.Speaker.end();
        return false;
    }
    delay(35);
    if (!M5.In_I2C.writeRegister8(0x18, 0x0D, 0x01, 100000)) {
        M5.Speaker.end();
        return false;
    }
    return true;
}

bool speakerDrained() {
    if (duplexAudio::active()) return duplexAudio::drained();
    if (M5.Speaker.isPlaying()) {
        speakerDrainSince = 0;
        return false;
    }
    if (!speakerDrainSince) speakerDrainSince = millis();
    // isPlaying clears when the last samples enter DMA. Keep the amplifier
    // on until those samples have actually had time to reach the speaker.
    return millis() - speakerDrainSince >= 100;
}

void stopSpeaker() {
    if (duplexAudio::active()) duplexAudio::stopOutput();
    else if (M5.Speaker.isRunning()) M5.Speaker.end();
}

void serviceTimer() {
    const auto timer = redTimer::snapshot();
    if (!displaySleeping && screen == Screen::timer && !settingsOpen && !wifiPicker::active() &&
        (timer.state != displayedTimer.state || timer.seconds != displayedTimer.seconds)) {
        drawTimer(timer);
    }
    if (timer.state != redTimer::State::finished) timerChimePending = false;
    if (redTimer::takeAlarm()) {
        wakeDisplay();
        timerChimePending = true;
        // Stop the motor on its own task so a Wi-Fi scan cannot extend the pulse.
        if (xTaskCreate([](void*) {
                if (M5.Power.setVibration(200)) {
                    vTaskDelay(pdMS_TO_TICKS(300));
                    bool stopped = M5.Power.setVibration(0);
                    if (!stopped) {
                        vTaskDelay(pdMS_TO_TICKS(20));
                        stopped = M5.Power.setVibration(0);
                    }
                    Serial.println(stopped ? "ORBIT TIMER VIBRATED" : "ORBIT TIMER VIBRATION STOP ERROR");
                } else {
                    Serial.println("ORBIT TIMER VIBRATION UNAVAILABLE");
                }
                vTaskDelete(nullptr);
            }, "red-timer-buzz", 3072, nullptr, 1, nullptr) != pdPASS) {
            Serial.println("ORBIT TIMER VIBRATION UNAVAILABLE");
        }
    }
    if (!timerChimePending || recording || directTurn || playing || checkingCloud) return;
    timerChimePending = false;
    if (duplexAudio::active()) {
        stopMic();
        duplexAudio::end();
    }
    playing = prepareSpeaker() && M5.Speaker.tone(880, 650, 0, false);
    Serial.println(playing ? "ORBIT TIMER ALARM" : "ORBIT TIMER ALARM AUDIO ERROR");
    if (!playing) stopSpeaker();
}

bool queueSpeaker(const int16_t* samples, size_t count) {
    if (duplexAudio::active()) return duplexAudio::play(samples, count, speakerVolume);
    return M5.Speaker.playRaw(samples, count, sampleRate, false, 1, 0);
}

bool playReply() {
    if (!playable || !prepareSpeaker()) return false;
    playing = queueSpeaker(playback, playable);
    if (!playing) stopSpeaker();
    return playing;
}

void receiveReply(const String& sizeText) {
    char* end = nullptr;
    const unsigned long size = strtoul(sizeText.c_str(), &end, 10);
    if (!playback || recording || directTurn || playing || !size || size % 2 ||
        size > playbackLimit * sizeof(int16_t) || *end) {
        Serial.println("ORBIT AUDIO ERROR");
        return;
    }
    playable = 0;
    auto* bytes = reinterpret_cast<uint8_t*>(playback);
    for (size_t offset = 0; offset < size;) {
        const size_t count = min(usbChunk, static_cast<size_t>(size - offset));
        Serial.println("ORBIT RX");
        if (Serial.readBytes(bytes + offset, count) != count) {
            Serial.println("ORBIT AUDIO ERROR");
            show(Mood::error, "Audio transfer failed");
            return;
        }
        offset += count;
    }
    playable = size / sizeof(int16_t);
    Serial.println(playReply() ? "ORBIT PLAYING" : "ORBIT AUDIO ERROR");
}

void stopRecording() {
    if (!recording) return;
    stopMic();
    recording = false;
}

void cancelTurn() {
    stopRecording();
    if (directTurn && !directFinished && !pendingSession) orbitLive::closeAsync();
    pendingSession = false;
    directTurn = false;
    directFinished = false;
    stopSpeaker();
    playing = false;
    autoRearmAt = millis() + 300;
}

void finishDirect(const char* error = nullptr, bool remember = true) {
    stopRecording();
    const bool hasSession = !pendingSession && !directFinished;
    pendingSession = false;
    directFinished = true;
    if (error) {
        if (hasSession) orbitLive::closeAsync();
        stopSpeaker();
        playing = false;
        directTurn = false;
        playable = 0;
        if (hasSession && orbitLive::timerViewRequested()) showTimer();
        else show(Mood::error, error);
        return;
    }
    playable = orbitLive::available();
    String caption = orbitLive::caption();
    if (hasSession && remember && playable && !orbitLive::waitingForBackend()) {
        orbitLive::rememberTurn(caption);
    }
    if (hasSession) orbitLive::closeAsync();
    showReply(caption.isEmpty() ? "Red has an answer" : caption, false, true);
}

bool retryDirect() {
    if (sessionRetried || queuedSamples || !orbitLive::canRetryTurn()) return false;
    orbitLive::closeAsync();
    sessionRetried = true;
    pendingSession = true;
    inputDrained = false;
    sent = discardSamples;
    directStartedAt = nextInputAt = millis();
    replyReminderSent = false;
    displayedInput = "";
    displayedInputStatus = nullptr;
    displayedReply = "";
    nextReplyCaptionAt = millis();
    Serial.println("ORBIT LIVE RETRY BUFFERED INPUT");
    return true;
}

void serviceDirect() {
    if (!directTurn) return;
    const unsigned long now = millis();
    if (pendingSession) {
        if (orbitLive::closing() || !orbitLive::ready()) {
            if (now - directStartedAt > 10000) finishDirect("Could not reconnect");
            return;
        }
        pendingSession = false;
        if (!orbitLive::start(playback, playbackLimit)) {
            finishDirect("Could not open GPT-Live");
            return;
        }
    }
    const size_t received = directFinished ? playable : orbitLive::available();
    if (!directFinished && orbitLive::failed()) {
        if (retryDirect()) return;
        if (!recording && received) finishDirect(nullptr, false);
        else finishDirect("Connection lost. Try again.");
        return;
    }
    if (!directFinished && !orbitLive::listening()) {
        if (now - directStartedAt > 10000 && !retryDirect()) finishDirect("GPT-Live did not connect");
        return;
    }
    if (!recording && !directFinished && !inputDrained &&
        now - recordingEndedAt > (sessionRetried ? 18000 : 10000)) {
        if (!retryDirect()) finishDirect("Could not send the question.");
        return;
    }

    if (!directFinished && sent < captured && orbitLive::canAppend() &&
        static_cast<long>(now - nextInputAt) >= 0) {
        const bool catchingUp = captured - sent > 2 * micChunk;
        const size_t count = catchingUp ? 2 * micChunk : micChunk;
        if (!orbitLive::append(capture + sent, count)) {
            if (!orbitLive::failed() || !retryDirect()) finishDirect("Could not send audio");
            return;
        }
        sent += count;
        nextInputAt = millis() + micChunkMs;
    }
    if (recording || sent < captured) return;

    if (!directFinished) {
        if (!inputDrained) {
            if (!orbitLive::canAppend()) return;
            inputDrained = true;
            releasedAt = nextSilenceAt = now;
            Serial.printf("ORBIT TIMING UPLOAD %lu ms\n", now - recordingEndedAt);
        }
        const bool waitingForLookup = orbitLive::waitingForBackend();
        const unsigned long sinceRelease = now - releasedAt;
        if (!received && sinceRelease > (waitingForLookup ? lookupTimeoutMs : displayedInput.isEmpty() ? 10000 : 16000)) {
            String heard = orbitLive::inputCaption();
            heard.trim();
            finishDirect(heard.isEmpty() ? "No words transcribed. Try again." : "Heard you. No spoken reply.");
            return;
        }
        // The network callback can update the reply timestamp after this
        // loop reads millis(). Signed comparison avoids wrapping that gap.
        const bool replyQuiet = !waitingForLookup &&
                                static_cast<long>(now - orbitLive::lastReplyActivity()) > static_cast<long>(replyQuietMs);
        if (received && (replyQuiet || orbitLive::outputFull() || sinceRelease > replyTimeoutMs)) {
            finishDirect();
        } else if (orbitLive::canAppend() && static_cast<long>(now - nextSilenceAt) >= 0) {
            static const int16_t silence[micChunk]{};
            if (!orbitLive::append(silence, micChunk)) {
                finishDirect("Could not send audio");
                return;
            }
            nextSilenceAt += micChunkMs;
            const unsigned long afterSend = millis();
            if (static_cast<long>(afterSend - nextSilenceAt) >= 0) {
                nextSilenceAt = afterSend + micChunkMs;
            }
        }
        if (!autoRecording && orbitLive::canAppend() && !received && !waitingForLookup && !replyReminderSent &&
            sinceRelease >= 6000 && !displayedInput.isEmpty()) {
            replyReminderSent = true;
            Serial.println(orbitLive::requestReply() ? "ORBIT LIVE REPLY REMINDER" :
                                                      "ORBIT LIVE REPLY REMINDER FAILED");
        }
    }
    const size_t threshold = queuedSamples ? micChunk : speakerPreroll;
    if (received > queuedSamples &&
        (directFinished || received - queuedSamples >= threshold) &&
        (duplexAudio::active() ? duplexAudio::queuedOutput() : M5.Speaker.isPlaying(0)) < 2) {
        const size_t count = min(speakerChunk, received - queuedSamples);
        if (!prepareSpeaker() || !queueSpeaker(playback + queuedSamples, count)) {
            finishDirect("Speaker unavailable");
            return;
        }
        if (!queuedSamples) Serial.printf("ORBIT TIMING REPLY %lu ms\n", millis() - recordingEndedAt);
        if (!queuedSamples && !directFinished && screen != Screen::answer) showReply("", true);
        queuedSamples += count;
        playing = true;
        speakerDrainSince = 0;
    }
    const bool drained = playing && speakerDrained();
    if (directFinished && queuedSamples == playable && drained) {
        stopSpeaker();
        playing = false;
        directTurn = false;
        if (orbitLive::timerViewRequested()) showTimer();
    }
}

int frameLevel(const int16_t* samples) {
    int sum = 0;
    for (size_t i = 0; i < micChunk; ++i) sum += samples[i];
    const int mean = sum / static_cast<int>(micChunk);
    int deviation = 0;
    for (size_t i = 0; i < micChunk; ++i) deviation += abs(samples[i] - mean);
    return deviation / static_cast<int>(micChunk);
}

bool primeHandsFree() {
    if (standbyCompleted.load() < standbyQueued) return false;
    const size_t first = standbyQueued > standbyFrames ? standbyQueued - standbyFrames : 0;
    for (size_t frame = first; frame < standbyQueued; ++frame) {
        memcpy(capture + captured, standby[frame % standbyFrames], sizeof(standby[0]));
        captured += micChunk;
    }
    micCompleted = micQueued = captured;
    autoPrimed = true;
    return true;
}

void updateMic() {
    if (!recording) return;
    if (autoRecording && !autoPrimed && !primeHandsFree()) return;
    // GPT-Live decides when to answer from the continuous input stream.
    // Wait for its speech and a quiet mic before handing the codec to the speaker.
    const bool waitingForSpeech = pendingSession || quietFrames < speakerHandoffQuietMs / micChunkMs ||
                                  !orbitLive::hasSpokenOutput();
    const bool handsFreeListening = autoRecording && waitingForSpeech &&
                                    quietFrames < silenceFallbackMs / micChunkMs &&
                                    millis() - buttonPressedAt < 12000;
    const bool held = M5.BtnA.isPressed() || handsFreeListening;
    if (!held && !releaseSeenAt) releaseSeenAt = millis();
    const bool ready = static_cast<long>(millis() - micReadyAt) >= 0;
    const bool duplex = duplexAudio::active();
    // Keep a second request in flight while the main loop sends or renders
    // the previous buffer. A drained capture queue drops audio between frames.
    if (held && !releaseSeenAt && ready) {
        while (micQueued < captureLimit && (duplex || micQueued - micCompleted.load() < 2 * legacyMicBlock)) {
            const size_t count = duplex ? captureLimit - micQueued : min(legacyMicBlock, captureLimit - micQueued);
            const bool queued = duplex ? duplexAudio::record(capture + micQueued, count, micChunk) :
                                         queueMic(capture + micQueued, count);
            if (!queued) {
                if (directTurn) finishDirect("Mic unavailable");
                else {
                    stopRecording();
                    show(Mood::error, "Mic unavailable");
                    Serial.println("ORBIT MIC SHORT");
                }
                return;
            }
            micQueued += count;
        }
    }
    while (captured < micCompleted.load()) {
        if (autoRecording) {
            if (frameLevel(capture + captured) >= max(120, noiseLevel * 2)) {
                quietFrames = 0;
                ++voicedFrames;
            } else {
                ++quietFrames;
            }
        }
        if (captured >= discardSamples) {
            for (size_t i = 0; i < micChunk; ++i) {
                micPeak = max(micPeak, abs(static_cast<int>(capture[captured + i])));
            }
        }
        captured += micChunk;
        if (held && captured == discardSamples + micChunk) {
            if (directTurn) Serial.printf("ORBIT TIMING LISTEN %lu ms\n", millis() - buttonPressedAt);
            show(Mood::listening, "Listening...");
        }
    }
    if (releaseSeenAt || captured == captureLimit) {
        if (captured < micQueued && millis() - releaseSeenAt < (duplex ? 20 : 90)) return;
        recordingEndedAt = millis();
        stopRecording();
        const size_t minSamples = sampleRate * (directTurn ? 3 : 2) / 10;
        if (captured < discardSamples + minSamples || (autoRecording && voicedFrames < 6)) {
            cancelTurn();
            if (autoRecording) showHome();
            else show(Mood::idle, "Hold A through the question");
            Serial.println("ORBIT MIC SHORT");
            return;
        }
        if (directTurn) {
            mood = Mood::thinking;
            thinkingSince = millis();
            Serial.printf("ORBIT LIVE INPUT %u samples peak %d\n",
                          static_cast<unsigned>(captured - discardSamples), micPeak);
            updateInputCaption(true);
            return;
        }
        show(Mood::thinking, "Waiting for the USB bridge...");
        const size_t size = captured * sizeof(int16_t);
        Serial.printf("ORBIT MIC %u\n", static_cast<unsigned int>(size));
        Serial.write(reinterpret_cast<const uint8_t*>(capture), size);
    }
}

void startRecording(bool handsFree = false) {
    wakeDisplay();
    if (!capture || !playback) {
        show(Mood::error, "Audio memory unavailable");
        return;
    }
    if (!bridgeReady && !orbitLive::ready()) {
        show(Mood::idle, homePrompt());
        return;
    }
    if (directTurn || playing) {
        if (handsFree) Serial.println("ORBIT HOT MIC INTERRUPT");
        // The next press can arrive after the speaker has finished but
        // before the network's end-of-reply grace period has elapsed.
        if (directTurn && !directFinished && playing && queuedSamples == orbitLive::available() &&
            !orbitLive::waitingForBackend() && speakerDrained()) {
            orbitLive::rememberTurn(orbitLive::caption());
        }
        cancelTurn();
    }
    const bool alreadyWarm = micRunning();
    autoRecording = handsFree;
    show(Mood::listening, handsFree ? "Listening..." : "Starting mic...");
    if (!startMic()) {
        show(Mood::error, "Mic unavailable");
        return;
    }
    if (!bridgeReady) {
        directTurn = true;
        directFinished = false;
        pendingSession = true;
        queuedSamples = 0;
        playable = 0;
        directStartedAt = millis();
    }
    captured = 0;
    sessionRetried = false;
    inputDrained = false;
    autoPrimed = false;
    quietFrames = 0;
    voicedFrames = handsFree ? voiceRun : 0;
    // Skip any samples left in the DMA queue from before the button hold.
    discardSamples = alreadyWarm && !handsFree ? 3 * micChunk : 0;
    sent = discardSamples;
    nextInputAt = millis();
    micQueued = 0;
    micCompleted = 0;
    micPeak = 0;
    releaseSeenAt = 0;
    nextCaptionAt = 0;
    nextReplyCaptionAt = millis();
    displayedInput = "";
    displayedInputStatus = nullptr;
    displayedReply = "";
    replyReminderSent = false;
    recording = true;
    if (bridgeReady) Serial.println("ORBIT RECORDING");
}

void updateStandby(unsigned long now) {
    if (!hotMic || bridgeReady || !orbitLive::ready() || checkingCloud || recording ||
        chordHeld || M5.BtnA.isPressed() || M5.BtnB.isPressed() ||
        static_cast<long>(now - autoRearmAt) < 0) return;
    if (!micRunning()) {
        if (directTurn || playing || static_cast<long>(now - nextMicWarmupAt) < 0) return;
        if (!voiceInterruptBeta) {
            if (!startMic()) nextMicWarmupAt = now + 2000;
        } else if (!duplexAudio::begin(releaseMicBuffer)) {
            nextMicWarmupAt = now + 2000;
            voiceInterruptBeta = false;
            Serial.println("ORBIT DUPLEX ERROR");
        } else {
            micReadyAt = millis() + 800;
            Serial.println("ORBIT DUPLEX READY");
        }
        return;
    }
    if ((directTurn || playing) && !duplexAudio::active()) return;
    if (static_cast<long>(now - micReadyAt) < 0) return;
    while (standbyRead < standbyCompleted.load()) {
        const int level = frameLevel(standby[standbyRead++ % standbyFrames]);
        const bool canInterrupt = !playing || duplexAudio::interruptible();
        const int threshold = playing ? max(1200, noiseLevel * 5) : max(220, noiseLevel * 2 + 100);
        if (canInterrupt && level >= threshold) {
            if (++voiceRun >= (playing ? 6 : 4)) {
                Serial.printf("ORBIT HOT MIC SPEECH %d noise %d\n", level, noiseLevel);
                buttonPressedAt = millis();
                startRecording(true);
                return;
            }
        } else {
            voiceRun = 0;
            if (!playing) noiseLevel = (noiseLevel * 15 + level + 8) / 16;
        }
    }
    while (standbyQueued - standbyCompleted.load() < 2) {
        if (!queueMic(standby[standbyQueued % standbyFrames], micChunk)) {
            stopMic();
            return;
        }
        ++standbyQueued;
    }
}

void toggleHotMic() {
    hotMic = !hotMic;
    Serial.println(hotMic ? "ORBIT HOT MIC ON" : "ORBIT HOT MIC OFF");
    if (!hotMic) {
        if (duplexAudio::active()) {
            cancelTurn();
            duplexAudio::end();
        }
        if (recording && autoRecording) cancelTurn();
        if (!recording) stopMic();
    } else {
        noiseLevel = 80;
        autoRearmAt = millis() + 200;
    }
    if (!settingsOpen) {
        if (!directTurn && !playing) showHome();
    }
}

void drawSettings() {
    auto& display = M5.Display;
    display.fillScreen(TFT_BLACK);
    display.setTextDatum(top_center);
    display.setTextSize(1);
    display.setFont(uiFont::title());
    const String items[] = {"Wi-Fi networks", "Bluetooth setup", "Volume " + String(speakerVolume) + "%",
                            String("Display: ") + displayTimeouts[displayTimeoutIndex].label,
                            hotMic ? "Hands-free on" : "Hands-free off",
                            voiceInterruptBeta ? "Voice interrupt beta on" : "Voice interrupt beta off", "Back"};
    for (int item = 0; item < settingsCount; ++item) {
        const int y = 85 + item * 33;
        if (item == settingsItem) display.fillRoundRect(center - 150, y - 4, 300, 32, 8, TFT_DARKCYAN);
        display.setTextColor(TFT_WHITE, item == settingsItem ? TFT_DARKCYAN : TFT_BLACK);
        display.drawString(items[item], center, y);
    }
    display.setFont(uiFont::label());
    display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    display.drawString(firmwareVersion, center, 320);
    display.setTextColor(TFT_CYAN, TFT_BLACK);
    display.drawString("A  Next     B  Select     Red  Back", center, 354);
    drawStatus();
}

void wakeDisplay() {
    displayActivityAt = millis();
    if (!displaySleeping) return;
    M5.Display.wakeup();
    displaySleeping = false;
    if (bluetoothSetup::active()) {
        bluetoothSetup::redraw();
    } else if (wifiPicker::active()) {
        wifiPicker::redraw();
    } else if (settingsOpen) {
        drawSettings();
    } else if (screen == Screen::home) {
        drawHome(homePrompt());
    } else {
        M5.Display.fillScreen(TFT_BLACK);
        drawStatus();
        if (screen == Screen::timer) drawTimer(redTimer::snapshot());
        else conversation.pushSprite(center - conversationWidth / 2, conversationTop);
    }
    Serial.println("ORBIT DISPLAY WAKE");
}

bool pickedUp(unsigned long now) {
    if (!M5.Imu.isEnabled() || now - lastMotionSample < motionSampleMs) return false;
    lastMotionSample = now;
    const auto sensors = M5.Imu.update();
    if (!(sensors & m5::IMU_Class::sensor_mask_accel)) return false;
    const auto& data = M5.Imu.getImuData();
    if (!restingAccelReady) {
        restingAccel = data.accel;
        restingAccelReady = true;
        return false;
    }
    const float dx = data.accel.x - restingAccel.x;
    const float dy = data.accel.y - restingAccel.y;
    const float dz = data.accel.z - restingAccel.z;
    const auto& gyro = data.gyro;
    const bool turning = (sensors & m5::IMU_Class::sensor_mask_gyro) &&
                         gyro.x * gyro.x + gyro.y * gyro.y + gyro.z * gyro.z >= pickupGyroDps * pickupGyroDps;
    const bool moving = dx * dx + dy * dy + dz * dz >= pickupAccelG * pickupAccelG;
    // A single spike can come from a bump to the table.
    pickupSamples = moving || turning ? pickupSamples + 1 : 0;
    return pickupSamples >= 2;
}

void updateDisplayTimeout(bool menuClicked) {
    const bool touching = M5.Touch.getCount() && M5.Touch.getDetail().isPressed();
    if (touching || menuClicked || M5.BtnA.isPressed() || M5.BtnB.isPressed() ||
        recording || directTurn || playing || (bridgeReady && mood == Mood::thinking)) {
        wakeDisplay();
        return;
    }
    const unsigned long now = millis();
    if (displaySleeping) {
        if (pickedUp(now)) {
            Serial.println("ORBIT DISPLAY MOTION");
            wakeDisplay();
        }
        return;
    }
    const unsigned long timeout = displayTimeouts[displayTimeoutIndex].durationMs;
    if (timeout && now - displayActivityAt >= timeout) {
        restingAccelReady = false;
        pickupSamples = 0;
        lastMotionSample = now - motionSampleMs;
        M5.Display.sleep();
        displaySleeping = true;
        Serial.println("ORBIT DISPLAY SLEEP");
    }
}

void openSettings() {
    cancelTurn();
    stopMic();
    duplexAudio::end();
    settingsOpen = true;
    replyStreaming = false;
    settingsItem = 0;
    scrollTouch = false;
    if (touchHeld) touchConsumed = true;
    drawSettings();
    Serial.println("ORBIT MENU OPEN");
}

void closeSettings() {
    settingsOpen = false;
    chordHeld = M5.BtnA.isPressed() || M5.BtnB.isPressed();
    returnToReplyOrHome();
    Serial.println("ORBIT MENU CLOSED");
}

bool configureMenuButton() {
    // The PM1 normally resets the ESP32 on a single click. Change only
    // that action, keeping the double-click power-off and USB download.
    constexpr uint8_t disableSingleReset = 0x01;
    auto& pmic = M5.Power.M5pm1;
    uint8_t config = 0;
    if (!pmic.readRegister(M5PM1_REG_BTN_CFG_1, &config, 1)) return false;
    if (!(config & disableSingleReset) &&
        !pmic.writeRegister8(M5PM1_REG_BTN_CFG_1, config | disableSingleReset)) return false;
    return pmic.readRegister(M5PM1_REG_BTN_CFG_1, &config, 1) && (config & disableSingleReset);
}

bool pollMenuButton() {
    const unsigned long now = millis();
    if (!menuButtonReady || now - M5.BtnPWR.getUpdateMsec() < 20) return false;
    // Read the live button state. Some PM1 firmware does not report the
    // single-click interrupt after its reset action has been disabled.
    uint8_t status = 0;
    if (!M5.Power.M5pm1.readRegister(M5PM1_REG_BTN_STATUS, &status, 1)) return false;
    M5.BtnPWR.setRawState(now, status & 0x01);
    return M5.BtnPWR.wasClicked();
}

void updateButtons() {
    auto& a = M5.BtnA;
    auto& b = M5.BtnB;
    if (a.isPressed() && b.isPressed()) {
        if (!chordHeld) {
            chordHeld = true;
            aHandled = true;
            lastClickAt = 0;
            if (recording) {
                cancelTurn();
                showHome();
            }
        }
        if (!settingsOpen && a.pressedFor(settingsHoldMs) && b.pressedFor(settingsHoldMs)) {
            openSettings();
        }
        return;
    }
    if (chordHeld) {
        if (!a.isPressed() && !b.isPressed()) chordHeld = false;
        return;
    }
    if (settingsOpen) {
        if (b.wasPressed() && settingsItem == settingsBack) {
            closeSettings();
        } else if (a.wasPressed()) {
            settingsItem = (settingsItem + 1) % settingsCount;
            drawSettings();
        } else if (b.wasPressed()) {
            if (settingsItem == settingsWifi) {
                openPicker();
            } else if (settingsItem == settingsBluetooth) {
                if (checkingCloud) {
                    checkingCloud = false;
                    orbitLive::closeAsync();
                }
                autoScan = false;
                bluetoothSetup::open();
            } else if (settingsItem == settingsVolume) {
                speakerVolume = speakerVolume == 100 ? 25 : speakerVolume + 25;
                Preferences preferences;
                if (preferences.begin("orbit", false)) {
                    preferences.putUChar("volume", speakerVolume);
                    preferences.end();
                }
                drawSettings();
            } else if (settingsItem == settingsDisplay) {
                displayTimeoutIndex = (displayTimeoutIndex + 1) % displayTimeoutCount;
                Preferences preferences;
                if (preferences.begin("orbit", false)) {
                    preferences.putUChar("displayTimeout", displayTimeoutIndex);
                    preferences.end();
                }
                drawSettings();
            } else if (!bridgeReady && settingsItem == settingsHandsFree) {
                toggleHotMic();
                drawSettings();
            } else if (!bridgeReady && settingsItem == settingsInterrupt) {
                voiceInterruptBeta = !voiceInterruptBeta;
                drawSettings();
            }
        }
        return;
    }
    if (a.wasPressed()) {
        aHandled = false;
        buttonPressedAt = millis();
    }
    if (!aHandled && a.pressedFor(holdToTalkMs)) {
        aHandled = true;
        lastClickAt = 0;
        if (!checkingCloud && !recording && (directTurn || playing || mood != Mood::thinking)) startRecording();
    }
    if (a.wasReleased() && !aHandled) {
        const unsigned long now = millis();
        if (lastClickAt && now - lastClickAt <= doubleClickMs) {
            lastClickAt = 0;
            if (!bridgeReady) toggleHotMic();
        } else {
            lastClickAt = now;
        }
    }
    if (b.wasReleased() && !a.isPressed()) {
        const bool goingHome = screen != Screen::home && !recording && mood != Mood::thinking &&
                               b.wasReleaseFor(holdToHomeMs);
        if (directTurn || playing || goingHome) {
            cancelTurn();
            showHome();
        } else if (!recording && mood != Mood::thinking) {
            if (!bridgeReady && !orbitLive::ready()) openPicker();
            else if (!playReply()) show(Mood::idle, homePrompt());
        }
    }
}

void handleCommand(const String& command) {
    if (bluetoothSetup::active() && command != "WIFI STATUS") return;
    if (command == "WIFI STATUS") {
        Serial.print("ORBIT WIFI ");
        Serial.println(networkStatus());
    } else if (command == "WIFI CHECK") {
        if (checkingCloud || directTurn || recording || playing || !orbitLive::ready() ||
            !orbitLive::start(nullptr, 0)) {
            Serial.println("ORBIT CLOUD UNAVAILABLE");
        } else {
            checkingCloud = true;
            cloudCheckStarted = millis();
            cloudNextFrame = cloudCheckStarted;
            cloudFramesSent = 0;
        }
    } else if (command == "WIFI PICK") {
        StaticJsonDocument<96> result;
        result["ssid"] = wifiPicker::selected();
        Serial.print("ORBIT WIFI PICK ");
        serializeJson(result, Serial);
        Serial.println();
    } else if (command == "WIFI SCAN" && !bridgeReady && !directTurn && !playing && !recording) {
        openPicker();
    } else if (command.startsWith("WIFI SET ") && !checkingCloud && !orbitLive::closing() &&
               !directTurn && !recording) {
        if (!orbitLive::provision(command.substring(9))) {
            Serial.println("ORBIT WIFI ERROR");
            return;
        }
        Serial.println("ORBIT WIFI SAVED");
        Serial.flush();
        delay(250);
        ESP.restart();
    } else if ((command == "HELLO LIVE" || command == "HELLO DEMO") && !directTurn) {
        stopRecording();
        stopMic();
        duplexAudio::end();
        wifiPicker::close();
        settingsOpen = false;
        bridgeReady = true;
        showHome();
        Serial.println("ORBIT READY");
    } else if (command == "BYE") {
        stopRecording();
        wifiPicker::close();
        settingsOpen = false;
        bridgeReady = false;
        showHome();
    } else if (command == "THINK") {
        if (recording) {
            stopRecording();
            Serial.println("ORBIT MIC SHORT");
        }
        show(Mood::thinking, "Waiting for the USB bridge...");
    } else if (command.startsWith("SAY ")) {
        showReply(command.substring(4));
        Serial.println("ORBIT SHOWN");
    } else if (command.startsWith("ERROR ")) {
        show(Mood::error, command.substring(6));
    } else if (command.startsWith("PLAY ")) {
        receiveReply(command.substring(5));
    }
}

void readSerial() {
    for (int bytes = 0; bytes < 256 && Serial.available(); ++bytes) {
        const char ch = Serial.read();
        if (ch == '\n') {
            if (!discardLine) handleCommand(line);
            line = "";
            discardLine = false;
        } else if (ch != '\r') {
            if (line.length() < maxLine) line += ch;
            else discardLine = true;
        }
    }
}
}  // namespace

void setup() {
    Serial.setRxBufferSize(usbChunk * 2);
    Serial.setTxBufferSize(usbChunk * 2);
    Serial.begin(115200);
    Serial.setTimeout(2000);
    auto config = M5.config();
    config.pmic_button = false;
    M5.begin(config);
    if (M5.Display.getBoard() != m5::board_t::board_M5StopWatch) {
        Serial.println("ORBIT ERROR Expected StopWatch display");
        return;
    }
    center = M5.Display.width() / 2;
    menuButtonReady = configureMenuButton();
    Serial.println(menuButtonReady ? "ORBIT MENU BUTTON READY" : "ORBIT MENU BUTTON UNAVAILABLE");
    Serial.println(M5.Imu.isEnabled() ? "ORBIT IMU READY" : "ORBIT IMU UNAVAILABLE");
    auto micConfig = M5.Mic.config();
    micConfig.sample_rate = sampleRate;
    M5.Mic.config(micConfig);
    M5.Mic.setBufferReleaseCallback(nullptr, [](void*, void* samples, size_t count) {
        releaseMicBuffer(static_cast<int16_t*>(samples), count);
    });
    capture = static_cast<int16_t*>(ps_malloc(captureLimit * sizeof(int16_t)));
    playback = static_cast<int16_t*>(ps_malloc(playbackLimit * sizeof(int16_t)));
    Preferences preferences;
    if (preferences.begin("orbit", true)) {
        const uint8_t savedVolume = preferences.getUChar("volume", 100);
        if (savedVolume >= 25 && savedVolume <= 100 && savedVolume % 25 == 0) speakerVolume = savedVolume;
        const uint8_t savedTimeout = preferences.getUChar("displayTimeout", 0);
        if (savedTimeout < displayTimeoutCount) displayTimeoutIndex = savedTimeout;
        preferences.end();
    }
    orbitLive::begin();
    conversation.setColorDepth(16);
    conversation.setPsram(true);
    conversation.createSprite(conversationWidth, conversationHeight);
    uiFont::begin();
    flower.setColorDepth(16);
    flower.setPsram(true);
    if (flower.createSprite(avatarSize, avatarSize)) {
        flower.fillScreen(TFT_BLACK);
        flowerReady = flower.drawPng(flowerStart, flowerEnd - flowerStart,
                                     -94, -42, 0, 0, 0, 0, 0.30f) &&
                      flower.readPixel(avatarSize / 2, avatarSize / 2) != TFT_BLACK;
    }
    Serial.println(flowerReady ? "ORBIT AVATAR READY" : "ORBIT AVATAR FALLBACK");
    onboardingAt = millis() + (orbitLive::configured() ? 12000 : 800);
    showHome();
    displayActivityAt = millis();
    Serial.printf("ORBIT BOOT reset %d\n", esp_reset_reason());
}

void loop() {
    if (!center) {
        delay(100);
        return;
    }
    M5.update();
    const bool menuClicked = pollMenuButton();
    updateDisplayTimeout(menuClicked);
    readSerial();
    checkCloud();
    pollStatus();
    if (playing && !directTurn && speakerDrained()) {
        stopSpeaker();
        playing = false;
    }
    serviceTimer();
    const bool tapped = updateTouch(!settingsOpen && !wifiPicker::active());
    if (bluetoothSetup::active()) {
        if (!bluetoothSetup::update(M5.BtnB.wasPressed(), menuClicked)) drawSettings();
        delay(8);
        return;
    }
    if (wifiPicker::active()) {
        if (!wifiPicker::update(M5.BtnA.wasPressed(), M5.BtnB.wasPressed(), menuClicked)) {
            if (settingsOpen) drawSettings();
            else returnToReplyOrHome();
        }
        delay(8);
        return;
    }
    if (menuClicked) {
        if (settingsOpen) closeSettings();
        else openSettings();
        return;
    }
    const bool wasInSettings = settingsOpen;
    updateButtons();
    if (settingsOpen || wasInSettings || wifiPicker::active()) {
        delay(8);
        return;
    }
    updateMic();
    serviceDirect();
    updateReplyCaption();
    updateInputCaption();
    const unsigned long now = millis();
    updateStandby(now);
    if (micRunning() && !recording && (bridgeReady || !orbitLive::ready()) && !playing) {
        stopMic();
        duplexAudio::end();
    }
    if (autoScan && !bridgeReady && static_cast<long>(now - onboardingAt) >= 0) {
        autoScan = false;
        if (!orbitLive::wifiConnected()) {
            openPicker();
            return;
        }
    }
    static const char* previousPrompt = nullptr;
    const char* prompt = homePrompt();
    if (screen == Screen::home && mood == Mood::idle && prompt != previousPrompt) {
        show(Mood::idle, prompt);
        previousPrompt = prompt;
    }
    if (tapped && screen == Screen::home && !playing) avatarBounceAt = now;
    updateAvatarBounce(now);
    if (bridgeReady && mood == Mood::thinking && now - thinkingSince > 45000) {
        show(Mood::error, "Reply timed out");
    }
    delay(recording || (directTurn && !directFinished) ? 1 : 8);
}
