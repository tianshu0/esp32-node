#include "voc21.h"

#include <cstdio>
#include "esp_log.h"

static const char* TAG = "voc21";

Voc21::Voc21(uart_port_t port)
    : port_(port)
{
}

esp_err_t Voc21::Init()
{
    ESP_LOGI(TAG, "21VOC ready (uart 9600 8N1)");
    return ESP_OK;
}

void Voc21::Feed(const uint8_t* data, size_t len)
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

bool Voc21::Read(uint16_t* tvoc, uint16_t* ch2o, uint16_t* eco2,
                 float* temperature_c, float* humidity_pct)
{
    // 非阻塞读取所有已到达字节并喂入解析器
    uint8_t chunk[64];
    while (true) {
        int n = uart_read_bytes(port_, chunk, sizeof(chunk), 0);
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
