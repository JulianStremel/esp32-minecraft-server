// The world on a microSD card.
//
// The card keeps a normal FAT32 filesystem (copy the world to a PC, keep other files on
// it); the world is one file, preallocated contiguous (f_expand) so that writing to it
// never touches the FAT or the directory again. It is read and written through FatFs
// directly (32-bit file offsets: up to 4 GB; the VFS layer would stop at 2 GB), through
// a small DMA-capable bounce buffer in internal RAM so the SD driver can move several
// sectors per command, and behind mc::WriteBackCache (lines in PSRAM) so the card sees
// few large writes instead of many small ones.
//
// exFAT is not available: ESP-IDF builds FatFs without it (FF_FS_EXFAT 0, no option),
// so cards must be FAT32 (cards over 32 GB come formatted exFAT and need reformatting).
#include "sd_storage.h"
#include <stdio.h>
#include <string.h>
#include "firmware_config.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "diskio_sdmmc.h"   // after ff.h (it uses its types)
#include "mc/platform.h"
#include "mc/storage/write_cache.h"
#include "sdmmc_cmd.h"

// ------------------------------------------------------------------ settings (config.h)
#ifndef SD_MODE_SDMMC
#define SD_MODE_SDMMC 0
#define SD_MODE_SPI 1
#endif
#ifndef SD_MODE
#define SD_MODE SD_MODE_SDMMC
#endif
// Waveshare ESP32-S3-Touch-AMOLED-1.8 (its BSP: 1-bit SDMMC; the card's D3/CS line is on
// the TCA9554 I/O expander, EXIO7, and is left alone, as the BSP does)
#ifndef SD_PIN_CLK
#define SD_PIN_CLK 2
#endif
#ifndef SD_PIN_CMD
#define SD_PIN_CMD 1   // SPI mode: MOSI
#endif
#ifndef SD_PIN_D0
#define SD_PIN_D0 3    // SPI mode: MISO
#endif
#ifndef SD_PIN_D1
#define SD_PIN_D1 -1   // D1..D3 set: 4-bit SDMMC
#endif
#ifndef SD_PIN_D2
#define SD_PIN_D2 -1
#endif
#ifndef SD_PIN_D3
#define SD_PIN_D3 -1
#endif
#ifndef SD_PIN_CS
#define SD_PIN_CS -1   // SPI mode: chip select (-1: held active by the board)
#endif
#ifndef SD_FREQ_KHZ
#define SD_FREQ_KHZ 20000   // SDMMC_FREQ_DEFAULT; 40000 for high speed
#endif
#ifndef SD_WORLD_FILE
#define SD_WORLD_FILE "world.img"
#endif
#ifndef SD_WORLD_SIZE_MB
#define SD_WORLD_SIZE_MB 0   // 0: as large as fits (up to 4095 MB)
#endif
#ifndef SD_FORMAT_IF_NEEDED
#define SD_FORMAT_IF_NEEDED 0   // 1: format a card that does not mount (erases it)
#endif
#ifndef SD_CACHE_LINE_KB
#define SD_CACHE_LINE_KB 16
#endif
#ifndef SD_CACHE_LINES
#define SD_CACHE_LINES 32
#endif

namespace {

const char* const MOUNT = "/sd";
const uint32_t BOUNCE = 4096;   // DMA-capable bytes in internal RAM

// The world file through FatFs, the data bounced through internal DMA memory.
class SdFileDevice : public mc::BlockDevice {
public:
    SdFileDevice(FIL* f, uint64_t size, uint8_t* bounce, const char* desc) : f_(f), size_(size), bounce_(bounce) {
        snprintf(desc_, sizeof(desc_), "%s", desc);
    }
    uint64_t size() override { return size_; }
    const char* describe() override { return desc_; }

    bool read(uint64_t off, void* buf, uint32_t len) override {
        if (off + len > size_) return false;
        uint32_t t0 = mc::plat::millis();
        if (f_lseek(f_, (FSIZE_t)off) != FR_OK) return fail();
        uint8_t* out = (uint8_t*)buf;
        while (len) {
            uint32_t n = len < BOUNCE ? len : BOUNCE;
            UINT got = 0;
            if (f_read(f_, bounce_, n, &got) != FR_OK || got != n) return fail();
            memcpy(out, bounce_, n);
            out += n;
            len -= n;
            stats_.bytesRead += n;
        }
        stats_.reads++;
        stats_.lastLatencyMs = mc::plat::millis() - t0;
        return true;
    }

