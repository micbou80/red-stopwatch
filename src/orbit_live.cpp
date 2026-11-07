#include "orbit_live.h"
#include "orbit_ca.h"
#include "red_timer.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <array>
#include <atomic>
#include <esp_websocket_client.h>
#include <mbedtls/base64.h>
#include <time.h>
#include <utility>

namespace orbitLive {
namespace {
constexpr size_t maxEvent = 32768;
constexpr size_t maxInputSamples = 960;
constexpr int speechFloor = 384;
constexpr unsigned long writeTimeoutMs = 1500;
constexpr char sessionStart[] = R"JSON({
  "type": "session.start",
  "session": {
    "model": "gpt-live-1",
    "instructions": "You are Red, a friendly pocket flower. Let the user finish speaking, then answer aloud in their language. Keep simple answers brief and give more detail when useful or requested. Use recent conversation to resolve follow-ups. Ask for a location when a forecast has no location. Ask for clarification if speech is unclear. Delegation policy: Generate <|SpawnThinking|> for current information such as weather, and whenever the user asks to start, check, extend, or cancel the watch timer. For timer requests, wait for the backend result before giving a duration or confirming any action.",
    "audio": {"format": {"type": "audio/pcm", "rate": 24000}, "output": {"voice": "marin"}},
    "delegation": {
      "type": "responses",
      "responses": {
        "model": "gpt-6-luna",
        "instructions": "Use web search for current facts and include the relevant location and date. Use watch_timer for explicit timer requests, including follow-ups about remaining time. Use start for a new countdown, status to check, extend to add the requested duration, and cancel to stop. Convert durations to whole seconds. If the action or duration is unclear, ask the user. Run a requested action once. If a timer is already running, tell the user and ask whether to extend or cancel it. Report the actual tool result to the voice model.",
        "tools": [
          {"type": "web_search"},
          {
            "type": "function",
            "name": "watch_timer",
            "description": "Start, check, extend, or cancel the one countdown timer on the watch.",
            "strict": true,
            "parameters": {
              "type": "object",
              "properties": {
                "action": {"type": "string", "enum": ["start", "status", "extend", "cancel"]},
                "seconds": {"type": "integer", "description": "Duration for start or amount to add for extend. Use zero for status and cancel."}
              },
              "required": ["action", "seconds"],
              "additionalProperties": false
            }
          }
        ],
        "parallel_tool_calls": false,
        "tool_choice": "auto"
      }
    }
  }
})JSON";
constexpr char audioPrefix[] = "{\"type\":\"session.input_audio.append\",\"audio\":\"";
String apiHeader;
esp_websocket_client_handle_t socket = nullptr;
std::atomic<bool> connected{false};
std::atomic<bool> accepted{false};
std::atomic<bool> error{false};
std::atomic<bool> apiError{false};
std::atomic<bool> closePending{false};
std::atomic<bool> senderRunning{false};
std::atomic<bool> senderExited{true};
std::atomic<bool> backendPending{false};
std::atomic<unsigned long> backendFinishedAt{0};
struct TimerCall {
    char id[96];
    redTimer::Action action;
    uint32_t seconds;
};
TimerCall timerCall{};
String lastTimerCallId;
std::atomic<bool> timerCallReady{false};
std::atomic<bool> timerContinuing{false};
std::atomic<bool> timerHandled{false};
std::atomic<bool> showCountdown{false};
std::atomic<size_t> received{0};
std::atomic<bool> bufferFull{false};
std::atomic<unsigned long> replyActivityAt{0};
std::atomic<unsigned> audioFrames{0};
std::atomic<int> audioPeak{0};
std::atomic<bool> spokenOutput{false};
constexpr size_t outboundCapacity = sizeof(audioPrefix) + maxInputSamples * 8 / 3 + 4;
char outbound[outboundCapacity];
std::atomic<size_t> pendingWrite{0};
wifi_ps_type_t previousSleep = WIFI_PS_MIN_MODEM;
int16_t* pcm = nullptr;
size_t pcmCapacity = 0;
String eventText;
struct Turn {
    String user;
    String assistant;
};
std::array<Turn, 4> recentTurns;
size_t nextTurn = 0;
size_t turnCount = 0;
template <size_t Capacity> struct Transcript {
    char text[Capacity + 1]{};
    size_t length = 0;
    bool full = false;
};
Transcript<512> userTranscript;
Transcript<2048> replyTranscript;
portMUX_TYPE transcriptLock = portMUX_INITIALIZER_UNLOCKED;

