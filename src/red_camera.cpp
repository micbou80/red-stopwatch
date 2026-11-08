#include "red_camera.h"

#include <ArduinoJson.h>
#include <atomic>
#include <esp_http_client.h>

// Arduino's WiFiClientSecure header shadows the ESP-IDF header of the same name.
extern "C" esp_err_t esp_crt_bundle_attach(void* config);

namespace redCamera {
namespace {
constexpr size_t snapshotCapacity = 384 * 1024;
constexpr size_t maxStreamFrame = 1024 * 1024;

esp_http_client_config_t clientConfig(const char* url, int timeout) {
    esp_http_client_config_t config{};
    config.url = url;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.disable_auto_redirect = true;
    config.timeout_ms = timeout;
    return config;
}

void authorize(esp_http_client_handle_t client, const char* token) {
    const String authorization = String("Bearer ") + token;
    esp_http_client_set_header(client, "Authorization", authorization.c_str());
}

struct Body {
    Body(uint8_t* data, size_t capacity) : data(data), capacity(capacity) {}

    uint8_t* data;
    size_t capacity;
    size_t size = 0;
    bool overflow = false;
};

esp_err_t receive(esp_http_client_event_t* event) {
    if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    auto& body = *static_cast<Body*>(event->user_data);
    if (body.overflow || static_cast<size_t>(event->data_len) > body.capacity - body.size) {
        body.overflow = true;
        return ESP_FAIL;
    }
    memcpy(body.data + body.size, event->data, event->data_len);
    body.size += event->data_len;
    return ESP_OK;
}

int request(const char* url, const char* token, Body& body, const String* payload = nullptr,
            int timeout = 10000) {
    auto config = clientConfig(url, timeout);
    config.event_handler = receive;
    config.user_data = &body;
    config.method = payload ? HTTP_METHOD_POST : HTTP_METHOD_GET;
    auto client = esp_http_client_init(&config);
    if (!client) return 0;
    authorize(client, token);
    if (payload) {
        esp_http_client_set_header(client, "Accept", "application/json");
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, payload->c_str(), payload->length());
    }
    const auto result = esp_http_client_perform(client);
    const int status = result == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);
    return body.overflow ? 413 : status;
}

bool isCameraId(const char* entity) {
    constexpr char prefix[] = "camera.";
    if (strncmp(entity, prefix, sizeof(prefix) - 1) || strlen(entity) > 255) return false;
    entity += sizeof(prefix) - 1;
    if (!*entity) return false;
    for (; *entity; ++entity) {
        if (!((*entity >= 'a' && *entity <= 'z') || (*entity >= '0' && *entity <= '9') ||
              *entity == '_')) return false;
    }
    return true;
}

uint16_t readU16(const uint8_t* data) {
    return (static_cast<uint16_t>(data[0]) << 8) | data[1];
}

bool readDimensions(Snapshot& snapshot) {
    const uint8_t* data = snapshot.data;
    if (snapshot.size >= 24 && !memcmp(data, "\x89PNG\r\n\x1a\n", 8)) {
        if (memcmp(data + 12, "IHDR", 4) || readU16(data + 16) || readU16(data + 20)) return false;
        snapshot.width = readU16(data + 18);
        snapshot.height = readU16(data + 22);
        return snapshot.width && snapshot.height;
    }
    if (snapshot.size < 4 || readU16(data) != 0xffd8) return false;
    for (size_t offset = 2; offset + 4 <= snapshot.size;) {
        if (data[offset++] != 0xff) return false;
        while (offset < snapshot.size && data[offset] == 0xff) ++offset;
        if (offset + 3 > snapshot.size) return false;
        const uint8_t marker = data[offset++];
        if (marker == 0xda || marker == 0xd9) return false;
        const size_t length = readU16(data + offset);
        if (length < 2 || length > snapshot.size - offset) return false;
        if (marker == 0xc0 || marker == 0xc1 || marker == 0xc2) {
            if (length < 7) return false;
            snapshot.height = readU16(data + offset + 3);
            snapshot.width = readU16(data + offset + 5);
            return snapshot.width && snapshot.height;
        }
        offset += length;
    }
    return false;
}

