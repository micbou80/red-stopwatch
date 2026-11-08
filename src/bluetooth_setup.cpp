#include "bluetooth_setup.h"
#include "orbit_live.h"
#include "ui_font.h"

#include <WiFi.h>
#include <atomic>
#include <esp_system.h>
#include <esp_wifi.h>
#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_ble.h>

namespace bluetoothSetup {
namespace {
constexpr unsigned long pairingWindowMs = 120000;
enum class Page { closed, starting, pairing, joining, rejected, saving, done, error };
enum class Progress { waiting, joining, rejected, joined };
Page page = Page::closed;
std::atomic<Progress> progress{Progress::waiting};
std::atomic<bool> ended{false};
bool managerOpen = false;
bool saved = false;
unsigned long startedAt = 0;
char serviceName[20];
char pairingCode[9];
const char* errorMessage = "";

void draw() {
    auto& display = M5.Display;
    const int center = display.width() / 2;
    display.fillScreen(TFT_BLACK);
    display.setTextDatum(top_center);
    display.setTextSize(1);
    display.setFont(uiFont::label());
    display.setTextColor(TFT_CYAN, TFT_BLACK);
    if (page == Page::pairing) {
        char payload[96];
        snprintf(payload, sizeof(payload),
                 "{\"ver\":\"v1\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"ble\",\"security\":1}",
                 serviceName, pairingCode);
        display.drawString("Scan in ESP BLE Provisioning", center, 84);
        display.qrcode(payload, center - 100, 111, 200, 1, true);
        display.setTextColor(TFT_WHITE, TFT_BLACK);
        display.drawString(serviceName, center, 325);
        display.drawString(String("Code: ") + pairingCode, center, 352);
        display.drawString("B: Back", center, 385);
        return;
    }
    const char* message = page == Page::starting ? "Starting Bluetooth..." :
                          page == Page::joining ? "Joining Wi-Fi..." :
                          page == Page::rejected ? "Could not join Wi-Fi" :
                          page == Page::saving ? "Wi-Fi joined" :
                          page == Page::done ? "Wi-Fi saved" : errorMessage;
    display.setFont(uiFont::title());
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.drawString(message, center, 196);
    display.setFont(uiFont::label());
    if (page == Page::done && !orbitLive::hasKey()) {
        display.drawString("Use USB setup to add the OpenAI key", center, 235);
    }
    display.setTextColor(TFT_CYAN, TFT_BLACK);
    display.drawString(page == Page::error || page == Page::rejected ? "A+B: Retry     B: Back" : "B: Back", center, 355);
}

void onProvisionEvent(void*, wifi_prov_cb_event_t event, void*) {
    switch (event) {
        case WIFI_PROV_CRED_RECV: progress = Progress::joining; break;
        case WIFI_PROV_CRED_FAIL: progress = Progress::rejected; break;
        case WIFI_PROV_CRED_SUCCESS: progress = Progress::joined; break;
        case WIFI_PROV_END: ended = true; break;
        default: break;
    }
}

void finish(bool keepWifi) {
    if (managerOpen) {
        wifi_prov_mgr_deinit();
        managerOpen = false;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (!keepWifi) {
        WiFi.mode(WIFI_OFF);
        orbitLive::begin();
    }
    WiFi.setAutoReconnect(true);
}

void fail(const char* message) {
    finish(false);
    errorMessage = message;
    page = Page::error;
    draw();
}

void start() {
    WiFi.persistent(false);
    if (!WiFi.mode(WIFI_STA)) {
        fail("Could not start Wi-Fi");
        return;
    }
    WiFi.setAutoReconnect(false);
    uint8_t mac[6]{};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(serviceName, sizeof(serviceName), "PROV_RED_%02X%02X%02X", mac[3], mac[4], mac[5]);
    snprintf(pairingCode, sizeof(pairingCode), "%08lX", static_cast<unsigned long>(esp_random()));
    progress = Progress::waiting;
    ended = false;
    saved = false;
    wifi_prov_mgr_config_t config{};
    config.scheme = wifi_prov_scheme_ble;
    config.app_event_handler.event_cb = onProvisionEvent;
    esp_err_t result = wifi_prov_mgr_init(config);
    if (result == ESP_OK) {
        managerOpen = true;
        result = wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1, pairingCode, serviceName, nullptr);
    }
    if (result != ESP_OK) {
        Serial.printf("ORBIT BLE START ERROR %s\n", esp_err_to_name(result));
        fail("Bluetooth setup failed");
        return;
    }
    startedAt = millis();
    page = Page::pairing;
    draw();
    Serial.println("ORBIT BLE PAIRING");
}

bool saveConnectedNetwork() {
    wifi_config_t network{};
    if (esp_wifi_get_config(WIFI_IF_STA, &network) != ESP_OK) return false;
    char ssid[sizeof(network.sta.ssid) + 1]{};
    char password[sizeof(network.sta.password) + 1]{};
    memcpy(ssid, network.sta.ssid, sizeof(network.sta.ssid));
    memcpy(password, network.sta.password, sizeof(network.sta.password));
    return orbitLive::saveWifi(ssid, password);
}
}

void open() {
    page = Page::starting;
    saved = false;
    draw();
}

bool active() { return page != Page::closed; }
void redraw() { draw(); }

bool update(bool retry, bool back) {
    if (back) {
        if (managerOpen) finish(saved);
        page = Page::closed;
        return false;
    }
    if (page == Page::starting && !orbitLive::closing()) {
        start();
    } else if (retry && (page == Page::rejected || page == Page::error)) {
        if (managerOpen) finish(false);
        open();
    } else if (managerOpen) {
        const auto state = progress.load();
        if (state == Progress::joined && page != Page::saving) {
            saved = saveConnectedNetwork();
            if (!saved) {
                fail("Could not save Wi-Fi");
                return true;
            }
            page = Page::saving;
            draw();
        }
        if (page == Page::saving) {
            // Give the phone time to read the connection result before shutting down BLE.
            if (ended || millis() - startedAt >= pairingWindowMs) {
                finish(true);
                page = Page::done;
                draw();
                Serial.println("ORBIT BLE WIFI SAVED");
            }
        } else if (ended || millis() - startedAt >= pairingWindowMs) {
            fail("Bluetooth pairing timed out");
        } else if (state == Progress::joining && page != Page::joining) {
            page = Page::joining;
            draw();
        } else if (state == Progress::rejected && page != Page::rejected) {
            page = Page::rejected;
            draw();
        }
    }
    return true;
}
}
