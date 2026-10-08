#include "serial_console.h"
#include "improv.h"
#include <cstdio>
#include <cstring>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mc/platform.h"
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG || CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG
#define MC_CONSOLE_USJ 1
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#endif
#if CONFIG_ESP_CONSOLE_UART
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#endif

namespace {
constexpr size_t LINE_CAP = 128;
struct Line { char text[LINE_CAP]; };
QueueHandle_t s_lines = nullptr;

struct LineBuffer {
    char buf[LINE_CAP];
    size_t len = 0;
    void feed(const uint8_t* data, int n) {
        for (int i = 0; i < n; i++) {
            char ch = (char)data[i];
            if (ch == '\r' || ch == '\n') {
                if (!len) continue;
                Line l;
                memcpy(l.text, buf, len);
                l.text[len] = 0;
                len = 0;
                // a full queue drops the line: the game loop is far behind anyway
                if (xQueueSend(s_lines, &l, 0) == pdTRUE) mc::plat::wake();
            } else if (ch == '\b' || ch == 0x7f) {
                if (len) len--;
            } else if (len < LINE_CAP - 1 && ch >= ' ') {
                buf[len++] = ch;
            }
        }
    }
};

void readerTask(void*) {
    uint8_t data[64];
#ifdef MC_CONSOLE_USJ
    LineBuffer usj;
#endif
#if CONFIG_ESP_CONSOLE_UART
    LineBuffer uart;
#endif
    for (;;) {
#ifdef MC_CONSOLE_USJ
        {
            int n = usb_serial_jtag_read_bytes(data, sizeof(data), pdMS_TO_TICKS(20));
            improvHandleSerialData(data, n > 0 ? (size_t)n : 0);
            usj.feed(data, n);
        }
#endif
#if CONFIG_ESP_CONSOLE_UART
        {
            int n = uart_read_bytes((uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM, data, sizeof(data), pdMS_TO_TICKS(20));
            improvHandleSerialData(data, n > 0 ? (size_t)n : 0);
            uart.feed(data, n);
        }
#endif
    }
}
}  // namespace

void serialConsoleStart() {
    bool any = false;
#ifdef MC_CONSOLE_USJ
    // with the driver, output to an unplugged USB port is dropped after a short timeout
    usb_serial_jtag_driver_config_t usj = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    if (usb_serial_jtag_driver_install(&usj) == ESP_OK) {
        usb_serial_jtag_vfs_use_driver();
        any = true;
    }
#endif
#if CONFIG_ESP_CONSOLE_UART
    uart_port_t port = (uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM;
    if (uart_driver_install(port, 256, 0, 0, nullptr, 0) == ESP_OK) {
        uart_vfs_dev_use_driver(port);
        any = true;
    }
#endif
    if (!any) return;
    s_lines = xQueueCreate(4, sizeof(Line));
    if (!s_lines || xTaskCreatePinnedToCore(readerTask, "console", 3072, nullptr, 1, nullptr, 0) != pdPASS)
        puts("serial console unavailable");
}

bool serialConsoleLine(char* buf, size_t cap) {
    Line l;
    if (!s_lines || xQueueReceive(s_lines, &l, 0) != pdTRUE) return false;
    strncpy(buf, l.text, cap - 1);
    buf[cap - 1] = 0;
    return true;
}