template <size_t Capacity>
void addTranscript(Transcript<Capacity>& transcript, const char* delta) {
    const size_t length = strlen(delta);
    portENTER_CRITICAL(&transcriptLock);
    if (!transcript.full) {
        if (length <= Capacity - transcript.length) {
            memcpy(transcript.text + transcript.length, delta, length + 1);
            transcript.length += length;
        } else {
            transcript.full = true;
        }
    }
    portEXIT_CRITICAL(&transcriptLock);
}

template <size_t Capacity>
String readTranscript(const Transcript<Capacity>& transcript) {
    char copy[sizeof(transcript.text)];
    portENTER_CRITICAL(&transcriptLock);
    memcpy(copy, transcript.text, transcript.length + 1);
    const bool full = transcript.full;
    portEXIT_CRITICAL(&transcriptLock);
    String result(copy);
    if (full) result += "...";
    return result;
}

struct PsramAllocator {
    void* allocate(size_t size) { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM); }
    void deallocate(void* pointer) { heap_caps_free(pointer); }
    void* reallocate(void* pointer, size_t size) {
        return heap_caps_realloc(pointer, size, MALLOC_CAP_SPIRAM);
    }
};

bool validSettings(JsonDocument& settings) {
    const String ssid = settings["ssid"] | "";
    const String password = settings["password"] | "";
    const String key = settings["key"] | "";
    if (ssid.isEmpty() || ssid.length() > 32 || password.length() > 63 ||
        !key.startsWith("sk-") || key.length() > 250) return false;
    for (char ch : key) {
        if (!(isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_')) return false;
    }
    return true;
}

void queueTimerCall(JsonObjectConst item) {
    const char* id = item["call_id"] | "";
    if (!*id || lastTimerCallId == id) return;
    if (timerCallReady || strlen(id) >= sizeof(timerCall.id)) {
        Serial.println("ORBIT TIMER UNEXPECTED CALL");
        error = true;
        return;
    }
    StaticJsonDocument<192> args;
    const auto parsed = deserializeJson(args, item["arguments"] | "");
    const char* action = parsed ? "" : (args["action"] | "");
    const redTimer::Action command = !strcmp(action, "start") ? redTimer::Action::start :
                                    !strcmp(action, "status") ? redTimer::Action::status :
                                    !strcmp(action, "extend") ? redTimer::Action::extend :
                                    !strcmp(action, "cancel") ? redTimer::Action::cancel :
                                    redTimer::Action::invalid;
    strcpy(timerCall.id, id);
    timerCall.action = command;
    timerCall.seconds = args["seconds"].is<uint32_t>() ? args["seconds"].as<uint32_t>() : 0;
    lastTimerCallId = id;
    timerCallReady = true;
}

void handleMessage() {
    StaticJsonDocument<320> filter;
    filter["type"] = true;
    filter["delta"] = true;
    filter["error"]["code"] = true;
    filter["event"]["type"] = true;
    auto itemFilter = filter["event"]["item"];
    itemFilter["type"] = true;
    itemFilter["name"] = true;
    itemFilter["call_id"] = true;
    itemFilter["arguments"] = true;
    BasicJsonDocument<PsramAllocator> event(maxEvent + 256);
    auto parse = deserializeJson(event, eventText, DeserializationOption::Filter(filter));
    if (parse == DeserializationError::IncompleteInput) return;
    eventText = "";
    if (parse) return;
    const char* type = event["type"] | "";
    if (!strcmp(type, "session.started")) {
        accepted = true;
    } else if (!strcmp(type, "error")) {
        Serial.printf("ORBIT LIVE API ERROR %s\n", event["error"]["code"] | "unknown");
        apiError = true;
        error = true;
    } else if (!strcmp(type, "session.input_transcript.delta")) {
        addTranscript(userTranscript, event["delta"] | "");
    } else if (!strcmp(type, "session.output_transcript.delta")) {
        const char* delta = event["delta"] | "";
        addTranscript(replyTranscript, delta);
        if (*delta) {
            spokenOutput = true;
            replyActivityAt = millis();
        }
    } else if (!strcmp(type, "session.delegation.created")) {
        backendPending = true;
    } else if (!strcmp(type, "response.event")) {
        const char* eventType = event["event"]["type"] | "";
        if (!strcmp(eventType, "response.created")) {
            backendPending = true;
            timerContinuing = false;
        } else if (!strcmp(eventType, "response.output_item.done")) {
            const auto item = event["event"]["item"].as<JsonObjectConst>();
            if (!strcmp(item["type"] | "", "function_call") && !strcmp(item["name"] | "", "watch_timer")) {
                queueTimerCall(item);
            }
        }
        if (!strcmp(eventType, "response.web_search_call.completed")) {
            Serial.println("ORBIT LIVE WEB SEARCH COMPLETE");
        }
        if (!strcmp(eventType, "response.completed") || !strcmp(eventType, "response.failed") ||
            !strcmp(eventType, "response.incomplete") || !strcmp(eventType, "response.done")) {
            backendFinishedAt = millis();
            backendPending = false;
        }
    } else if (!strcmp(type, "session.output_audio.delta")) {
        const char* data = event["delta"] | "";
        const size_t offset = received.load();
        if (offset >= pcmCapacity || bufferFull) return;
        size_t bytes = 0;
        const int result = mbedtls_base64_decode(
            reinterpret_cast<uint8_t*>(pcm + offset), (pcmCapacity - offset) * 2,
            &bytes, reinterpret_cast<const uint8_t*>(data), strlen(data));
        if (result == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
            bufferFull = true;
        } else if (result == 0 && bytes && !(bytes % 2)) {
            int peak = 0;
            for (size_t i = 0; i < bytes / 2; ++i) {
                peak = max(peak, abs(static_cast<int>(pcm[offset + i])));
            }
            ++audioFrames;
            audioPeak = max(audioPeak.load(), peak);
            if (peak >= speechFloor) spokenOutput = true;
            // Some sessions send quiet frames before speech starts. They
            // must not start playback or the end-of-reply timer.
            if (peak < speechFloor && !offset) return;
            if (peak >= speechFloor) {
                if (!offset) Serial.printf("ORBIT LIVE SPEECH PEAK %d\n", peak);
                const float gain = min(4.0f, max(1.0f, 24000.0f / peak));
                for (size_t i = 0; i < bytes / 2; ++i) {
                    pcm[offset + i] = static_cast<int16_t>(pcm[offset + i] * gain);
                }
                replyActivityAt = millis();
            }
            received = offset + bytes / 2;
        }
    }
}

void socketEvent(void*, esp_event_base_t, int32_t eventId, void* data) {
    switch (eventId) {
        case WEBSOCKET_EVENT_CONNECTED:
            connected = true;
            break;
        case WEBSOCKET_EVENT_ERROR:
        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_CLOSED:
            connected = false;
            error = true;
            break;
        case WEBSOCKET_EVENT_DATA: {
            const auto* event = static_cast<const esp_websocket_event_data_t*>(data);
            if (event->op_code != 1 && event->op_code != 0) break;
            if (event->op_code == 1 && event->payload_offset == 0) eventText = "";
            if (eventText.length() + event->data_len > maxEvent ||
                !eventText.concat(event->data_ptr, event->data_len)) {
                error = true;
                break;
            }
            if (event->payload_offset + event->data_len == event->payload_len) handleMessage();
            break;
        }
    }
}

bool send(const char* message, size_t length, unsigned long timeoutMs = writeTimeoutMs) {
    int sent = esp_websocket_client_send_text(socket, message, length, pdMS_TO_TICKS(timeoutMs));
    // A lock timeout leaves the socket connected and sends no bytes. A
    // transport write failure instead aborts the socket in the ESP client.
    if (sent < 0 && esp_websocket_client_is_connected(socket)) {
        sent = esp_websocket_client_send_text(socket, message, length, pdMS_TO_TICKS(timeoutMs));
    }
    if (sent != static_cast<int>(length)) {
        Serial.printf("ORBIT LIVE WRITE ERROR %d of %u\n", sent, static_cast<unsigned>(length));
        return false;
    }
    return true;
}

bool sendTimerResult(const TimerCall& call) {
    const auto result = redTimer::apply(call.action, call.seconds);
    showCountdown = result.timer.state != redTimer::State::idle && !strcmp(result.status, "ok");
    const char* state = result.timer.state == redTimer::State::running ? "running" :
                        result.timer.state == redTimer::State::finished ? "finished" : "idle";
    char output[128];
    snprintf(output, sizeof(output), "{\"status\":\"%s\",\"state\":\"%s\",\"seconds_remaining\":%lu}",
             result.status, state, static_cast<unsigned long>(result.timer.seconds));
    StaticJsonDocument<320> event;
    event["type"] = "response.item.create";
    event["event_id"] = call.id;
    auto item = event.createNestedObject("item");
    item["type"] = "function_call_output";
    item["call_id"] = call.id;
    item["output"] = output;
    char message[512];
    const size_t size = serializeJson(event, message);
    if (!send(message, size)) return false;
    event.clear();
    event["type"] = "response.create";
    event["event_id"] = String("continue-") + call.id;
    const size_t continuationSize = serializeJson(event, message);
    Serial.printf("ORBIT TIMER %s %s %lu\n", result.status, state,
                  static_cast<unsigned long>(result.timer.seconds));
    return send(message, continuationSize);
}

void addHistoryMessage(JsonArray input, const char* role, const char* contentType, const String& text) {
    JsonObject message = input.createNestedObject();
    message["type"] = "message";
    message["role"] = role;
    JsonObject content = message.createNestedArray("content").createNestedObject();
    content["type"] = contentType;
    content["text"] = text.c_str();
}

bool sendSessionStart() {
    if (!pcmCapacity || !turnCount) return send(sessionStart, sizeof(sessionStart) - 1);
    BasicJsonDocument<PsramAllocator> request(6144);
    if (deserializeJson(request, sessionStart)) return false;
    JsonArray input = request["session"].createNestedArray("input");
    const size_t first = (nextTurn + recentTurns.size() - turnCount) % recentTurns.size();
    for (size_t i = 0; i < turnCount; ++i) {
        const Turn& turn = recentTurns[(first + i) % recentTurns.size()];
        addHistoryMessage(input, "user", "input_text", turn.user);
        addHistoryMessage(input, "assistant", "output_text", turn.assistant);
    }
    String message;
    const size_t length = measureJson(request);
    if (request.overflowed() || !message.reserve(length + 1) || serializeJson(request, message) != length) {
        return false;
    }
    Serial.printf("ORBIT LIVE CONTEXT %u turns\n", static_cast<unsigned>(turnCount));
    return send(message.c_str(), message.length());
}

void senderTask(void*) {
    bool started = false;
    while (senderRunning && !error) {
        if (!started && connected) {
            started = true;
            if (!sendSessionStart()) error = true;
        }
        if (timerCallReady && !closePending) {
            const TimerCall call = timerCall;
            timerHandled = true;
            timerContinuing = true;
            timerCallReady = false;
            if (!sendTimerResult(call)) error = true;
        }
        const size_t size = pendingWrite.load();
        if (size) {
            if (!send(outbound, size)) error = true;
            pendingWrite = 0;
        } else {
            vTaskDelay(1);
        }
    }
    senderExited = true;
    vTaskDelete(nullptr);
}
}

void begin() {
    Preferences nvs;
    if (!nvs.begin("orbit", true)) return;
    String saved = nvs.getString("setup");
    nvs.end();
    StaticJsonDocument<768> settings;
    if (deserializeJson(settings, saved) || !validSettings(settings)) return;
    apiHeader = "Authorization: Bearer ";
    apiHeader += settings["key"].as<const char*>();
    apiHeader += "\r\n";
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(settings["ssid"].as<const char*>(), settings["password"].as<const char*>());
    configTime(0, 0, "time.cloudflare.com", "pool.ntp.org");
}

bool provision(const String& setup) {
    StaticJsonDocument<768> settings;
    if (deserializeJson(settings, setup)) return false;
    Preferences nvs;
    if (!nvs.begin("orbit", false)) return false;
    if (String(settings["key"] | "").isEmpty()) {
        StaticJsonDocument<768> previous;
        if (!deserializeJson(previous, nvs.getString("setup"))) {
            settings["key"] = String(previous["key"] | "");
        }
    }
    String merged;
    if (validSettings(settings)) serializeJson(settings, merged);
    const bool stored = !merged.isEmpty() && nvs.putString("setup", merged) == merged.length();
    nvs.end();
    return stored;
}

bool configured() { return !apiHeader.isEmpty(); }
bool wifiConnected() { return WiFi.status() == WL_CONNECTED; }
int wifiSignal() { return wifiConnected() ? WiFi.RSSI() : 0; }
bool ready() { return wifiConnected() && time(nullptr) > 1700000000; }
bool listening() { return accepted && !error; }
bool failed() { return error; }
bool canRetryTurn() {
    return !apiError && !spokenOutput && !backendPending && !backendFinishedAt &&
           !timerCallReady && !timerHandled;
}
bool canAppend() { return listening() && !pendingWrite.load(); }
bool closing() { return closePending.load(); }
bool waitingForBackend() {
    const unsigned long ended = backendFinishedAt.load();
    return backendPending || timerCallReady || timerContinuing || (ended && millis() - ended < 3500);
}
bool timerViewRequested() { return showCountdown; }
size_t available() { return received.load(); }
bool hasSpokenOutput() { return spokenOutput.load(); }
bool outputFull() { return bufferFull || received >= pcmCapacity; }
unsigned long lastReplyActivity() { return replyActivityAt.load(); }

bool start(int16_t* output, size_t capacity) {
    if (closing() || socket || !ready()) return false;
    connected = false;
    accepted = false;
    error = false;
    apiError = false;
    backendPending = false;
    backendFinishedAt = 0;
    lastTimerCallId = "";
    timerCallReady = false;
    timerContinuing = false;
    timerHandled = false;
    showCountdown = false;
    received = 0;
    bufferFull = false;
    replyActivityAt = 0;
    audioFrames = 0;
    audioPeak = 0;
    spokenOutput = false;
    pendingWrite = 0;
    pcm = output;
    pcmCapacity = capacity;
    userTranscript = {};
    replyTranscript = {};
    eventText = "";
    esp_websocket_client_config_t config{};
    config.uri = "wss://api.openai.com/v1/live/sessions";
    config.headers = apiHeader.c_str();
    config.cert_pem = orbitRootCa;
    config.disable_auto_reconnect = true;
    config.task_stack = 8192;
    config.buffer_size = 4096;
    socket = esp_websocket_client_init(&config);
    if (!socket) return false;
    previousSleep = WiFi.getSleep();
    WiFi.setSleep(WIFI_PS_NONE);
    if (esp_websocket_register_events(socket, WEBSOCKET_EVENT_ANY, socketEvent, nullptr) == ESP_OK &&
        esp_websocket_client_start(socket) == ESP_OK) {
        senderRunning = true;
        senderExited = false;
        if (xTaskCreate(senderTask, "red-send", 6144, nullptr, 2, nullptr) == pdPASS) return true;
        senderRunning = false;
        senderExited = true;
    }
    esp_websocket_client_destroy(socket);
    socket = nullptr;
    WiFi.setSleep(previousSleep);
    return false;
}

bool append(const int16_t* samples, size_t count) {
    if (!canAppend() || count > maxInputSamples) {
        Serial.println("ORBIT LIVE AUDIO UNAVAILABLE");
        return false;
    }
    memcpy(outbound, audioPrefix, sizeof(audioPrefix) - 1);
    size_t size = 0;
    if (mbedtls_base64_encode(
            reinterpret_cast<uint8_t*>(outbound + sizeof(audioPrefix) - 1),
            sizeof(outbound) - sizeof(audioPrefix), &size,
            reinterpret_cast<const uint8_t*>(samples), count * 2)) {
        Serial.println("ORBIT LIVE AUDIO ENCODE ERROR");
        return false;
    }
    size += sizeof(audioPrefix) - 1;
    outbound[size++] = '"';
    outbound[size++] = '}';
    pendingWrite = size;
    return true;
}

bool requestReply() {
    constexpr char prompt[] = R"JSON({"type":"session.instructions.append","event_id":"red-reply-reminder","delegation_id":null,"content":"The user has released the talk button and finished their question. Answer aloud now, or ask them to repeat it if unclear."})JSON";
    if (!canAppend()) return false;
    memcpy(outbound, prompt, sizeof(prompt) - 1);
    pendingWrite = sizeof(prompt) - 1;
    return true;
}

void close() {
    if (!socket) return;
    senderRunning = false;
    while (!senderExited) vTaskDelay(1);
    constexpr char end[] = "{\"type\":\"session.close\"}";
    if (accepted && connected) {
        send(end, sizeof(end) - 1, 350);
        esp_websocket_client_close(socket, pdMS_TO_TICKS(350));
    }
    esp_websocket_client_destroy(socket);
    socket = nullptr;
    WiFi.setSleep(previousSleep);
    if (pcmCapacity) {
        Serial.printf("ORBIT LIVE OUTPUT %u frames peak %d caption bytes %u\n",
                      audioFrames.load(), audioPeak.load(), static_cast<unsigned>(userTranscript.length));
    }
}

void closeAsync() {
    if (closing() || !socket) return;
    closePending = true;
    if (xTaskCreate([](void*) {
            close();
            closePending = false;
            vTaskDelete(nullptr);
        }, "red-close", 6144, nullptr, 1, nullptr) != pdPASS) {
        close();
        closePending = false;
    }
}

String caption() { return readTranscript(replyTranscript); }
String inputCaption() { return readTranscript(userTranscript); }

void rememberTurn(String answer) {
    constexpr size_t textLimit = 256;
    String question = inputCaption();
    question.trim();
    answer.trim();
    if (question.isEmpty() || answer.isEmpty()) return;
    for (String* text : {&question, &answer}) {
        if (text->length() <= textLimit) continue;
        size_t end = textLimit;
        while (end && (static_cast<uint8_t>((*text)[end]) & 0xc0) == 0x80) --end;
        text->remove(end);
    }
    recentTurns[nextTurn] = {std::move(question), std::move(answer)};
    nextTurn = (nextTurn + 1) % recentTurns.size();
    turnCount = min(turnCount + 1, recentTurns.size());
}
}
