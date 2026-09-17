#include "sensors/Voc21Sensor.hpp"

#include <cstdio>
#include "esp_log.h"
#include "esp_timer.h"

namespace esp32node {

static const char* TAG = "voc21";

esp_err_t Voc21Sensor::Start(HardwareContext& hw, AppConfig& /*config*/,
                             SensorRegistry& registry)
{
    if (hw.uart == nullptr) {
        ESP_LOGE(TAG, "no uart bus in hardware context");
        return ESP_ERR_INVALID_ARG;
    }
    uart_ = hw.uart;

    // 本地屏幕字段：TV / CH / CO2 / T / H。线上 format_json 保持完整 5 字段。
    static const SensorField kFields[] = {
        {"tvoc",     "TV",  "ug", 0},
        {"ch2o",     "CH",  "ug", 0},
        {"eco2",     "CO2", "ppm",0},
        {"temp",     "T",   "\xC2\xB0" "C", 1},
        {"humidity", "H",   "%",   1},
    };
    registry.Register(Type(), "21VOC",
                      "{\"tvoc\":\"int\",\"ch2o\":\"int\",\"eco2\":\"int\","
                      "\"temp\":\"float\",\"humidity\":\"float\","
                      "\"unit\":\"ug/ug/ppm/C/%\"}",
                      kFields, sizeof(kFields) / sizeof(kFields[0]),
                      &Voc21Sensor::ReadThunk, this);

    ESP_LOGI(TAG, "21VOC ready on uart%d (9600 8N1)", static_cast<int>(uart_->Port()));
    return ESP_OK;
}

void Voc21Sensor::Feed(const uint8_t* data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        uint8_t b = data[i];
        if (rx_len_ == 0) {
            if (b == kFrameHeader) {
                rx_buf_[0] = b;
                rx_len_ = 1;
            }
            // 非帧头字节直接丢弃
        } else if (rx_len_ < kFrameLen) {
            rx_buf_[rx_len_++] = b;
        }

        if (rx_len_ == kFrameLen) {
            // 校验和：B11 = (0x100 - (B0~B10 累加 & 0xFF)) & 0xFF，即累加和的补码
            uint8_t sum = 0;
            for (size_t j = 0; j < kFrameLen - 1; ++j) {
                sum += rx_buf_[j];
            }
            uint8_t expected = static_cast<uint8_t>(0x100u - sum);
            if (expected == rx_buf_[kFrameLen - 1]) {
                tvoc_  = static_cast<uint16_t>((rx_buf_[1] << 8) | rx_buf_[2]);
                ch2o_  = static_cast<uint16_t>((rx_buf_[3] << 8) | rx_buf_[4]);
                eco2_  = static_cast<uint16_t>((rx_buf_[5] << 8) | rx_buf_[6]);
                int16_t raw_t = static_cast<int16_t>((rx_buf_[7] << 8) | rx_buf_[8]);
                temperature_c_ = static_cast<float>(raw_t) * 0.1f;
                uint16_t raw_h = static_cast<uint16_t>((rx_buf_[9] << 8) | rx_buf_[10]);
                humidity_pct_ = static_cast<float>(raw_h) * 0.1f;
                has_valid_ = true;
                if (debug_dump_left_ > 0) {
                    --debug_dump_left_;
                    char hex[3 * kFrameLen] = {};
                    int off = 0;
                    for (size_t j = 0; j < kFrameLen; ++j) {
                        off += std::snprintf(hex + off, sizeof(hex) - off, "%02X ",
                                             rx_buf_[j]);
                    }
                    ESP_LOGI(TAG, "frame[%d]: %s| tvoc=%u ch2o=%u eco2=%u t=%.1f h=%.1f",
                             3 - debug_dump_left_, hex,
                             static_cast<unsigned>((rx_buf_[1] << 8) | rx_buf_[2]),
                             static_cast<unsigned>((rx_buf_[3] << 8) | rx_buf_[4]),
                             static_cast<unsigned>((rx_buf_[5] << 8) | rx_buf_[6]),
                             static_cast<double>(temperature_c_),
                             static_cast<double>(humidity_pct_));
                }
            } else {
                ESP_LOGW(TAG, "checksum mismatch: expected=0x%02X got=0x%02X",
                         expected, rx_buf_[kFrameLen - 1]);
            }
            // 无论校验是否通过，清空缓冲等待下一帧头
            rx_len_ = 0;
        }
    }
}

bool Voc21Sensor::Read(uint16_t* tvoc, uint16_t* ch2o, uint16_t* eco2,
                       float* temperature_c, float* humidity_pct)
{
    if (uart_ == nullptr) {
        return false;
    }

    // 非阻塞读取所有已到达字节并喂入解析器
    uint8_t chunk[64];
    while (true) {
        int n = uart_->Read(chunk, sizeof(chunk), 0);
        if (n <= 0) {
            break;
        }
        Feed(chunk, static_cast<size_t>(n));
        if (static_cast<size_t>(n) < sizeof(chunk)) {
            break;
        }
    }

    if (!has_valid_) {
        return false;
    }
    if (tvoc != nullptr)        *tvoc = tvoc_;
    if (ch2o != nullptr)        *ch2o = ch2o_;
    if (eco2 != nullptr)        *eco2 = eco2_;
    if (temperature_c != nullptr)  *temperature_c = temperature_c_;
    if (humidity_pct != nullptr)   *humidity_pct = humidity_pct_;
    return true;
}

bool Voc21Sensor::ReadThunk(void* ctx, SensorReading* out)
{
    Voc21Sensor* self = static_cast<Voc21Sensor*>(ctx);
    if (self == nullptr || out == nullptr) {
        return false;
    }
    uint16_t tvoc = 0, ch2o = 0, eco2 = 0;
    float t = 0.0f, h = 0.0f;
    if (!self->Read(&tvoc, &ch2o, &eco2, &t, &h)) {
        return false;
    }
    out->ts_ms = esp_timer_get_time() / 1000;
    std::snprintf(out->values_json, sizeof(out->values_json),
                  "{\"tvoc\":%u,\"ch2o\":%u,\"eco2\":%u,\"temp\":%.1f,\"humidity\":%.1f}",
                  static_cast<unsigned>(tvoc), static_cast<unsigned>(ch2o),
                  static_cast<unsigned>(eco2),
                  static_cast<double>(t), static_cast<double>(h));
    return true;
}

} // namespace esp32node
