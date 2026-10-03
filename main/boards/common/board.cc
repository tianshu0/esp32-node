#include "board.h"
#include "display.h"

#include <esp_random.h>

#include <cstdio>

Board::Board() {
    uuid_ = GenerateUuid();
}

Display* Board::GetDisplay() {
    static NoDisplay no_display;
    return &no_display;
}

std::string Board::GenerateUuid() {
    uint8_t bytes[16];
    for (int i = 0; i < 16; ++i) {
        bytes[i] = static_cast<uint8_t>(esp_random() & 0xFF);
    }
    // RFC 4122 version 4 + variant 10xx
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    bytes[8] = (bytes[8] & 0x3F) | 0x80;

    char buf[37];
    std::snprintf(buf, sizeof(buf),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  bytes[0], bytes[1], bytes[2], bytes[3],
                  bytes[4], bytes[5], bytes[6], bytes[7],
                  bytes[8], bytes[9], bytes[10], bytes[11],
                  bytes[12], bytes[13], bytes[14], bytes[15]);
    return std::string(buf);
}
