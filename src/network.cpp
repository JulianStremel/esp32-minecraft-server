#include "network.h"
#include "firmware_config.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <cstdio>
#include <cstring>
#if CONFIG_IDF_TARGET_ESP32S3 && !defined(MC_QEMU_CAPTURE)
#include "esp_wifi.h"
#else
#include "esp_eth.h"
#endif

namespace {
EventGroupHandle_t events;
constexpr EventBits_t GOT_IP = BIT0;
constexpr EventBits_t WIFI_CONFIGURED = BIT1;
constexpr EventBits_t WIFI_READY = BIT2;
#if CONFIG_IDF_TARGET_ESP32S3 && !defined(MC_QEMU_CAPTURE)
constexpr char WIFI_NAMESPACE[] = "wifi";
constexpr uint32_t WIFI_MAX_TIMEOUT_MS = 60000;

bool copyCreds(wifi_config_t& config, const char* ssid, const char* password) {
    if (!ssid || !password) return false;
    size_t ssidLen = strlen(ssid);
    size_t passLen = strlen(password);
    if (!ssidLen || ssidLen >= sizeof(config.sta.ssid) || passLen >= sizeof(config.sta.password))
        return false;
    memset(&config, 0, sizeof(config));
    memcpy(config.sta.ssid, ssid, ssidLen);
    memcpy(config.sta.password, password, passLen);
    return true;
}

bool loadProvisionedCreds(char* ssid, size_t ssidCap, char* password, size_t passCap) {
    nvs_handle_t nvs = 0;
    if (nvs_open(WIFI_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false;
    size_t ssidLen = ssidCap;
    size_t passLen = passCap;
    esp_err_t ssidErr = nvs_get_str(nvs, "ssid", ssid, &ssidLen);
    esp_err_t passErr = nvs_get_str(nvs, "password", password, &passLen);
    nvs_close(nvs);
    return ssidErr == ESP_OK && passErr == ESP_OK && ssid[0];
}

bool storeProvisionedCreds(const char* ssid, const char* password) {
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return false;
    err = nvs_set_str(nvs, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(nvs, "password", password);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err == ESP_OK;
}
#endif

void onIp(void*, esp_event_base_t, int32_t, void* data) {
    auto* event = static_cast<ip_event_got_ip_t*>(data);
    printf("IP address: " IPSTR "\n", IP2STR(&event->ip_info.ip));
    xEventGroupSetBits(events, GOT_IP);
}

#if CONFIG_IDF_TARGET_ESP32S3 && !defined(MC_QEMU_CAPTURE)
void onWifi(void*, esp_event_base_t, int32_t id, void* data) {
    if (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED) {
        if (id == WIFI_EVENT_STA_DISCONNECTED) {
            xEventGroupClearBits(events, GOT_IP);
            auto* event = static_cast<wifi_event_sta_disconnected_t*>(data);
            printf("WiFi lost (reason %d), reconnecting\n", event->reason);
        }
        if (!(xEventGroupGetBits(events) & WIFI_CONFIGURED)) return;
        ESP_ERROR_CHECK(esp_wifi_connect());
    }
}
#endif
}  // namespace

void networkStart() {
    events = xEventGroupCreate();
    ESP_ERROR_CHECK(events ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
#if CONFIG_IDF_TARGET_ESP32S3 && !defined(MC_QEMU_CAPTURE)
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    esp_netif_t* netif = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(netif ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(esp_netif_set_hostname(netif, MC_HOSTNAME));
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, onWifi, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, onIp, nullptr));
    wifi_config_t config = {};
    static_assert(sizeof(WIFI_SSID) - 1 <= sizeof(config.sta.ssid), "WiFi SSID too long");
    static_assert(sizeof(WIFI_PASSWORD) - 1 <= sizeof(config.sta.password), "WiFi password too long");
    char ssid[sizeof(config.sta.ssid)] = {};
    char password[sizeof(config.sta.password)] = {};
    if (!loadProvisionedCreds(ssid, sizeof(ssid), password, sizeof(password))) {
        memcpy(ssid, WIFI_SSID, sizeof(WIFI_SSID) - 1);
        memcpy(password, WIFI_PASSWORD, sizeof(WIFI_PASSWORD) - 1);
    }
    if (ssid[0]) {
        memcpy(config.sta.ssid, ssid, sizeof(config.sta.ssid));
        memcpy(config.sta.password, password, sizeof(config.sta.password));
        xEventGroupSetBits(events, WIFI_CONFIGURED);
    }
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    xEventGroupSetBits(events, WIFI_READY);
#else
    eth_mac_config_t macConfig = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phyConfig = ETH_PHY_DEFAULT_CONFIG();
#ifdef MC_QEMU_CAPTURE
    // IDF's OpenCores driver supports S3. Used only by the GIF capture build.
    phyConfig.phy_addr = 1; // QEMU OpenCores MAC's DP83848
    phyConfig.reset_gpio_num = -1;
    esp_eth_mac_t* mac = esp_eth_mac_new_openeth(&macConfig);
    esp_eth_phy_t* phy = esp_eth_phy_new_dp83848(&phyConfig);
#else
    // P4 has no integrated WiFi. The hardware profile uses configurable RMII/LAN8720.
    eth_esp32_emac_config_t emac = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    phyConfig.phy_addr = CONFIG_MC_ETH_PHY_ADDR;
    phyConfig.reset_gpio_num = CONFIG_MC_ETH_PHY_RESET;
    emac.smi_gpio.mdc_num = CONFIG_MC_ETH_MDC;
    emac.smi_gpio.mdio_num = CONFIG_MC_ETH_MDIO;
    emac.clock_config.rmii.clock_gpio = CONFIG_MC_ETH_CLK_GPIO;
#ifdef CONFIG_MC_ETH_CLK_OUT
    emac.clock_config.rmii.clock_mode = EMAC_CLK_OUT;
#else
    emac.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
#endif
    emac.emac_dataif_gpio.rmii = {CONFIG_MC_ETH_TX_EN, CONFIG_MC_ETH_TXD0,
        CONFIG_MC_ETH_TXD1, CONFIG_MC_ETH_CRS_DV, CONFIG_MC_ETH_RXD0, CONFIG_MC_ETH_RXD1};
    esp_eth_mac_t* mac = esp_eth_mac_new_esp32(&emac, &macConfig);
#ifdef MC_EMULATOR
    // The emulator supplies a generic MDIO PHY, not a physical LAN8720.
    phyConfig.reset_gpio_num = -1;
    esp_eth_phy_t* phy = esp_eth_phy_new_generic(&phyConfig);
#else
    esp_eth_phy_t* phy = esp_eth_phy_new_lan87xx(&phyConfig);
#endif
#endif
    ESP_ERROR_CHECK(mac && phy ? ESP_OK : ESP_ERR_NO_MEM);
    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth = nullptr;
    ESP_ERROR_CHECK(esp_eth_driver_install(&config, &eth));
    esp_netif_config_t netConfig = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t* netif = esp_netif_new(&netConfig);
    ESP_ERROR_CHECK(netif ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(esp_netif_set_hostname(netif, MC_HOSTNAME));
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, onIp, nullptr));
    ESP_ERROR_CHECK(esp_eth_start(eth));
#endif
    printf("Waiting for network DHCP lease\n");
    xEventGroupWaitBits(events, GOT_IP, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(MC_HOSTNAME));
    ESP_ERROR_CHECK(mdns_service_add(nullptr, "_minecraft", "_tcp", MC_PORT, nullptr, 0));
#if MC_DASHBOARD
    if (MC_DASHBOARD_PORT) mdns_service_add(nullptr, "_http", "_tcp", MC_DASHBOARD_PORT, nullptr, 0);
#endif
}

bool networkHasIp() {
    return events && (xEventGroupGetBits(events) & GOT_IP);
}

bool networkConfigured() {
#if CONFIG_IDF_TARGET_ESP32S3 && !defined(MC_QEMU_CAPTURE)
    return events && (xEventGroupGetBits(events) & WIFI_CONFIGURED);
#else
    return true;
#endif
}

bool networkProvision(const char* ssid, const char* password, uint32_t timeoutMs) {
#if CONFIG_IDF_TARGET_ESP32S3 && !defined(MC_QEMU_CAPTURE)
    if (!events || !ssid || !password) return false;
    wifi_config_t config = {};
    if (!copyCreds(config, ssid, password)) return false;
    if (!storeProvisionedCreds(ssid, password)) return false;
    if (!(xEventGroupWaitBits(events, WIFI_READY, pdFALSE, pdTRUE, pdMS_TO_TICKS(3000)) & WIFI_READY))
        return false;
    xEventGroupSetBits(events, WIFI_CONFIGURED);
    xEventGroupClearBits(events, GOT_IP);
    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT && err != ESP_ERR_WIFI_NOT_STARTED)
        return false;
    if (esp_wifi_set_config(WIFI_IF_STA, &config) != ESP_OK) return false;
    if (esp_wifi_connect() != ESP_OK) return false;
    uint32_t capped = timeoutMs > WIFI_MAX_TIMEOUT_MS ? WIFI_MAX_TIMEOUT_MS : timeoutMs;
    EventBits_t bits = xEventGroupWaitBits(events, GOT_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(capped));
    return bits & GOT_IP;
#else
    (void)ssid;
    (void)password;
    (void)timeoutMs;
    return false;
#endif
}
