// OpenCores Ethernet MAC driver for running the firmware in Espressif's QEMU on
// an ESP32-S3 target (`-nic user,model=open_eth`). ESP-IDF ships an equivalent
// driver only for the original ESP32 (CONFIG_ETH_USE_OPENETH), so this file
// adapts it (Apache-2.0, Copyright 2019 Espressif Systems) to the ESP32-S3
// machine model: registers at 0x600CD000, interrupt on matrix source 0.
// It is compiled only into the esp32s3-qemu build (MC_QEMU).
#ifdef MC_QEMU

#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "qemu_eth.h"
#include "soc/soc.h"

static const char* TAG = "qemu_eth";

// ---------------------------------------------------------------- registers (see QEMU hw/net/opencores_eth.c)
#define OE_BASE 0x600CD000u
#define OE_INTR_SOURCE 0  // ETS_WIFI_MAC_INTR_SOURCE on the QEMU ESP32-S3 machine
#define OE_MODER (OE_BASE + 0x00)
#define OE_INT_SOURCE (OE_BASE + 0x04)
#define OE_INT_MASK (OE_BASE + 0x08)
#define OE_PACKETLEN (OE_BASE + 0x18)
#define OE_TX_BD_NUM (OE_BASE + 0x20)
#define OE_MIICOMMAND (OE_BASE + 0x2c)
#define OE_MIIADDRESS (OE_BASE + 0x30)
#define OE_MIITX_DATA (OE_BASE + 0x34)
#define OE_MIIRX_DATA (OE_BASE + 0x38)
#define OE_MAC_ADDR0 (OE_BASE + 0x40)
#define OE_MAC_ADDR1 (OE_BASE + 0x44)
#define OE_DESC_BASE (OE_BASE + 0x400)
#define OE_MODER_DEFAULT 0xa000
#define OE_RST (1u << 11)
#define OE_PAD (1u << 15)
#define OE_CRCEN (1u << 13)
#define OE_FULLD (1u << 10)
#define OE_PRO (1u << 5)
#define OE_TXEN (1u << 1)
#define OE_RXEN (1u << 0)
#define OE_INT_BUSY (1u << 4)
#define OE_INT_RXB (1u << 2)
#define OE_INT_TXB (1u << 0)
#define OE_WCTRLDATA (1u << 2)
#define OE_RSTAT (1u << 1)

#define DMA_BUF_SIZE 1600
#define RX_BUF_COUNT 16
#define TX_BUF_COUNT 4
#define OE_DESC_CNT 128

static inline uint32_t rd(uint32_t a) { return *(volatile uint32_t*)a; }
static inline void wr(uint32_t a, uint32_t v) { *(volatile uint32_t*)a = v; }

// QEMU's descriptor layout: word0 = len << 16 | flags, word1 = buffer pointer.
// The bitfields above follow the IDF driver (little endian): TX flags rd(15) irq(14) wr(13) ... cs(0)
typedef struct {
    uint32_t flags_len;
    uint32_t ptr;
} oe_desc_t;

#define TXD_RD (1u << 15)
#define TXD_IRQ (1u << 14)
#define TXD_WR (1u << 13)
#define TXD_PAD (1u << 12)
#define TXD_CRC (1u << 11)
#define RXD_E (1u << 15)
#define RXD_IRQ (1u << 14)
#define RXD_WR (1u << 13)

static inline volatile oe_desc_t* tx_desc(int i) { return (volatile oe_desc_t*)(OE_DESC_BASE + 8 * i); }
static inline volatile oe_desc_t* rx_desc(int i) { return (volatile oe_desc_t*)(OE_DESC_BASE + 8 * (TX_BUF_COUNT + i)); }

typedef struct {
    esp_eth_mac_t parent;
    esp_eth_mediator_t* eth;
    intr_handle_t intr;
    TaskHandle_t rx_task;
    int cur_rx, cur_tx;
    uint8_t addr[6];
    uint8_t* rx_buf[RX_BUF_COUNT];
    uint8_t* tx_buf[TX_BUF_COUNT];
} oe_mac_t;

static void IRAM_ATTR oe_isr(void* arg) {
    oe_mac_t* m = (oe_mac_t*)arg;
    uint32_t st = rd(OE_INT_SOURCE);
    if (st & OE_INT_RXB) {
        BaseType_t woken = pdFALSE;
        vTaskNotifyGiveFromISR(m->rx_task, &woken);
        if (woken) portYIELD_FROM_ISR();
    }
    wr(OE_INT_SOURCE, st);
}

