#include "improv.h"
#include "network.h"
#include "esp_app_desc.h"
#include <array>
#include <cstdio>
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
    STATE_STOPPED = 0x00,
    STATE_AUTHORIZED = 0x02,
    STATE_PROVISIONING = 0x03,
    STATE_PROVISIONED = 0x04,
};

enum Error : uint8_t {
    ERROR_INVALID_RPC = 0x01,
    ERROR_UNKNOWN_RPC = 0x02,
    ERROR_UNABLE_TO_CONNECT = 0x03,
};

enum Command : uint8_t {
    CMD_WIFI_SETTINGS = 0x01,
    CMD_IDENTIFY = 0x02,
    CMD_GET_CURRENT_STATE = 0x03,
    CMD_GET_DEVICE_INFO = 0x04,
};

struct Parser {
    size_t headerPos = 0;
    bool gotHeader = false;
    size_t framePos = 0;
    uint8_t version = 0;
    uint8_t type = 0;
    uint8_t payloadLen = 0;
    std::array<uint8_t, 255> payload = {};
    uint8_t checksum = 0;
} parser;

uint8_t calcChecksum(uint8_t type, const uint8_t* payload, uint8_t payloadLen) {
    uint32_t sum = 0;
    for (uint8_t b : HEADER) sum += b;
    sum += VERSION;
    sum += type;
    sum += payloadLen;
    for (int i = 0; i < payloadLen; i++) sum += payload[i];
    return (uint8_t)(sum & 0xff);
}

void sendFrame(uint8_t type, const std::vector<uint8_t>& payload) {
    if (payload.size() > 255) return;
    std::vector<uint8_t> frame;
    frame.insert(frame.end(), HEADER.begin(), HEADER.end());
    frame.push_back(VERSION);
    frame.push_back(type);
    frame.push_back((uint8_t)payload.size());
    frame.insert(frame.end(), payload.begin(), payload.end());
    frame.push_back(calcChecksum(type, payload.data(), (uint8_t)payload.size()));
    fwrite(frame.data(), 1, frame.size(), stdout);
    fflush(stdout);
}

void sendCurrentState() {
    uint8_t state = STATE_STOPPED;
    if (networkHasIp()) state = STATE_PROVISIONED;
    else if (networkConfigured()) state = STATE_AUTHORIZED;
    sendFrame(CURRENT_STATE, {state});
}

void sendError(uint8_t code) {
    sendFrame(ERROR_STATE, {code});
}

void sendRpcResponse(uint8_t command, const std::vector<std::string>& values) {
    std::vector<uint8_t> payload;
    payload.push_back(command);
    payload.push_back(0);
    size_t dataLen = 0;
    for (const auto& value : values) {
        if (value.size() > 255) return;
        payload.push_back((uint8_t)value.size());
        payload.insert(payload.end(), value.begin(), value.end());
        dataLen += value.size() + 1;
    }
    payload[1] = (uint8_t)dataLen;
    sendFrame(RPC_RESPONSE, payload);
}

void handleRpc(const uint8_t* payload, size_t len) {
    if (!len) {
        sendError(ERROR_INVALID_RPC);
        return;
    }
    uint8_t cmd = payload[0];
    switch (cmd) {
        case CMD_WIFI_SETTINGS: {
            if (len < 3) {
                sendError(ERROR_INVALID_RPC);
                return;
            }
            uint8_t ssidLen = payload[1];
            if (len < 2 + ssidLen + 1) {
                sendError(ERROR_INVALID_RPC);
                return;
            }
            size_t passLenOffset = 2 + ssidLen;
            uint8_t passLen = payload[passLenOffset];
            if (len != passLenOffset + 1 + passLen) {
                sendError(ERROR_INVALID_RPC);
                return;
            }
            std::string ssid((const char*)(payload + 2), ssidLen);
            std::string pass((const char*)(payload + passLenOffset + 1), passLen);
            sendFrame(CURRENT_STATE, {STATE_PROVISIONING});
            if (!networkProvision(ssid.c_str(), pass.c_str(), 20000)) {
                sendError(ERROR_UNABLE_TO_CONNECT);
                sendCurrentState();
                return;
            }
            sendCurrentState();
            sendRpcResponse(CMD_WIFI_SETTINGS, {"http://esp32-minecraft.local"});
            return;
        }
        case CMD_IDENTIFY:
            sendRpcResponse(CMD_IDENTIFY, {});
            return;
        case CMD_GET_CURRENT_STATE:
            sendCurrentState();
            sendRpcResponse(CMD_GET_CURRENT_STATE, {});
            return;
        case CMD_GET_DEVICE_INFO: {
            const esp_app_desc_t* app = esp_app_get_description();
            sendRpcResponse(CMD_GET_DEVICE_INFO,
                {"ESP32 Minecraft server", app ? app->version : "dev", "ESP32-S3"});
            return;
        }
        default:
            sendError(ERROR_UNKNOWN_RPC);
            return;
    }
}

void feedByte(uint8_t b) {
    if (!parser.gotHeader) {
        if (b == HEADER[parser.headerPos]) {
            parser.headerPos++;
            if (parser.headerPos == HEADER.size()) {
                parser.gotHeader = true;
                parser.framePos = 0;
            }
        } else {
            parser.headerPos = (b == HEADER[0]) ? 1 : 0;
        }
        return;
    }
    if (parser.framePos == 0) {
        parser.version = b;
        if (parser.version != VERSION) {
            parser = {};
        }
    } else if (parser.framePos == 1) {
        parser.type = b;
    } else if (parser.framePos == 2) {
        parser.payloadLen = b;
    } else if (parser.framePos < 3 + parser.payloadLen) {
        parser.payload[parser.framePos - 3] = b;
    } else {
        parser.checksum = b;
        uint8_t calc = calcChecksum(parser.type, parser.payload.data(), parser.payloadLen);
        if (calc == parser.checksum && parser.type == RPC)
            handleRpc(parser.payload.data(), parser.payloadLen);
        parser = {};
        return;
    }
    parser.framePos++;
}
}  // namespace

void improvHandleSerialData(const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; i++) feedByte(data[i]);
}
