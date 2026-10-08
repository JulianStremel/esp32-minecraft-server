#include "mc/server/books.h"
#include "mc/server/server.h"
#include "mc/nbt.h"
#include "mc/bytebuf.h"
#include "mc/registry.h"
#include <string.h>

namespace mc {
namespace Books {
bool isBook(const ItemStack& item) {
    return !item.empty() && (item.id == itm::WritableBook || item.id == itm::WrittenBook);
}
int pages(const ItemStack& item) {
    if (!item.tagSize()) return 0;
    int count = 0;
    Reader r(item.tagData(), item.tagSize());
    nbtVisitRoot(
        r,
        [](void* ctx, uint8_t type, const char* name, Reader& value) {
            if (type != NBT_LIST || strcmp(name, "pages")) return false;
            if (value.u8() == NBT_STRING) *(int*)ctx = value.i32();
            return false; // visit uses a copy, so the payload is still skipped normally
        },
        &count);
    return count;
}
int comparator(const TileEntity& tile) {
    int count = pages(tile.items[0]);
    float fraction = count > 1 ? (float)tile.bookPage / (float)(count - 1) : 1.0f;
    return (int)(fraction * 14.0f) + (isBook(tile.items[0]) ? 1 : 0);
}
void properties(Player& p, const TileEntity& tile) {
    Packet packet(pkt::s2c::CraftProgressBar);
    packet.w.varint(p.winId);
    packet.w.i16(0);
    packet.w.i16(tile.bookPage);
    p.conn.send(packet);
}
bool place(Server& s, Player& p, int x, int y, int z, ItemStack& item) {
    uint16_t state = s.blockAt(x, y, z);
    if (blockIdOf(state) != blk::Lectern || getBool(state, "has_book") || !isBook(item)) return false;
    Chunk* c = s.world.get(s.curDim, x >> 4, z >> 4);
    TileEntity* t = c ? c->tileAt(x & 15, y, z & 15) : nullptr;
    if (!t || t->type != TILE_LECTERN) return false;
    t->items[0] = item;
    t->items[0].count = 1;
    t->bookPage = 0;
    if (p.gamemode != GM_CREATIVE && --item.count == 0) item.clear();
    c->dirty = true;
    s.setBlock(x, y, z, setBool(setBool(state, "has_book", true), "powered", false));
    s.redstone.neighbours(s, x, y - 1, z);
    s.playSound("item.book.put", x + .5, y + .5, z + .5, 1, 1, 4);
    return true;
}
void removedBook(Server& s, int x, int y, int z) {
    uint16_t state = s.blockAt(x, y, z);
    if (blockIdOf(state) != blk::Lectern) return;
    Chunk* c = s.world.get(s.curDim, x >> 4, z >> 4);
    TileEntity* t = c ? c->tileAt(x & 15, y, z & 15) : nullptr;
    if (!t || t->type != TILE_LECTERN || !t->items[0].empty()) return;
    t->bookPage = 0;
    c->dirty = true;
    s.setBlock(x, y, z, setBool(setBool(state, "has_book", false), "powered", false));
    s.redstone.neighbours(s, x, y - 1, z);
    s.containerChanged(x, y, z);
}
void turnPage(Server& s, int x, int y, int z, int page) {
    uint16_t state = s.blockAt(x, y, z);
    if (blockIdOf(state) != blk::Lectern || !getBool(state, "has_book")) return;
    Chunk* c = s.world.get(s.curDim, x >> 4, z >> 4);
    TileEntity* t = c ? c->tileAt(x & 15, y, z & 15) : nullptr;
    if (!t || t->type != TILE_LECTERN || !isBook(t->items[0])) return;
    int last = pages(t->items[0]) - 1;
    page = page < 0 ? 0 : page > last ? last : page; // Mth.clamp also when last == -1
    if (page == t->bookPage) return;
    t->bookPage = page;
    c->dirty = true;
    s.setBlock(x, y, z, setBool(state, "powered", true));
    s.redstone.neighbours(s, x, y - 1, z);
    s.scheduleTick(x, y, z, 2);
    Packet packet(pkt::s2c::WorldEvent);
    packet.w.i32(1043);
    packet.w.u64(packPos(x, y, z));
    packet.w.i32(0);
    packet.w.boolean(false);
    s.broadcastNear(packet, x >> 4, z >> 4);
    s.containerChanged(x, y, z);
}
} // namespace Books

void Player::onWindowButton(Reader& r) {
    int window = r.varint(), button = r.varint();
    if (!r.ok() || dead || gamemode == GM_SPECTATOR || winKind != WK_LECTERN || window != winId) return;
    Server& s = *srv;
    double dx = e.x - (winX + .5), dy = e.y - (winY + .5), dz = e.z - (winZ + .5);
    if (dx * dx + dy * dy + dz * dz > 64) {
        s.closeWindow(*this, true);
        return;
    }
    uint16_t state = s.blockAt(winX, winY, winZ);
    Chunk* c = s.world.get(s.curDim, winX >> 4, winZ >> 4);
    TileEntity* t = c ? c->tileAt(winX & 15, winY, winZ & 15) : nullptr;
    if (blockIdOf(state) != blk::Lectern || !getBool(state, "has_book") || !t || t->type != TILE_LECTERN) return;
    if (button >= 100)
        Books::turnPage(s, winX, winY, winZ, button - 100);
    else if (button == 1 || button == 2)
        Books::turnPage(s, winX, winY, winZ, t->bookPage + (button == 1 ? -1 : 1));
    else if (button == 3 && gamemode != GM_ADVENTURE) {
        ItemStack book = t->items[0];
        t->items[0].clear();
        Books::removedBook(s, winX, winY, winZ);
        int left = s.giveItem(*this, book);
        if (left) {
            book.count = left;
            s.throwItem(*this, book);
        }
    }
}
} // namespace mc