static esp_err_t oe_receive(esp_eth_mac_t* mac, uint8_t* buf, uint32_t* length) {
    oe_mac_t* m = __containerof(mac, oe_mac_t, parent);
    volatile oe_desc_t* d = rx_desc(m->cur_rx);
    uint32_t fl = d->flags_len;
    if (fl & RXD_E) return ESP_ERR_INVALID_STATE;  // still empty
    uint32_t len = fl >> 16;
    if (*length < len) return ESP_ERR_INVALID_SIZE;
    *length = len;
    memcpy(buf, m->rx_buf[m->cur_rx], len);
    d->flags_len = RXD_E | RXD_IRQ | (m->cur_rx == RX_BUF_COUNT - 1 ? RXD_WR : 0);
    m->cur_rx = (m->cur_rx + 1) % RX_BUF_COUNT;
    return ESP_OK;
}

static void oe_rx_task(void* arg) {
    oe_mac_t* m = (oe_mac_t*)arg;
    for (;;) {
        // interrupt driven, with a short timeout as a polling fallback
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2));
        for (;;) {
            uint32_t len = DMA_BUF_SIZE;
            uint8_t* b = malloc(len);
            if (!b) break;
            if (oe_receive(&m->parent, b, &len) != ESP_OK || !len) {
                free(b);
                break;
            }
            m->eth->stack_input(m->eth, b, len);  // takes ownership of b
        }
    }
}

static esp_err_t oe_set_mediator(esp_eth_mac_t* mac, esp_eth_mediator_t* eth) {
    __containerof(mac, oe_mac_t, parent)->eth = eth;
    return ESP_OK;
}

static esp_err_t oe_write_phy_reg(esp_eth_mac_t* mac, uint32_t addr, uint32_t reg, uint32_t val) {
    wr(OE_MIIADDRESS, (reg << 8) | addr);
    wr(OE_MIITX_DATA, val & 0xffff);
    wr(OE_MIICOMMAND, OE_WCTRLDATA);
    return ESP_OK;
}

static esp_err_t oe_read_phy_reg(esp_eth_mac_t* mac, uint32_t addr, uint32_t reg, uint32_t* val) {
    wr(OE_MIIADDRESS, (reg << 8) | addr);
    wr(OE_MIICOMMAND, OE_RSTAT);
    *val = rd(OE_MIIRX_DATA) & 0xffff;
    return ESP_OK;
}

static esp_err_t oe_set_addr(esp_eth_mac_t* mac, uint8_t* a) {
    oe_mac_t* m = __containerof(mac, oe_mac_t, parent);
    memcpy(m->addr, a, 6);
    wr(OE_MAC_ADDR0, (uint32_t)a[2] << 24 | (uint32_t)a[3] << 16 | (uint32_t)a[4] << 8 | a[5]);
    wr(OE_MAC_ADDR1, (uint32_t)a[0] << 8 | a[1]);
    return ESP_OK;
}

static esp_err_t oe_get_addr(esp_eth_mac_t* mac, uint8_t* a) {
    memcpy(a, __containerof(mac, oe_mac_t, parent)->addr, 6);
    return ESP_OK;
}

static void oe_enable(void) {
    wr(OE_TX_BD_NUM, TX_BUF_COUNT);
    wr(OE_INT_MASK, OE_INT_RXB | OE_INT_BUSY);
    wr(OE_MODER, rd(OE_MODER) | OE_TXEN | OE_RXEN | OE_PAD | OE_CRCEN | OE_FULLD);
}

static void oe_disable(void) {
    wr(OE_INT_MASK, 0);
    wr(OE_MODER, rd(OE_MODER) & ~(OE_TXEN | OE_RXEN));
}

static esp_err_t oe_set_link(esp_eth_mac_t* mac, eth_link_t link) {
    oe_mac_t* m = __containerof(mac, oe_mac_t, parent);
    if (link == ETH_LINK_UP) {
        if (m->intr) esp_intr_enable(m->intr);
        oe_enable();
    } else {
        if (m->intr) esp_intr_disable(m->intr);
        oe_disable();
    }
    return ESP_OK;
}

static esp_err_t oe_ok_speed(esp_eth_mac_t* mac, eth_speed_t s) { return ESP_OK; }
static esp_err_t oe_ok_duplex(esp_eth_mac_t* mac, eth_duplex_t d) { return ESP_OK; }
static esp_err_t oe_ok_bool(esp_eth_mac_t* mac, bool b) { return ESP_OK; }
static esp_err_t oe_ok_u32(esp_eth_mac_t* mac, uint32_t v) { return ESP_OK; }

static esp_err_t oe_set_promiscuous(esp_eth_mac_t* mac, bool en) {
    wr(OE_MODER, en ? (rd(OE_MODER) | OE_PRO) : (rd(OE_MODER) & ~OE_PRO));
    return ESP_OK;
}

