// World-wide state that is not in any chunk: where the nether portals are (to link
// them), and the dragon fight. Saved with the world metadata (WorldMeta::extra).
#pragma once
#include <stdint.h>
#include <string.h>

namespace mc {

struct PortalRef {
    int32_t x = 0, z = 0;   // the lowest inside block nearest the negative end of its axis
    int16_t y = 0;
    uint8_t dim = 0;
    uint8_t axis = 0;       // 0: the portal spans x, 1: it spans z
};

// The dragon fight in the End (dragon.cpp).
struct DragonFight {
    enum : uint8_t { NOT_STARTED = 0, DRAGON_ALIVE = 1, KILLED = 2 };
    uint8_t state = NOT_STARTED;
    bool previouslyKilled = false;   // the egg and the 12000 XP are for the first kill only
    bool eggPlaced = false;
    float dragonHealth = 200;        // kept over restarts (the dragon itself is not saved)
    uint16_t crystals = 0x3FF;       // bit per spike: its end crystal still stands
    int16_t portalY = 0;             // the exit portal's floor (0: not placed yet)
};

struct WorldState {
    static const int MAX_PORTALS = 96;
    PortalRef portals[MAX_PORTALS];
    int nPortals = 0;
    DragonFight dragon;
    bool dirty = false;

    void addPortal(const PortalRef& p) {
        for (int i = 0; i < nPortals; i++) {
            const PortalRef& q = portals[i];
            if (q.dim == p.dim && q.x == p.x && q.y == p.y && q.z == p.z && q.axis == p.axis) return;
        }
        if (nPortals == MAX_PORTALS) {   // full: forget the oldest
            memmove(portals, portals + 1, sizeof(PortalRef) * (MAX_PORTALS - 1));
            nPortals--;
        }
        portals[nPortals++] = p;
        dirty = true;
    }
    void removePortal(int i) {
        if (i < 0 || i >= nPortals) return;
        memmove(portals + i, portals + i + 1, sizeof(PortalRef) * (size_t)(nPortals - i - 1));
        nPortals--;
        dirty = true;
    }

    // Serialised form: version, portals, dragon fight. Returns the length (0: too small).
    size_t encode(uint8_t* out, size_t cap) const {
        size_t need = 2 + (size_t)nPortals * 12 + 10;
        if (need > cap) return 0;
        size_t n = 0;
        out[n++] = 2;   // version (2: + crystals, portal height)
        out[n++] = (uint8_t)nPortals;
        for (int i = 0; i < nPortals; i++) {
            const PortalRef& p = portals[i];
            out[n++] = p.dim;
            out[n++] = p.axis;
            put32(out + n, (uint32_t)p.x); n += 4;
            out[n++] = (uint8_t)((uint16_t)p.y >> 8);
            out[n++] = (uint8_t)p.y;
            put32(out + n, (uint32_t)p.z); n += 4;
        }
        out[n++] = dragon.state;
        out[n++] = (uint8_t)((dragon.previouslyKilled ? 1 : 0) | (dragon.eggPlaced ? 2 : 0));
        uint32_t h;
        memcpy(&h, &dragon.dragonHealth, 4);
        put32(out + n, h); n += 4;
        out[n++] = (uint8_t)(dragon.crystals >> 8);
        out[n++] = (uint8_t)dragon.crystals;
        out[n++] = (uint8_t)((uint16_t)dragon.portalY >> 8);
        out[n++] = (uint8_t)dragon.portalY;
        return n;
    }
    bool decode(const uint8_t* in, size_t len) {
        // in place: a WorldState temporary is 1.6 KB of stack
        nPortals = 0;
        dragon = DragonFight();
        dirty = false;
        if (len < 2 || in[0] < 1 || in[0] > 2) return false;
        int version = in[0];
        int np = in[1];
        if (np > MAX_PORTALS || len < 2 + (size_t)np * 12 + (version >= 2 ? 10 : 8)) return false;
        size_t n = 2;
        for (int i = 0; i < np; i++) {
            PortalRef& p = portals[i];
            p.dim = in[n++];
            p.axis = in[n++];
            p.x = (int32_t)get32(in + n); n += 4;
            p.y = (int16_t)((uint16_t)in[n] << 8 | in[n + 1]); n += 2;
            p.z = (int32_t)get32(in + n); n += 4;
        }
        nPortals = np;
        dragon.state = in[n++];
        dragon.previouslyKilled = (in[n] & 1) != 0;
        dragon.eggPlaced = (in[n] & 2) != 0;
        n++;
        uint32_t h = get32(in + n);
        memcpy(&dragon.dragonHealth, &h, 4);
        n += 4;
        if (version >= 2) {
            dragon.crystals = (uint16_t)(in[n] << 8 | in[n + 1]);
            dragon.portalY = (int16_t)((uint16_t)in[n + 2] << 8 | in[n + 3]);
        }
        if (!(dragon.dragonHealth > 0 && dragon.dragonHealth <= 200)) dragon.dragonHealth = 200;
        if (dragon.state > DragonFight::KILLED) dragon.state = DragonFight::NOT_STARTED;
        return true;
    }

private:
    static void put32(uint8_t* b, uint32_t v) {
        b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16); b[2] = (uint8_t)(v >> 8); b[3] = (uint8_t)v;
    }
    static uint32_t get32(const uint8_t* b) {
        return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
    }
};

}  // namespace mc