namespace mc {
namespace {
struct BookEdit {
    const uint8_t* pages = nullptr;
    size_t pagesBytes = 0;
    const uint8_t* title = nullptr;
    uint16_t titleBytes = 0;
    int count = 0;
    bool valid = false;
};
int utf16Length(const uint8_t* text, size_t size) {
    int length = 0;
    for (size_t i = 0; i < size; ++i)
        if ((text[i] & 0xc0) != 0x80) length += text[i] >= 0xf0 ? 2 : 1;
    return length;
}
void jsonBookPage(ByteBuf& json, const uint8_t* text, size_t size) {
    Writer w(json);
    w.bytes((const uint8_t*)"{\"text\":\"", 9);
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < size; ++i) {
        uint8_t c = text[i];
        if (c == '"' || c == '\\') {
            w.u8('\\');
            w.u8(c);
        } else if (c < 32) {
            w.u8('\\');
            w.u8('u');
            w.u8('0');
            w.u8('0');
            w.u8(hex[c >> 4]);
            w.u8(hex[c & 15]);
        } else
            w.u8(c);
    }
    w.u8('"');
    w.u8('}');
}
} // namespace
// 1.21.8: the hand's slot, the pages as strings and, when signing, the title.
void Player::onEditBook(Reader& r) {
    int inventorySlot = r.varint();
    int32_t count = r.varint();
    if (!r.ok() || count < 0 || count > 100) return;
    BookEdit edit;
    edit.valid = true;
    ByteBuf pageBytes;   // the pages as an NBT string list payload (u16 length, bytes)
    Writer pw(pageBytes);
    for (int32_t i = 0; i < count; i++) {
        int32_t length = r.varint();
        const uint8_t* text = r.take((size_t)(length < 0 ? 0 : length));
        if (!r.ok() || length < 0 || length > 65535 || utf16Length(text, (size_t)length) > 1024) return;
        pw.u16((uint16_t)length);
        pw.bytes(text, (size_t)length);
    }
    bool signing = r.boolean();
    char titleText[64] = "";
    if (signing) r.string(titleText, sizeof(titleText));
    if (!r.ok() || pageBytes.failed() || dead || gamemode == GM_SPECTATOR ||
        ((inventorySlot < 0 || inventorySlot > 8) && inventorySlot != 40))
        return;
    int slot = inventorySlot == 40 ? SLOT_OFFHAND : SLOT_HOTBAR_START + inventorySlot;
    ItemStack& held = inv[slot];
    if (held.id != itm::WritableBook || held.empty()) return;
    edit.pages = pageBytes.data();
    edit.pagesBytes = pageBytes.size();
    edit.count = count;
    edit.title = (const uint8_t*)titleText;
    edit.titleBytes = (uint16_t)strlen(titleText);
    ByteBuf result;
    Writer w(result);
    NbtWriter n(w);
    // Keep the server's existing metadata. Canonicalization replaces only the
    // fields edited here; client-supplied enchantments/author are never copied.
    if (held.tagSize())
        w.bytes(held.tagData(), held.tagSize() - 1);
    else
        n.beginRoot();
    if (signing) {
        n.str("author", name);
        w.u8(NBT_STRING);
        w.u16(5);
        w.bytes((const uint8_t*)"title", 5);
        w.u16(edit.titleBytes);
        if (edit.title) w.bytes(edit.title, edit.titleBytes);
    }
    n.listHeader("pages", NBT_STRING, edit.count);
    if (signing && edit.count) {
        Reader pages(edit.pages, edit.pagesBytes);
        ByteBuf json;
        for (int i = 0; i < edit.count; ++i) {
            uint16_t size = pages.u16();
            const uint8_t* text = pages.take(size);
            json.clear();
            jsonBookPage(json, text, size);
            if (json.failed() || json.size() > 65535) return;
            w.u16((uint16_t)json.size());
            w.bytes(json.data(), json.size());
        }
    } else if (edit.pagesBytes)
        w.bytes(edit.pages, edit.pagesBytes);
    n.end();
    if (result.failed()) return;
    ItemStack updated = held;
    uint16_t damage = updated.damage;
    if (!updated.setTag(result.data(), result.size())) return;
    updated.damage = damage;
    if (signing) {
        updated.id = itm::WrittenBook;
        updated.count = 1;
    }
    held = updated;
    sendSlot(slot);
    srv->broadcastEquipment(*this);
}
} // namespace mc
