#include "duplex_audio.h"

#include <M5Unified.h>
#include <atomic>
#include <driver/i2s.h>
#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>
#include <utility/M5IOE1_Class.hpp>

namespace duplexAudio {
namespace {
constexpr i2s_port_t port = I2S_NUM_0;
constexpr int sampleRate = 24000;
constexpr size_t frames = 240;
constexpr int echoFilterMs = 100;
constexpr uint8_t codecAddress = 0x18;

template <typename Sample> struct Buffer {
    Buffer(Sample* samples = nullptr, size_t count = 0, size_t interval = 0)
        : data(samples), length(count), progress(interval) {}
    Sample* data = nullptr;
    size_t length = 0;
    size_t offset = 0;
    size_t progress = 0;
};

Buffer<int16_t> input[2];
Buffer<const int16_t> output[2];
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
std::atomic<bool> running{false};
std::atomic<bool> workerExited{true};
Release releaseInput = nullptr;
unsigned long lastOutputAt = 0;
unsigned long outputStartedAt = 0;
uint8_t outputVolume = 100;
bool amplifierOn = false;
SpeexEchoState* echo = nullptr;
SpeexPreprocessState* preprocess = nullptr;

bool codec(uint8_t reg, uint8_t value) {
    return M5.In_I2C.writeRegister8(codecAddress, reg, value, 100000);
}

void amplifier(bool enabled) {
    if (amplifierOn == enabled) return;
    M5.getIOExpander(0).digitalWrite(m5::M5IOE1_Class::gpio10, enabled);
    amplifierOn = enabled;
}

bool configureCodec() {
    // Combine the StopWatch ADC and DAC setup on the same 16-bit stereo
    // clock. Arm the analog paths after I2S has started.
    constexpr uint8_t registers[][2] = {
        {0x00, 0x80}, {0x01, 0xBF}, {0x02, 0x18}, {0x09, 0x0C},
        {0x0A, 0x0C}, {0x0E, 0x02},
        {0x12, 0x00}, {0x13, 0x10}, {0x14, 0x17}, {0x17, 0xCB},
        {0x1C, 0x64}, {0x32, 0xC7}, {0x37, 0x08},
    };
    for (const auto& reg : registers) {
        if (!codec(reg[0], reg[1])) return false;
    }
    delay(35);
    return codec(0x0D, 0x01);
}

bool fillOutput(int16_t* stereo, int16_t* reference) {
    bool audible = false;
    portENTER_CRITICAL(&lock);
    if (output[0].data) lastOutputAt = millis();
    for (size_t frame = 0; frame < frames; ++frame) {
        int16_t sample = 0;
        if (output[0].data) {
            auto& current = output[0];
            sample = static_cast<int32_t>(current.data[current.offset++]) * outputVolume * 3 / 400;
            if (current.offset == current.length) {
                output[0] = output[1];
                output[1] = {};
            }
        }
        stereo[2 * frame] = stereo[2 * frame + 1] = sample;
        reference[frame] = sample;
        audible |= sample != 0;
    }
    portEXIT_CRITICAL(&lock);
    return audible;
}

void deliverInput(const int16_t* samples) {
    portENTER_CRITICAL(&lock);
    size_t copied = 0;
    while (copied < frames && input[0].data) {
        auto& current = input[0];
        const size_t count = min(frames - copied, current.length - current.offset);
        memcpy(current.data + current.offset, samples + copied, count * sizeof(int16_t));
        current.offset += count;
        copied += count;
        if (current.progress && current.offset % current.progress == 0) {
            releaseInput(current.data, current.offset);
        }
        if (current.offset == current.length) {
            if (!current.progress) releaseInput(current.data, current.length);
            input[0] = input[1];
            input[1] = {};
        }
    }
    portEXIT_CRITICAL(&lock);
}

void audioTask(void*) {
    int16_t stereo[2 * frames];
    int16_t mic[frames];
    int16_t reference[frames];
    int16_t cleaned[frames];
    unsigned echoTail = 0;
    while (running) {
        size_t transferred = 0;
        if (fillOutput(stereo, reference)) echoTail = echoFilterMs / 10;
        i2s_write(port, stereo, sizeof(stereo), &transferred, pdMS_TO_TICKS(20));
        if (i2s_read(port, stereo, sizeof(stereo), &transferred, pdMS_TO_TICKS(20)) == ESP_OK &&
            transferred == sizeof(stereo)) {
            for (size_t frame = 0; frame < frames; ++frame) mic[frame] = stereo[2 * frame + 1];
            if (echoTail) {
                speex_echo_cancellation(echo, mic, reference, cleaned);
                speex_preprocess_run(preprocess, cleaned);
                --echoTail;
            } else {
                memcpy(cleaned, mic, sizeof(cleaned));
            }
            for (auto& sample : cleaned) sample = constrain(static_cast<int>(sample) * 4, -32768, 32767);
            deliverInput(cleaned);
        }
    }
    workerExited = true;
    vTaskDelete(nullptr);
}

void releaseEcho() {
    if (preprocess) speex_preprocess_state_destroy(preprocess);
    if (echo) speex_echo_state_destroy(echo);
    preprocess = nullptr;
    echo = nullptr;
}
}

bool begin(Release release) {
    if (running) return true;
    auto& expander = M5.getIOExpander(0);
    expander.digitalWrite(m5::M5IOE1_Class::gpio3, true);
    expander.digitalWrite(m5::M5IOE1_Class::gpio10, false);
    delay(5);
    i2s_config_t config{};
    config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX);
    config.sample_rate = sampleRate;
    config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    config.dma_buf_count = 4;
    config.dma_buf_len = 120;
    config.tx_desc_auto_clear = true;
    const i2s_pin_config_t pins{GPIO_NUM_18, GPIO_NUM_17, GPIO_NUM_15, GPIO_NUM_21, GPIO_NUM_16};
    if (i2s_driver_install(port, &config, 0, nullptr) != ESP_OK) return false;
    if (i2s_set_pin(port, &pins) != ESP_OK || !configureCodec()) {
        i2s_driver_uninstall(port);
        return false;
    }
    i2s_zero_dma_buffer(port);
    echo = speex_echo_state_init(frames, sampleRate * echoFilterMs / 1000);
    preprocess = speex_preprocess_state_init(frames, sampleRate);
    if (!echo || !preprocess) {
        releaseEcho();
        i2s_driver_uninstall(port);
        return false;
    }
    int rate = sampleRate;
    int denoise = 0;
    int suppress = -45;
    int suppressActive = -20;
    speex_echo_ctl(echo, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
    speex_preprocess_ctl(preprocess, SPEEX_PREPROCESS_SET_DENOISE, &denoise);
    speex_preprocess_ctl(preprocess, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS, &suppress);
    speex_preprocess_ctl(preprocess, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE, &suppressActive);
    speex_preprocess_ctl(preprocess, SPEEX_PREPROCESS_SET_ECHO_STATE, echo);
    releaseInput = release;
    running = true;
    workerExited = false;
    if (xTaskCreatePinnedToCore(audioTask, "red-audio", 6144, nullptr, 3, nullptr, 1) != pdPASS) {
        running = false;
        workerExited = true;
        releaseEcho();
        i2s_driver_uninstall(port);
        return false;
    }
    return true;
}

void end() {
    if (!running) return;
    stopOutput();
    clearInput();
    running = false;
    while (!workerExited) delay(1);
    releaseEcho();
    codec(0x0D, 0xFC);
    i2s_driver_uninstall(port);
}

bool active() { return running; }

bool record(int16_t* samples, size_t count, size_t progress) {
    portENTER_CRITICAL(&lock);
    const bool queued = !input[1].data;
    if (queued) input[input[0].data ? 1 : 0] = {samples, count, progress};
    portEXIT_CRITICAL(&lock);
    return queued;
}

void clearInput() {
    portENTER_CRITICAL(&lock);
    input[0] = input[1] = {};
    portEXIT_CRITICAL(&lock);
}

bool play(const int16_t* samples, size_t count, uint8_t volume) {
    if (!amplifierOn) outputStartedAt = millis();
    amplifier(true);
    portENTER_CRITICAL(&lock);
    const bool queued = !output[1].data;
    if (queued) {
        outputVolume = volume;
        output[output[0].data ? 1 : 0] = {samples, count};
    }
    portEXIT_CRITICAL(&lock);
    return queued;
}

unsigned queuedOutput() {
    portENTER_CRITICAL(&lock);
    const unsigned queued = (output[0].data != nullptr) + (output[1].data != nullptr);
    portEXIT_CRITICAL(&lock);
    return queued;
}

bool drained() {
    portENTER_CRITICAL(&lock);
    const bool done = !output[0].data && millis() - lastOutputAt >= 45;
    portEXIT_CRITICAL(&lock);
    return done;
}

bool interruptible() { return amplifierOn && millis() - outputStartedAt >= 180; }

void stopOutput() {
    amplifier(false);
    portENTER_CRITICAL(&lock);
    output[0] = output[1] = {};
    portEXIT_CRITICAL(&lock);
}
}