const char* fetchSnapshot(const Source& source, Snapshot& snapshot, int timeout, bool logStatus) {
    snapshot.data = static_cast<uint8_t*>(ps_malloc(snapshotCapacity));
    if (!snapshot.data) return "out_of_memory";
    Body body{snapshot.data, snapshotCapacity};
    const int status = request(source.imageUrl.c_str(), source.token.c_str(), body, nullptr, timeout);
    if (logStatus) Serial.printf("ORBIT CAMERA IMAGE HTTP %d\n", status);
    if (status == 401 || status == 403) return "home_assistant_access_denied";
    if (status == 413) return "snapshot_too_large";
    if (status != 200 || !body.size) return "snapshot_unavailable";
    snapshot.size = body.size;
    return readDimensions(snapshot) ? "ok" : "unsupported_snapshot_format";
}

struct Feed {
    explicit Feed(const Source& source) : source(source) {}
    ~Feed() { delete latest.load(); }

    void publish(std::unique_ptr<Snapshot> frame) {
        delete latest.exchange(frame.release());
        unavailable = false;
    }

    const Source source;
    std::atomic<bool> stopped{false};
    std::atomic<bool> snapshotsOnly{false};
    std::atomic<bool> unavailable{false};
    std::atomic<Snapshot*> latest{nullptr};
};

std::shared_ptr<Feed> activeFeed;
std::atomic<bool> feedRunning{false};

class StreamReader {
public:
    StreamReader(esp_http_client_handle_t client, const std::atomic<bool>& stopped)
        : client(client), stopped(stopped) {}

    bool read(uint8_t* destination, size_t size) {
        while (size && !stopped) {
            if (offset == available && !refill()) return false;
            const size_t count = min(size, available - offset);
            memcpy(destination, buffer + offset, count);
            offset += count;
            destination += count;
            size -= count;
        }
        return size == 0;
    }

    size_t nextFrameSize() {
        char header[256];
        if (!readLine(header, sizeof(header))) return 0;
        if (!*header && !readLine(header, sizeof(header))) return 0;
        if (strcmp(header, "--frameboundary")) return 0;
        size_t size = 0;
        for (int lines = 0; lines < 12 && readLine(header, sizeof(header)); ++lines) {
            if (!*header) return size;
            constexpr char lengthHeader[] = "Content-Length:";
            if (strncasecmp(header, lengthHeader, sizeof(lengthHeader) - 1)) continue;
            char* end;
            const char* value = header + sizeof(lengthHeader) - 1;
            const unsigned long length = strtoul(value, &end, 10);
            if (end == value || *end || !length || length > maxStreamFrame || size) return 0;
            size = length;
        }
        return 0;
    }

private:
    bool refill() {
        const unsigned long started = millis();
        while (!stopped && millis() - started < 12000) {
            const int count = esp_http_client_read(client, reinterpret_cast<char*>(buffer), sizeof(buffer));
            if (count > 0) {
                offset = 0;
                available = count;
                return true;
            }
            if (count < 0 || esp_http_client_is_complete_data_received(client)) return false;
            vTaskDelay(1);
        }
        return false;
    }

    bool readLine(char* line, size_t capacity) {
        for (size_t i = 0; i + 1 < capacity; ++i) {
            uint8_t ch;
            if (!read(&ch, 1)) return false;
            if (ch == '\n') {
                line[i && line[i - 1] == '\r' ? i - 1 : i] = '\0';
                return true;
            }
            line[i] = ch;
        }
        return false;
    }

    esp_http_client_handle_t client;
    const std::atomic<bool>& stopped;
    uint8_t buffer[1024];
    size_t offset = 0;
    size_t available = 0;
};