static esp_err_t oe_transmit(esp_eth_mac_t* mac, uint8_t* buf, uint32_t length) {
    oe_mac_t* m = __containerof(mac, oe_mac_t, parent);
    if (length > DMA_BUF_SIZE) return ESP_ERR_INVALID_SIZE;
    // QEMU transmits synchronously when the descriptor's ready bit is set
    int i = m->cur_tx;
    memcpy(m->tx_buf[i], buf, length);
    volatile oe_desc_t* d = tx_desc(i);
    d->ptr = (uint32_t)m->tx_buf[i];
    d->flags_len = (length << 16) | TXD_RD | TXD_PAD | TXD_CRC | (i == TX_BUF_COUNT - 1 ? TXD_WR : 0);
    m->cur_tx = (i + 1) % TX_BUF_COUNT;
    return ESP_OK;
}

static esp_err_t oe_init(esp_eth_mac_t* mac) {
    oe_mac_t* m = __containerof(mac, oe_mac_t, parent);
    if (rd(OE_MODER) != OE_MODER_DEFAULT) {
        ESP_LOGE(TAG, "no OpenCores MAC found: run QEMU with -nic user,model=open_eth");
        return ESP_FAIL;
    }
    m->eth->on_state_changed(m->eth, ETH_STATE_LLINIT, NULL);
    wr(OE_MODER, rd(OE_MODER) | OE_RST);
    wr(OE_MODER, rd(OE_MODER) & ~OE_RST);
    wr(OE_TX_BD_NUM, TX_BUF_COUNT);
    for (int i = 0; i < TX_BUF_COUNT; i++) {
        tx_desc(i)->ptr = (uint32_t)m->tx_buf[i];
        tx_desc(i)->flags_len = i == TX_BUF_COUNT - 1 ? TXD_WR : 0;
    }
    for (int i = 0; i < RX_BUF_COUNT; i++) {
        rx_desc(i)->ptr = (uint32_t)m->rx_buf[i];
        rx_desc(i)->flags_len = RXD_E | RXD_IRQ | (i == RX_BUF_COUNT - 1 ? RXD_WR : 0);
    }
    m->cur_rx = m->cur_tx = 0;
    oe_set_addr(mac, m->addr);
    return ESP_OK;
}

static esp_err_t oe_deinit(esp_eth_mac_t* mac) {
    oe_mac_t* m = __containerof(mac, oe_mac_t, parent);
    m->eth->on_state_changed(m->eth, ETH_STATE_DEINIT, NULL);
    return ESP_OK;
}

static esp_err_t oe_start(esp_eth_mac_t* mac) { oe_enable(); return ESP_OK; }
static esp_err_t oe_stop(esp_eth_mac_t* mac) { oe_disable(); return ESP_OK; }
static esp_err_t oe_del(esp_eth_mac_t* mac) { return ESP_OK; }  // lives for the whole run

static esp_eth_mac_t* oe_new(const eth_mac_config_t* cfg) {
    oe_mac_t* m = calloc(1, sizeof(oe_mac_t));
    if (!m) return NULL;
    for (int i = 0; i < RX_BUF_COUNT; i++) m->rx_buf[i] = heap_caps_calloc(1, DMA_BUF_SIZE, MALLOC_CAP_DMA);
    for (int i = 0; i < TX_BUF_COUNT; i++) m->tx_buf[i] = heap_caps_calloc(1, DMA_BUF_SIZE, MALLOC_CAP_DMA);
    static const uint8_t qemuMac[6] = {0x02, 0xE5, 0x32, 0x00, 0x00, 0x01};  // locally administered
    memcpy(m->addr, qemuMac, 6);
    m->parent.set_mediator = oe_set_mediator;
    m->parent.init = oe_init;
    m->parent.deinit = oe_deinit;
    m->parent.start = oe_start;
    m->parent.stop = oe_stop;
    m->parent.del = oe_del;
    m->parent.write_phy_reg = oe_write_phy_reg;
    m->parent.read_phy_reg = oe_read_phy_reg;
    m->parent.set_addr = oe_set_addr;
    m->parent.get_addr = oe_get_addr;
    m->parent.set_speed = oe_ok_speed;
    m->parent.set_duplex = oe_ok_duplex;
    m->parent.set_link = oe_set_link;
    m->parent.set_promiscuous = oe_set_promiscuous;
    m->parent.enable_flow_ctrl = oe_ok_bool;
    m->parent.set_peer_pause_ability = oe_ok_u32;
    m->parent.transmit = oe_transmit;
    m->parent.receive = oe_receive;
    if (esp_intr_alloc(OE_INTR_SOURCE, 0, oe_isr, m, &m->intr) != ESP_OK) {
        ESP_LOGW(TAG, "no interrupt, polling the MAC");
        m->intr = NULL;
    }
    xTaskCreatePinnedToCore(oe_rx_task, "oe_rx", cfg->rx_task_stack_size, m, cfg->rx_task_prio, &m->rx_task, 0);
    return &m->parent;
}

