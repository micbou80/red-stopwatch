#include "wifi_picker.h"
#include "orbit_live.h"
#include "ui_font.h"

#include <M5Unified.h>
#include <WiFi.h>
#include <esp_wifi.h>

namespace wifiPicker {
namespace {
constexpr int limit = 5;
struct Network {
    String ssid;
    int32_t signal;
};
Network networks[limit];
String chosen;
enum class Page { closed, preparing, results, selected };
Page page = Page::closed;
bool reconnect = false;
bool configured = false;
bool scanFailed = false;
unsigned long scanAt = 0;
int count = 0;
int cursor = 0;

String displayName(const String& ssid, int width) {
    String label;
    for (char byte : ssid) {
        const auto ch = static_cast<unsigned char>(byte);
        label += ch >= 32 && ch < 127 ? byte : '?';
    }
    auto& display = M5.Display;
    if (display.textWidth(label) <= width) return label;
    while (label.length() && display.textWidth(label + "...") > width) {
        label.remove(label.length() - 1);
    }
    return label + "...";
}

void draw() {
    auto& display = M5.Display;
    const int center = display.width() / 2;
    display.fillScreen(TFT_BLACK);
    display.setTextDatum(top_center);
    display.setTextSize(1);
    display.setFont(uiFont::label());
    display.setTextColor(TFT_CYAN, TFT_BLACK);

    if (page == Page::preparing) {
        display.drawString("Scanning for Wi-Fi...", center, 217);
        display.drawString("B: Back", center, 348);
        return;
    }
    if (page == Page::selected) {
        display.setFont(uiFont::title());
        display.setTextColor(TFT_WHITE, TFT_BLACK);
        display.drawString(displayName(chosen, 336), center, 191);
        display.setFont(uiFont::label());
        display.drawString("Continue in the Windows setup window", center, 235);
        display.drawString("B: Back to networks", center, 348);
        return;
    }
    if (!count) {
        display.drawString(scanFailed ? "Wi-Fi scan failed" : "No visible Wi-Fi networks", center, 217);
        display.drawString("A+B: Scan again     B: Back", center, 348);
        return;
    }
    for (int row = 0; row < count; ++row) {
        const int y = 106 + row * 40;
        const uint16_t background = row == cursor ? display.color565(22, 65, 81) : TFT_BLACK;
        display.fillRoundRect(center - 175, y, 350, 34, 8, background);
        display.setTextColor(TFT_WHITE, background);
        display.setTextDatum(middle_left);
        display.drawString(displayName(networks[row].ssid, 245), center - 159, y + 17);
        display.setTextDatum(middle_right);
        display.drawString(String(networks[row].signal) + " dBm", center + 159, y + 17);
    }
    display.setTextDatum(top_center);
    display.setTextColor(TFT_CYAN, TFT_BLACK);
    display.drawString("2.4 GHz  /  strongest first", center, 319);
    display.drawString("A: Next     B: Back     A+B: Select", center, 348);
}

void insert(const String& ssid, int32_t signal) {
    if (ssid.isEmpty()) return;
    for (int i = 0; i < count; ++i) {
        if (networks[i].ssid != ssid) continue;
        if (networks[i].signal >= signal) return;
        for (int j = i; j + 1 < count; ++j) networks[j] = networks[j + 1];
        --count;
        break;
    }
    int position = 0;
    while (position < count && networks[position].signal >= signal) ++position;
    if (position == limit) return;
    if (count < limit) ++count;
    for (int i = count - 1; i > position; --i) networks[i] = networks[i - 1];
    networks[position] = {ssid, signal};
}

void resumeConnection() {
    if (!reconnect) return;
    orbitLive::begin();
    reconnect = false;
}

void finishScan(esp_err_t result) {
    scanFailed = result != ESP_OK;
    if (scanFailed) Serial.printf("ORBIT WIFI DRIVER %s\n", esp_err_to_name(result));
    resumeConnection();
    page = Page::results;
    Serial.printf("ORBIT WIFI SCAN %d\n", scanFailed ? -1 : count);
    draw();
}

esp_err_t scanRadio(wifi_scan_type_t type) {
    wifi_scan_config_t options{};
    options.scan_type = type;
    if (type == WIFI_SCAN_TYPE_PASSIVE) options.scan_time.passive = 220;
    else options.scan_time.active.max = 180;
    esp_err_t result = esp_wifi_scan_start(&options, true);
    uint16_t found = 0;
    if (result == ESP_OK) result = esp_wifi_scan_get_ap_num(&found);
    Serial.printf("ORBIT WIFI RADIO %s APs %u status %s\n",
                  type == WIFI_SCAN_TYPE_PASSIVE ? "passive" : "active",
                  found, esp_err_to_name(result));
    if (result == ESP_OK && found) {
        auto* records = static_cast<wifi_ap_record_t*>(
            heap_caps_calloc(found, sizeof(wifi_ap_record_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (records) {
            result = esp_wifi_scan_get_ap_records(&found, records);
            if (result == ESP_OK) {
                size_t named = 0;
                for (uint16_t i = 0; i < found; ++i) {
                    if (records[i].ssid[0]) {
                        ++named;
                        insert(reinterpret_cast<const char*>(records[i].ssid), records[i].rssi);
                    }
                }
                Serial.printf("ORBIT WIFI RECORDS %u named %u\n", found, static_cast<unsigned>(named));
            }
            free(records);
        } else {
            esp_wifi_clear_ap_list();
            result = ESP_ERR_NO_MEM;
        }
    }
    return result;
}

void scan() {
    const int found = WiFi.scanNetworks();
    Serial.printf("ORBIT WIFI ARDUINO %d\n", found);
    for (int i = 0; i < found && i < 256; ++i) insert(WiFi.SSID(i), WiFi.RSSI(i));
    WiFi.scanDelete();
    esp_err_t result = ESP_OK;
    if (!count) result = scanRadio(WIFI_SCAN_TYPE_ACTIVE);
    if (result == ESP_OK && !count) result = scanRadio(WIFI_SCAN_TYPE_PASSIVE);
    finishScan(result);
}
}

void open(bool savedNetwork) {
    if (page == Page::preparing) return;
    configured = savedNetwork;
    WiFi.persistent(false);
    reconnect = savedNetwork && WiFi.status() != WL_CONNECTED;
    if (reconnect) {
        WiFi.setAutoReconnect(false);
        // A pending join takes precedence over a scan on the ESP32. Reset
        // the station before scanning, then reload the saved network.
        WiFi.mode(WIFI_OFF);
    }
    const bool stationStarted = WiFi.mode(WIFI_STA);
    scanAt = millis() + 250;
    count = 0;
    cursor = 0;
    page = Page::preparing;
    if (stationStarted) draw();
    else finishScan(ESP_ERR_WIFI_NOT_STARTED);
}

void close() {
    resumeConnection();
    page = Page::closed;
}

bool active() { return page != Page::closed; }
void redraw() { draw(); }

bool update(bool next, bool select, bool back) {
    if (back) {
        if (page == Page::selected) {
            chosen = "";
            page = Page::results;
            draw();
            return true;
        }
        close();
        return false;
    }
    if (page == Page::preparing && static_cast<long>(millis() - scanAt) >= 0) {
        scan();
    } else if (page == Page::results && count && (next || select)) {
        if (select) {
            chosen = networks[cursor].ssid;
            page = Page::selected;
        } else {
            cursor = (cursor + 1) % count;
        }
        draw();
    } else if (page == Page::results && !count && select) {
        close();
        open(configured);
    }
    return true;
}

const String& selected() { return chosen; }
}
