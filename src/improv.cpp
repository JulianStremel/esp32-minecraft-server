// Improv Serial (https://www.improv-wifi.com/serial/): WiFi credentials from the web
// flasher (ESP Web Tools) or any other Improv client, over the console's serial port.
#include "improv.h"
#include "firmware_config.h"
#include "network.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
constexpr std::array<uint8_t, 6> HEADER = {'I', 'M', 'P', 'R', 'O', 'V'};
constexpr uint8_t VERSION = 1;

enum Type : uint8_t {
    CURRENT_STATE = 0x01,
    ERROR_STATE = 0x02,
    RPC = 0x03,
    RPC_RESPONSE = 0x04,
};

enum State : uint8_t {
    STATE_READY = 0x02,          // "authorized": serial needs no authorization
    STATE_PROVISIONING = 0x03,
    STATE_PROVISIONED = 0x04,
};

enum Error : uint8_t {
    ERROR_NONE = 0x00,
    ERROR_INVALID_RPC = 0x01,
    ERROR_UNKNOWN_RPC = 0x02,
    ERROR_UNABLE_TO_CONNECT = 0x03,
    ERROR_UNKNOWN = 0xFF,
};

// The serial variant's command numbers (BLE numbers them differently)
enum Command : uint8_t {
    CMD_WIFI_SETTINGS = 0x01,
    CMD_GET_CURRENT_STATE = 0x02,
    CMD_GET_DEVICE_INFO = 0x03,
    CMD_GET_WIFI_NETWORKS = 0x04,
};

#if CONFIG_IDF_TARGET_ESP32P4
constexpr char CHIP[] = "ESP32-P4";
#else
constexpr char CHIP[] = "ESP32-S3";
#endif

struct Parser {
    size_t headerPos = 0;
    bool gotHeader = false;
    size_t framePos = 0;
    uint8_t type = 0;
    uint8_t payloadLen = 0;
    std::array<uint8_t, 255> payload = {};
} parser;

uint8_t calcChecksum(uint8_t type, const uint8_t* payload, size_t payloadLen) {
    uint32_t sum = 0;
    for (uint8_t b : HEADER) sum += b;
    sum += VERSION;
    sum += type;
    sum += (uint8_t)payloadLen;
    for (size_t i = 0; i < payloadLen; i++) sum += payload[i];
    return (uint8_t)(sum & 0xff);
}

// One fwrite per frame: stdout's lock keeps other tasks' log lines out of it.
void sendFrame(uint8_t type, const std::vector<uint8_t>& payload) {
    if (payload.size() > 255) return;
    std::vector<uint8_t> frame(HEADER.begin(), HEADER.end());
    frame.push_back(VERSION);
    frame.push_back(type);
    frame.push_back((uint8_t)payload.size());
    frame.insert(frame.end(), payload.begin(), payload.end());
    frame.push_back(calcChecksum(type, payload.data(), payload.size()));
    frame.push_back('\n');   // keeps a terminal watching the console readable
    fwrite(frame.data(), 1, frame.size(), stdout);
    fflush(stdout);
}

void sendState(uint8_t state) { sendFrame(CURRENT_STATE, {state}); }
void sendError(uint8_t code) { sendFrame(ERROR_STATE, {code}); }

void sendRpcResponse(uint8_t command, const std::vector<std::string>& values) {
    std::vector<uint8_t> payload = {command, 0};
    for (const auto& value : values) {
        if (value.size() > 255 || payload.size() + 1 + value.size() > 255) return;
        payload.push_back((uint8_t)value.size());
        payload.insert(payload.end(), value.begin(), value.end());
    }
    payload[1] = (uint8_t)(payload.size() - 2);
    sendFrame(RPC_RESPONSE, payload);
}

// Where to go once the board is online: the status dashboard, which shows the address
// to join (builds without it have no page to offer).
std::vector<std::string> redirectUrls() {
#if MC_DASHBOARD
    char ip[16];
    if (MC_DASHBOARD_PORT && networkIp(ip, sizeof(ip))) {
        char url[48];
        if (MC_DASHBOARD_PORT == 80) snprintf(url, sizeof(url), "http://%s/", ip);
        else snprintf(url, sizeof(url), "http://%s:%d/", ip, (int)MC_DASHBOARD_PORT);
        return {url};
    }
#endif
    return {};
}

void sendCurrentState() {
    if (networkHasIp()) {
        sendState(STATE_PROVISIONED);
        sendRpcResponse(CMD_GET_CURRENT_STATE, redirectUrls());
    } else {
        sendState(STATE_READY);
    }
}