    bool write(uint64_t off, const void* buf, uint32_t len) override {
        if (off + len > size_) return false;
        uint32_t t0 = mc::plat::millis();
        if (f_lseek(f_, (FSIZE_t)off) != FR_OK) return fail();
        const uint8_t* in = (const uint8_t*)buf;
        while (len) {
            uint32_t n = len < BOUNCE ? len : BOUNCE;
            memcpy(bounce_, in, n);
            UINT put = 0;
            if (f_write(f_, bounce_, n, &put) != FR_OK || put != n) return fail();
            in += n;
            len -= n;
            stats_.bytesWritten += n;
        }
        stats_.writes++;
        stats_.lastLatencyMs = mc::plat::millis() - t0;
        return true;
    }

    bool flush() override {
        stats_.flushes++;
        return f_sync(f_) == FR_OK || fail();
    }

private:
    bool fail() {
        stats_.errors++;
        return false;
    }
    FIL* f_;
    uint64_t size_;
    uint8_t* bounce_;
    char desc_[96];
};

}  // namespace

mc::BlockDevice* sdStorageOpen(bool& created) {
    created = false;
    sdmmc_card_t* card = nullptr;
    esp_vfs_fat_sdmmc_mount_config_t mount = {};
    mount.format_if_mount_failed = SD_FORMAT_IF_NEEDED;
    mount.max_files = 2;
    mount.allocation_unit_size = 16 * 1024;
    esp_err_t err;
#if SD_MODE == SD_MODE_SDMMC
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SD_FREQ_KHZ;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = (gpio_num_t)SD_PIN_CLK;
    slot.cmd = (gpio_num_t)SD_PIN_CMD;
    slot.d0 = (gpio_num_t)SD_PIN_D0;
    slot.d1 = (gpio_num_t)SD_PIN_D1;
    slot.d2 = (gpio_num_t)SD_PIN_D2;
    slot.d3 = (gpio_num_t)SD_PIN_D3;
    slot.width = (SD_PIN_D1 >= 0 && SD_PIN_D2 >= 0 && SD_PIN_D3 >= 0) ? 4 : 1;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;   // in addition to the board's resistors
    err = esp_vfs_fat_sdmmc_mount(MOUNT, &host, &slot, &mount, &card);
    const char* bus = slot.width == 4 ? "SDMMC 4-bit" : "SDMMC 1-bit";
#else
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = SD_FREQ_KHZ;
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = SD_PIN_CMD;
    buscfg.miso_io_num = SD_PIN_D0;
    buscfg.sclk_io_num = SD_PIN_CLK;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = BOUNCE;
    err = spi_bus_initialize((spi_host_device_t)host.slot, &buscfg, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK) {
        printf("SD card: SPI bus init failed (%s)\n", esp_err_to_name(err));
        return nullptr;
    }
    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.host_id = (spi_host_device_t)host.slot;
    dev.gpio_cs = SD_PIN_CS >= 0 ? (gpio_num_t)SD_PIN_CS : GPIO_NUM_NC;
    err = esp_vfs_fat_sdspi_mount(MOUNT, &host, &dev, &mount, &card);
    const char* bus = "SPI";
#endif
    if (err != ESP_OK) {
        printf("SD card: not mounted (%s)%s\n", esp_err_to_name(err),
               err == ESP_FAIL ? ": no FAT32 filesystem (exFAT cards need reformatting as FAT32)" : "");
        return nullptr;
    }
    printf("SD card: %s, %llu MB, %s, %u kHz\n", card->cid.name,
           (unsigned long long)card->csd.capacity * card->csd.sector_size / (1024 * 1024), bus,
           (unsigned)card->max_freq_khz);

    // the world file, on the card's FatFs drive
    char path[48];
    snprintf(path, sizeof(path), "%d:/%s", (int)ff_diskio_get_pdrv_card(card), SD_WORLD_FILE);
    FILINFO info;
    uint64_t size = 0;
    if (f_stat(path, &info) == FR_OK) {
        size = info.fsize;
    } else {
        FATFS* fs = nullptr;
        DWORD freeClusters = 0;
        char drive[8];
        snprintf(drive, sizeof(drive), "%d:", (int)ff_diskio_get_pdrv_card(card));
        if (f_getfree(drive, &freeClusters, &fs) != FR_OK) {
            printf("SD card: cannot read the free space\n");
            return nullptr;
        }
        uint64_t freeBytes = (uint64_t)freeClusters * fs->csize * 512;   // FF_MAX_SS 512 on SD cards
        uint64_t want = SD_WORLD_SIZE_MB ? (uint64_t)SD_WORLD_SIZE_MB << 20 : (freeBytes / 10 * 9);
        if (want > 4095ull << 20) want = 4095ull << 20;   // FAT32's file limit
        if (want > freeBytes) want = freeBytes;
        want &= ~(uint64_t)((1 << 20) - 1);
        if (want < (64ull << 20)) {
            printf("SD card: only %llu MB free, too little for a world\n", (unsigned long long)(freeBytes >> 20));
            return nullptr;
        }
        char vpath[48];
        snprintf(vpath, sizeof(vpath), "%s/%s", MOUNT, SD_WORLD_FILE);
        printf("SD card: creating %s, %llu MB, contiguous ...\n", vpath, (unsigned long long)(want >> 20));
        err = esp_vfs_fat_create_contiguous_file(MOUNT, vpath, want, true);
        if (err != ESP_OK) {
            printf("SD card: cannot create the world file (%s)\n", esp_err_to_name(err));
            return nullptr;
        }
        size = want;
        created = true;
    }
    bool contiguous = false;
    {
        char vpath[48];
        snprintf(vpath, sizeof(vpath), "%s/%s", MOUNT, SD_WORLD_FILE);
        esp_vfs_fat_test_contiguous_file(MOUNT, vpath, &contiguous);
    }
    static FIL file;   // static: FatFs keeps a pointer to it while it is open
    if (f_open(&file, path, FA_READ | FA_WRITE) != FR_OK) {
        printf("SD card: cannot open %s\n", path);
        return nullptr;
    }
    uint8_t* bounce = (uint8_t*)heap_caps_malloc(BOUNCE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!bounce) {
        printf("SD card: no internal DMA memory for the bounce buffer\n");
        return nullptr;
    }
    char desc[96];
    snprintf(desc, sizeof(desc), "SD card %s %s (%llu MB%s)", bus, SD_WORLD_FILE, (unsigned long long)(size >> 20),
             contiguous ? ", contiguous" : ", FRAGMENTED");
    SdFileDevice* dev = new SdFileDevice(&file, size, bounce, desc);
    if (created) {
        // the file's old sectors hold whatever was on the card: the store's header area
        // (superblocks, player table, region directory) must start out zero
        uint64_t zero = (2ull << 20) + size / 256 + (64 << 10);
        if (zero > (18ull << 20)) zero = 18ull << 20;
        if (zero > size) zero = size;
        uint8_t* z = (uint8_t*)mc::plat::bigAlloc(64 * 1024);
        if (!z) return nullptr;
        memset(z, 0, 64 * 1024);
        for (uint64_t off = 0; off < zero; off += 64 * 1024) {
            uint32_t n = zero - off < 64 * 1024 ? (uint32_t)(zero - off) : 64 * 1024;
            if (!dev->write(off, z, n)) {
                printf("SD card: cannot write the world file\n");
                mc::plat::bigFree(z);
                return nullptr;
            }
        }
        mc::plat::bigFree(z);
        dev->flush();
        printf("SD card: the world file is ready (%llu MB zeroed)\n", (unsigned long long)(zero >> 20));
    }
    if (!contiguous) printf("SD card: warning: %s is fragmented (slower; the FAT changes on writes)\n", SD_WORLD_FILE);
    mc::WriteBackCache* cache = new mc::WriteBackCache(dev, SD_CACHE_LINE_KB * 1024, SD_CACHE_LINES);
    if (!cache->ok()) {
        printf("SD card: no PSRAM for the write cache: writing directly\n");
        delete cache;
        return dev;
    }
    return cache;
}