void receiveStream(Feed& feed, esp_http_client_handle_t client) {
    StreamReader reader(client, feed.stopped);
    bool first = true;
    while (!feed.stopped && !feed.snapshotsOnly) {
        const size_t size = reader.nextFrameSize();
        if (!size) return;
        std::unique_ptr<Snapshot> frame(new (std::nothrow) Snapshot);
        if (!frame) return;
        frame->data = static_cast<uint8_t*>(ps_malloc(size));
        if (!frame->data || !reader.read(frame->data, size) || feed.stopped) return;
        frame->size = size;
        if (!readDimensions(*frame)) return;
        feed.publish(std::move(frame));
        if (first) {
            first = false;
            Serial.println("ORBIT CAMERA STREAM FRAME");
        }
    }
}

void runFeed(Feed& feed) {
    auto config = clientConfig(feed.source.streamUrl.c_str(), 5000);
    if (auto client = esp_http_client_init(&config)) {
        authorize(client, feed.source.token.c_str());
        if (!feed.stopped && esp_http_client_open(client, 0) == ESP_OK &&
            esp_http_client_fetch_headers(client) >= 0) {
            const int status = esp_http_client_get_status_code(client);
            Serial.printf("ORBIT CAMERA STREAM HTTP %d\n", status);
            if (status == 200) {
                esp_http_client_set_timeout_ms(client, 400);
                receiveStream(feed, client);
            }
        }
        esp_http_client_cleanup(client);
    }
    if (feed.stopped) return;
    Serial.println("ORBIT CAMERA STREAM REFRESHING");
    while (!feed.stopped) {
        const unsigned long started = millis();
        std::unique_ptr<Snapshot> frame(new (std::nothrow) Snapshot);
        const bool loaded = frame && !strcmp(fetchSnapshot(feed.source, *frame, 5000, false), "ok");
        if (feed.stopped) return;
        if (loaded) feed.publish(std::move(frame));
        else feed.unavailable = true;
        while (!feed.stopped && millis() - started < 1000) vTaskDelay(pdMS_TO_TICKS(50));
        if (!loaded && !feed.stopped) vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

const char* resolve(const char* endpoint, const char* token, const char* lookupTool, bool returnsIds,
                    const char* name, const char* area, const String& origin, String& entity) {
    StaticJsonDocument<512> call;
    call["jsonrpc"] = "2.0";
    call["id"] = 1;
    call["method"] = "tools/call";
    auto params = call.createNestedObject("params");
    params["name"] = lookupTool;
    auto args = params.createNestedObject("arguments");
    if (returnsIds) args.createNestedArray("domain").add("camera");
    else args["domain"] = "camera";
    if (*name) args["name"] = name;
    if (*area) args["area"] = area;
    String payload;
    serializeJson(call, payload);

    constexpr size_t capacity = 12288;
    std::unique_ptr<uint8_t, decltype(&free)> buffer(
        static_cast<uint8_t*>(ps_malloc(capacity + 1)), free);
    if (!buffer) return "out_of_memory";
    Body body{buffer.get(), capacity};
    const int status = request(endpoint, token, body, &payload);
    Serial.printf("ORBIT CAMERA MCP HTTP %d\n", status);
    if (status == 401 || status == 403) return "home_assistant_access_denied";
    if (status == 413) return "multiple_camera_matches";
    if (status != 200) return "camera_lookup_failed";

    buffer.get()[body.size] = '\0';
    StaticJsonDocument<1024> envelope;
    if (deserializeJson(envelope, reinterpret_cast<char*>(buffer.get()), body.size)) {
        return "camera_lookup_failed";
    }
    auto result = envelope["result"].as<JsonObjectConst>();
    if (result.isNull()) return "camera_lookup_failed";
    if (result["isError"] | false) return "no_exposed_camera_match";
    JsonObjectConst details = result["structuredContent"];
    size_t detailsLength = 0;
    if (!returnsIds) {
        // GetLiveContext checks Assist exposure and provides names, but omits entity IDs.
        // The read-only Assist intent resolves the same target to an ID for the image endpoint.
        call.clear();
        call["name"] = "HassGetState";
        call["assistant"] = "conversation";
        auto slots = call.createNestedObject("data");
        slots.createNestedArray("domain").add("camera");
        if (*name) slots["name"] = name;
        if (*area) slots["area"] = area;
        payload = "";
        serializeJson(call, payload);
        body.size = 0;
        const String intentUrl = origin + "/api/intent/handle";
        const int intentStatus = request(intentUrl.c_str(), token, body, &payload);
        Serial.printf("ORBIT CAMERA INTENT HTTP %d\n", intentStatus);
        if (intentStatus != 200 || !body.size) return "camera_lookup_failed";
        detailsLength = body.size;
    } else if (details.isNull()) {
        // The MCP server wraps Home Assistant's JSON intent result in a text item.
        const auto content = result["content"][0];
        if (strcmp(content["type"] | "", "text")) return "camera_lookup_failed";
        const char* response = content["text"] | "";
        detailsLength = strlen(response);
        memmove(buffer.get(), response, detailsLength + 1);
    }
    if (detailsLength) {
        envelope.clear();
        StaticJsonDocument<192> filter;
        filter["data"]["success"][0]["id"] = true;
        const auto parsed = deserializeJson(envelope, reinterpret_cast<char*>(buffer.get()), detailsLength,
                                            DeserializationOption::Filter(filter));
        if (parsed == DeserializationError::NoMemory) return "multiple_camera_matches";
        if (parsed) return "camera_lookup_failed";
        details = envelope.as<JsonObjectConst>();
    }
    for (auto match : details["data"]["success"].as<JsonArrayConst>()) {
        const char* id = match["id"] | "";
        if (!isCameraId(id)) continue;
        if (!entity.isEmpty() && entity != id) return "multiple_camera_matches";
        entity = id;
    }
    return entity.isEmpty() ? "no_exposed_camera_match" : "ok";
}
}

const char* fetch(const char* endpoint, const char* token, const char* lookupTool, bool returnsIds,
                  const char* name, const char* area, View& view) {
    if (!*name && !*area) return "camera_name_or_area_required";
    String url(endpoint);
    url.remove(url.length() - strlen("/api/mcp/assist"));
    String entity;
    const char* lookup = resolve(endpoint, token, lookupTool, returnsIds, name, area, url, entity);
    if (strcmp(lookup, "ok")) return lookup;

    view.source = {url + "/api/camera_proxy/" + entity + "?width=800&height=800",
                   url + "/api/camera_proxy_stream/" + entity + "?interval=1", token};
    view.firstFrame.reset(new (std::nothrow) Snapshot);
    return view.firstFrame ? fetchSnapshot(view.source, *view.firstFrame, 10000, true) : "out_of_memory";
}

void start(const Source& source) {
    if (activeFeed || feedRunning) return;
    auto feed = std::make_shared<Feed>(source);
    auto* task = new (std::nothrow) std::shared_ptr<Feed>(feed);
    activeFeed = feed;
    feedRunning = true;
    if (task && xTaskCreate([](void* context) {
            std::unique_ptr<std::shared_ptr<Feed>> task(static_cast<std::shared_ptr<Feed>*>(context));
            runFeed(**task);
            task.reset();
            Serial.println("ORBIT CAMERA STREAM CLOSED");
            feedRunning = false;
            vTaskDelete(nullptr);
        }, "red-camera", 7168, task, 1, nullptr) == pdPASS) return;
    delete task;
    feedRunning = false;
    feed->unavailable = true;
}

void stop() {
    if (!activeFeed) return;
    activeFeed->stopped = true;
    activeFeed.reset();
}

void useSnapshots() {
    if (activeFeed) activeFeed->snapshotsOnly = true;
}

std::unique_ptr<Snapshot> takeFrame() {
    return std::unique_ptr<Snapshot>(activeFeed ? activeFeed->latest.exchange(nullptr) : nullptr);
}

bool unavailable() {
    return activeFeed && activeFeed->unavailable;
}
}