void handleRpc(const uint8_t* payload, size_t len) {
    // command, data length, data
    if (len < 2 || payload[1] != len - 2) {
        sendError(ERROR_INVALID_RPC);
        return;
    }
    const uint8_t* data = payload + 2;
    size_t dataLen = payload[1];
    switch (payload[0]) {
        case CMD_WIFI_SETTINGS: {
            // ssid length, ssid, password length, password
            if (dataLen < 2 || 1 + (size_t)data[0] + 1 > dataLen) {
                sendError(ERROR_INVALID_RPC);
                return;
            }
            size_t ssidLen = data[0];
            size_t passLen = data[1 + ssidLen];
            if (2 + ssidLen + passLen != dataLen || !ssidLen) {
                sendError(ERROR_INVALID_RPC);
                return;
            }
            std::string ssid((const char*)data + 1, ssidLen);
            std::string pass((const char*)data + 2 + ssidLen, passLen);
            sendError(ERROR_NONE);
            sendState(STATE_PROVISIONING);
            printf("improv: connecting to the WiFi network the client sent\n");
            if (!networkProvision(ssid.c_str(), pass.c_str(), 30000)) {
                printf("improv: could not connect; the previous settings stay\n");
                sendError(ERROR_UNABLE_TO_CONNECT);
                sendState(STATE_READY);
                return;
            }
            sendState(STATE_PROVISIONED);
            sendRpcResponse(CMD_WIFI_SETTINGS, redirectUrls());
            return;
        }
        case CMD_GET_CURRENT_STATE:
            sendCurrentState();
            return;
        case CMD_GET_DEVICE_INFO: {
            const esp_app_desc_t* app = esp_app_get_description();
            sendRpcResponse(CMD_GET_DEVICE_INFO,
                            {"ESP32 Minecraft server", app ? app->version : "dev", CHIP, MC_HOSTNAME});
            return;
        }
        case CMD_GET_WIFI_NETWORKS: {
            // one response per network (name, signal in dBm, whether it needs a password),
            // then an empty one
            std::vector<WifiNetwork> nets(16);   // the console task's stack is small
            int n = networkScan(nets.data(), (int)nets.size());
            for (int i = 0; i < n; i++)
                sendRpcResponse(CMD_GET_WIFI_NETWORKS,
                                {nets[i].ssid, std::to_string(nets[i].rssi), nets[i].secured ? "YES" : "NO"});
            sendRpcResponse(CMD_GET_WIFI_NETWORKS, {});
            return;
        }
        default:
            sendError(ERROR_UNKNOWN_RPC);
            return;
    }
}

// A byte after the header (version, type, length, payload, checksum)
void feedFrame(uint8_t b) {
    size_t pos = parser.framePos++;
    if (pos == 0) {
        if (b != VERSION) parser = {};
    } else if (pos == 1) {
        parser.type = b;
    } else if (pos == 2) {
        parser.payloadLen = b;
    } else if (pos < 3 + (size_t)parser.payloadLen) {
        parser.payload[pos - 3] = b;
    } else {
        if (parser.type == RPC) {
            if (b == calcChecksum(parser.type, parser.payload.data(), parser.payloadLen))
                handleRpc(parser.payload.data(), parser.payloadLen);
            else
                sendError(ERROR_INVALID_RPC);
        }
        parser = {};
    }
}
}  // namespace

size_t improvHandleSerialData(const uint8_t* data, size_t size, uint8_t* rest) {
    // a frame cut short (bytes lost while the board booted) must not swallow console
    // input: clients send a frame at once, so a pause of half a second ends it
    static int64_t lastByteUs = 0;
    int64_t now = esp_timer_get_time();
    if (size && (parser.gotHeader || parser.headerPos) && now - lastByteUs > 500000) parser = {};
    if (size) lastByteUs = now;
    size_t n = 0;
    for (size_t i = 0; i < size; i++) {
        uint8_t b = data[i];
        if (parser.gotHeader) {
            feedFrame(b);
            continue;
        }
        if (b == HEADER[parser.headerPos]) {
            if (++parser.headerPos == HEADER.size()) parser.gotHeader = true;
            continue;
        }
        // not a header after all: its bytes are console input
        for (size_t k = 0; k < parser.headerPos; k++) rest[n++] = HEADER[k];
        parser.headerPos = b == HEADER[0] ? 1 : 0;
        if (!parser.headerPos) rest[n++] = b;
    }
    return n;
}
