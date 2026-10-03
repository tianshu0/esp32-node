#include "display.h"

Display::Display() {
}
Display::~Display() {
}

void Display::SetStatus(const char* status) {
    ESP_LOGI("display", "SetStatus: %s", status);
}
void Display::SetupUI() {
    ESP_LOGI("display", "SetupUI");
}