// ---------------------------------------------------------------- a PHY that is always up
typedef struct {
    esp_eth_phy_t parent;
    esp_eth_mediator_t* eth;
    uint32_t addr;
    bool reported;
} qphy_t;

static esp_err_t qphy_set_mediator(esp_eth_phy_t* phy, esp_eth_mediator_t* eth) {
    __containerof(phy, qphy_t, parent)->eth = eth;
    return ESP_OK;
}
static esp_err_t qphy_ok(esp_eth_phy_t* phy) { return ESP_OK; }
static esp_err_t qphy_ok_bool(esp_eth_phy_t* phy, bool b) { return ESP_OK; }
static esp_err_t qphy_get_link(esp_eth_phy_t* phy) {
    qphy_t* p = __containerof(phy, qphy_t, parent);
    if (!p->reported) {
        p->reported = true;
        eth_speed_t speed = ETH_SPEED_100M;
        eth_duplex_t duplex = ETH_DUPLEX_FULL;
        p->eth->on_state_changed(p->eth, ETH_STATE_SPEED, (void*)speed);
        p->eth->on_state_changed(p->eth, ETH_STATE_DUPLEX, (void*)duplex);
        p->eth->on_state_changed(p->eth, ETH_STATE_LINK, (void*)ETH_LINK_UP);
    }
    return ESP_OK;
}
static esp_err_t qphy_set_addr(esp_eth_phy_t* phy, uint32_t a) { __containerof(phy, qphy_t, parent)->addr = a; return ESP_OK; }
static esp_err_t qphy_get_addr(esp_eth_phy_t* phy, uint32_t* a) { *a = __containerof(phy, qphy_t, parent)->addr; return ESP_OK; }
static esp_err_t qphy_pause(esp_eth_phy_t* phy, uint32_t ability) { return ESP_OK; }
static esp_err_t qphy_del(esp_eth_phy_t* phy) { return ESP_OK; }

static esp_eth_phy_t* qphy_new(void) {
    qphy_t* p = calloc(1, sizeof(qphy_t));
    if (!p) return NULL;
    p->parent.set_mediator = qphy_set_mediator;
    p->parent.reset = qphy_ok;
    p->parent.reset_hw = qphy_ok;
    p->parent.init = qphy_ok;
    p->parent.deinit = qphy_ok;
    p->parent.negotiate = qphy_ok;
    p->parent.get_link = qphy_get_link;
    p->parent.pwrctl = qphy_ok_bool;
    p->parent.set_addr = qphy_set_addr;
    p->parent.get_addr = qphy_get_addr;
    p->parent.advertise_pause_ability = qphy_pause;
    p->parent.loopback = qphy_ok_bool;
    p->parent.del = qphy_del;
    return &p->parent;
}

// ---------------------------------------------------------------- bring-up
static EventGroupHandle_t s_events;
static char s_ip[16];

static void on_got_ip(void* arg, esp_event_base_t base, int32_t id, void* data) {
    ip_event_got_ip_t* ev = (ip_event_got_ip_t*)data;
    esp_ip4addr_ntoa(&ev->ip_info.ip, s_ip, sizeof(s_ip));
    xEventGroupSetBits(s_events, 1);
}

int qemu_eth_start(char* ipOut, int ipCap) {
    s_events = xEventGroupCreate();
    esp_netif_init();
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return -1;
    esp_netif_config_t ncfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t* netif = esp_netif_new(&ncfg);
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    mac_cfg.rx_task_stack_size = 4096;
    esp_eth_mac_t* mac = oe_new(&mac_cfg);
    esp_eth_phy_t* phy = qphy_new();
    esp_eth_config_t cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t h = NULL;
    if (esp_eth_driver_install(&cfg, &h) != ESP_OK) return -1;
    esp_netif_attach(netif, esp_eth_new_netif_glue(h));
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, NULL);
    if (esp_eth_start(h) != ESP_OK) return -1;
    EventBits_t bits = xEventGroupWaitBits(s_events, 1, pdFALSE, pdTRUE, pdMS_TO_TICKS(20000));
    if (!(bits & 1)) return -1;
    if (ipOut) snprintf(ipOut, ipCap, "%s", s_ip);
    return 0;
}

#endif  // MC_QEMU
