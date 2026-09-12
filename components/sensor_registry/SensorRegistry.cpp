#include "sensor_registry/SensorRegistry.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include "esp_log.h"

namespace esp32node {

static const char* TAG = "sensor_registry";

// 追加格式化文本，返回新的写入偏移。
// 始终为结尾 '\0' 预留 1 字节，截断时停在容量上限。
static int AppendFmt(char* buf, int cap, int off, const char* fmt, ...)
{
    if (off < 0) {
        off = 0;
    }
    if (off >= cap - 1) {
        return cap - 1;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + off, static_cast<size_t>(cap - off), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return off;
    }
    return (n >= cap - off) ? (cap - 1) : (off + n);
}

esp_err_t SensorRegistry::Init()
{
    count_ = 0;
    for (int i = 0; i < kMaxSensors; ++i) {
        entries_[i] = Entry{};
    }
    std::strncpy(types_json_, "[]", sizeof(types_json_) - 1);
    std::strncpy(detail_json_, "[]", sizeof(detail_json_) - 1);
    ESP_LOGI(TAG, "sensor registry ready (max %d sensors)", kMaxSensors);
    return ESP_OK;
}

esp_err_t SensorRegistry::Register(const char* type, const char* model,
                                   const char* format_json,
                                   SensorReadFn read, void* ctx)
{
    if (type == nullptr || type[0] == '\0' || read == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (count_ >= kMaxSensors) {
        ESP_LOGW(TAG, "registry full, drop sensor %s", type);
        return ESP_ERR_NO_MEM;
    }

    Entry& e = entries_[count_];
    std::strncpy(e.type, type, sizeof(e.type) - 1);
    std::strncpy(e.model, model ? model : "unknown", sizeof(e.model) - 1);
    std::strncpy(e.format, format_json ? format_json : "{}", sizeof(e.format) - 1);
    e.read = read;
    e.ctx = ctx;
    ++count_;

    RebuildJson();
    ESP_LOGI(TAG, "registered sensor: %s (%s)", e.type, e.model);
    return ESP_OK;
}

const char* SensorRegistry::TypeAt(int index) const
{
    if (index < 0 || index >= count_) {
        return "";
    }
    return entries_[index].type;
}

int SensorRegistry::ReadAll(SensorReading* out, int max) const
{
    if (out == nullptr || max <= 0) {
        return 0;
    }
    int n = 0;
    for (int i = 0; i < count_ && n < max; ++i) {
        const Entry& e = entries_[i];
        if (e.read == nullptr) {
            continue;
        }
        SensorReading r = {};
        if (!e.read(e.ctx, &r)) {
            ESP_LOGW(TAG, "read failed: %s", e.type);
            continue;
        }
        // 类型由注册表统一回填，驱动只填 ts_ms / values_json
        std::strncpy(r.type, e.type, sizeof(r.type) - 1);
        r.type[sizeof(r.type) - 1] = '\0';
        r.values_json[sizeof(r.values_json) - 1] = '\0';
        out[n++] = r;
    }
    return n;
}

void SensorRegistry::RebuildJson()
{
    // types: ["temp_hum","pressure"]
    int off = AppendFmt(types_json_, sizeof(types_json_), 0, "[");
    for (int i = 0; i < count_; ++i) {
        off = AppendFmt(types_json_, sizeof(types_json_), off, "%s\"%s\"",
                        i ? "," : "", entries_[i].type);
    }
    AppendFmt(types_json_, sizeof(types_json_), off, "]");

    // detail: [{"type":..,"model":..,"format":{..}}]
    off = AppendFmt(detail_json_, sizeof(detail_json_), 0, "[");
    for (int i = 0; i < count_; ++i) {
        off = AppendFmt(detail_json_, sizeof(detail_json_), off,
                        "%s{\"type\":\"%s\",\"model\":\"%s\",\"format\":%s}",
                        i ? "," : "", entries_[i].type, entries_[i].model,
                        entries_[i].format);
    }
    AppendFmt(detail_json_, sizeof(detail_json_), off, "]");
}

} // namespace esp32node
