// Optional profiling build only. Runtime counters use the firmware's esp_timer
// clock; trace timestamps in esp-emu can drift from that clock under load.
#include <cstdio>
#include <cinttypes>
#include "esp_err.h"
#include "esp_ipc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
void checkpoint(void*) {}

void sampleTask(void*) {
    static TaskStatus_t tasks[48];
    static char line[6144];
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        // A brief IPC preemption makes core 1 account its current task's time.
        // Core 0 has already switched into this sampling task.
        ESP_ERROR_CHECK(esp_ipc_call_blocking(1, checkpoint, nullptr));
        const int64_t us = esp_timer_get_time();
        const UBaseType_t n = uxTaskGetSystemState(tasks, 48, nullptr);
        ESP_ERROR_CHECK(n ? ESP_OK : ESP_ERR_NO_MEM);
        int used = snprintf(line, sizeof(line), "[cpu] {\"us\":%" PRId64 ",\"tasks\":[", us);
        for (UBaseType_t i = 0; i < n; ++i) {
            const int written = snprintf(line + used, sizeof(line) - used,
                   "%s{\"id\":%u,\"name\":\"%s\",\"affinity\":%d,\"runtime_us\":%" PRIu64 "}",
                   i ? "," : "", (unsigned)tasks[i].xTaskNumber, tasks[i].pcTaskName,
                   (int)tasks[i].xCoreID, (uint64_t)tasks[i].ulRunTimeCounter);
            ESP_ERROR_CHECK(written >= 0 && (size_t)written + 3 < sizeof(line) - used ? ESP_OK : ESP_ERR_NO_MEM);
            used += written;
        }
        snprintf(line + used, sizeof(line) - used, "]}");
        puts(line);
    }
}
}

void startCpuProfile() {
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(sampleTask, "cpu_profile", 4096, nullptr,
        configMAX_PRIORITIES - 2, nullptr, 0) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
