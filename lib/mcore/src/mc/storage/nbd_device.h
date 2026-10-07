// Network Block Device client (https://github.com/NetworkBlockDevice/nbd/blob/master/doc/proto.md).
// Works with nbd-server, nbdkit, qemu-nbd and tools/nbd_server.py.
//  * fixed newstyle handshake, NBD_OPT_GO with fallback to NBD_OPT_EXPORT_NAME
//  * simple replies; reads are pipelined (readMany), writes are posted and their
//    replies collected lazily, so a chunk save costs no round trip
//  * reconnects automatically after network errors
#pragma once
#include <stdint.h>
#include "mc/platform.h"
#include "mc/storage/block_device.h"

namespace mc {

class NbdDevice : public BlockDevice {
public:
    NbdDevice(const char* host, uint16_t port, const char* exportName);
    ~NbdDevice() override;

    bool connect();
    void disconnect();
    bool available() override;
    uint64_t size() override { return size_; }
    bool readOnly() const { return (flags_ & 2) != 0; }

    bool read(uint64_t off, void* buf, uint32_t len) override;
    bool readMany(ReadOp* ops, int n) override;
    bool write(uint64_t off, const void* buf, uint32_t len) override;
    bool beginWrite(uint64_t off, uint32_t len) override;
    bool writeData(const void* buf, uint32_t len) override;
    bool endWrite() override;
    bool flush() override;
    bool flushLater() override;
    const char* describe() override { return desc_; }

private:
    bool handshake();
    bool sendRequest(uint16_t type, uint64_t handle, uint64_t off, uint32_t len);
    bool readReply(uint64_t& handle, uint32_t& error);
    bool drainWrites();
    bool fail(const char* what);
    bool sendAll(const void* d, size_t n);
    bool recvAll(void* d, size_t n);

    char host_[64];
    uint16_t port_;
    char export_[64];
    char desc_[160];
    Conn* conn_ = nullptr;
    uint64_t size_ = 0;
    uint16_t flags_ = 0;
    uint64_t nextHandle_ = 1;
    int pendingWrites_ = 0;
    uint32_t streamLeft_ = 0;
    uint32_t lastAttemptMs_ = 0;
    uint32_t backoffMs_ = 0;
    uint8_t wbuf_[1460];    // coalesces small sends into full TCP segments
    size_t wlen_ = 0;
    bool flushSend();
};

}  // namespace mc
