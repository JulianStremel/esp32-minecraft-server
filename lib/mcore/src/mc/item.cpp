#include "mc/item.h"
#include <string.h>
#include "mc/nbt.h"
#include "mc/registry.h"

namespace mc {

void writeSlot(Writer& w, const ItemStack& s) {
    if (s.empty()) {
        w.boolean(false);
        return;
    }
    w.boolean(true);
    w.varint(s.id);
    w.i8((int8_t)s.count);
    if (s.damage) {
        NbtWriter n(w);
        n.beginRoot();
        n.i32("Damage", s.damage);
        n.end();
    } else {
        w.u8(NBT_END);
    }
}

bool readSlot(Reader& r, ItemStack& s) {
    s.clear();
    if (!r.boolean()) return r.ok();
    int32_t id = r.varint();
    int8_t count = r.i8();
    struct Ctx { int32_t damage = 0; } ctx;
    nbtVisitRoot(r, [](void* c, uint8_t t, const char* name, Reader& rr) {
        if (t == NBT_INT && !strcmp(name, "Damage")) { ((Ctx*)c)->damage = rr.i32(); return true; }
        return false;
    }, &ctx);
    if (!r.ok()) return false;
    if (id <= 0 || id >= NUM_ITEMS || count <= 0) return true;  // treat as empty
    s.id = (uint16_t)id;
    s.count = (uint8_t)(count > 64 ? 64 : count);
    s.damage = (uint16_t)(ctx.damage < 0 ? 0 : (ctx.damage > 65535 ? 65535 : ctx.damage));
    return true;
}

}  // namespace mc
