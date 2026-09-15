// uart_bus 组件：UART 主机总线封装（ESP-IDF uart_driver）
//
// 由板型装配层创建并放入 HardwareContext，21VOC 等 UART 传感器从这里拿到端口。
// 与 I2cBus 不同：UART 是点对点，不存在「一条总线挂多个主机」的仲裁问题，
// 这里只做驱动安装/参数配置的封装，不额外加互斥锁。
#pragma once

#include <cstdint>
#include <cstddef>
#include "esp_err.h"
#include "driver/uart.h"

namespace esp32node {

class UartBus {
public:
    static constexpr int kDefaultBaud = 9600;
    static constexpr int kDefaultDataBits = UART_DATA_8_BITS;
    static constexpr int kDefaultParity = UART_PARITY_DISABLE;
    static constexpr int kDefaultStopBits = UART_STOP_BITS_1;
    static constexpr int kDefaultFlowCtrl = UART_HW_FLOWCTRL_DISABLE;

    UartBus() = default;
    ~UartBus();

    // 安装 UART 驱动并配置引脚/波特率（8N1，无流控）
    esp_err_t Init(uart_port_t port, int tx_gpio, int rx_gpio,
                   int baud = kDefaultBaud, int rx_buf_size = 1024, int tx_buf_size = 0);

    // 读取最多 len 字节到 buf，返回实际读到的字节数；timeout_ms 为 0 时非阻塞
    int Read(uint8_t* buf, size_t len, uint32_t timeout_ms = 0);

    // 发送 len 字节，返回实际发送字节数
    int Write(const uint8_t* buf, size_t len);

    uart_port_t Port() const { return port_; }

private:
    uart_port_t port_ = UART_NUM_MAX;  // UART_NUM_MAX 表示未初始化
    bool installed_ = false;
};

} // namespace esp32node
