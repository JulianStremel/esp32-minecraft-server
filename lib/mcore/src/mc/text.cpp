#include "mc/text.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mc/nbt.h"

namespace mc {

namespace {

struct Json {
    const char* p;
    bool ok = true;

    void ws() {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    }
    bool eat(char c) {
        ws();
        if (*p != c) return false;
        p++;
        return true;
    }
    // Decodes a JSON string into out (UTF-8); returns its length.
    size_t str(char* out, size_t cap) {
        size_t n = 0;
        ws();
        if (*p != '"') { ok = false; return 0; }
        p++;
        while (*p && *p != '"') {
            uint32_t c = (uint8_t)*p++;
            if (c == '\\') {
                char e = *p++;
                switch (e) {
                    case 'n': c = '\n'; break;
                    case 't': c = '\t'; break;
                    case 'r': c = '\r'; break;
                    case 'b': c = '\b'; break;
                    case 'f': c = '\f'; break;
                    case 'u': {
                        char hex[5] = {0};
                        for (int i = 0; i < 4 && *p; i++) hex[i] = *p++;
                        c = (uint32_t)strtoul(hex, nullptr, 16);
                        char u[4];
                        int k = 0;
                        if (c < 0x80) u[k++] = (char)c;
                        else if (c < 0x800) { u[k++] = (char)(0xC0 | (c >> 6)); u[k++] = (char)(0x80 | (c & 63)); }
                        else { u[k++] = (char)(0xE0 | (c >> 12)); u[k++] = (char)(0x80 | ((c >> 6) & 63)); u[k++] = (char)(0x80 | (c & 63)); }
                        for (int i = 0; i < k; i++) if (n + 1 < cap) out[n++] = u[i];
                        continue;
                    }
                    case 0: ok = false; return n;
                    default: c = (uint8_t)e; break;
                }
            }
            if (n + 1 < cap) out[n++] = (char)c;
        }
        if (*p != '"') { ok = false; return n; }
        p++;
        if (cap) out[n] = 0;
        return n;
    }
    // Decodes a JSON string (at p) as an NBT string payload into w: the length, then the
    // UTF-8 bytes. Nothing is buffered (this runs on the game loop's small stack).
    void strNbt(Writer& w) {
        Json probe{p};
        size_t n = probe.decode(nullptr);
        ok = ok && probe.ok;
        if (n > 65535) n = 65535;
        w.u16((uint16_t)n);
        decode(&w, n);
    }
    // Decodes the JSON string at p into w (at most limit bytes); returns the length.
    size_t decode(Writer* w, size_t limit = (size_t)-1) {
        size_t n = 0;
        ws();
        if (*p != '"') { ok = false; return 0; }
        p++;
        auto put = [&](uint8_t c) {
            if (n < limit && w) w->u8(c);
            n++;
        };
        while (*p && *p != '"') {
            uint32_t c = (uint8_t)*p++;
            if (c == '\\') {
                char e = *p++;
                switch (e) {
                    case 'n': c = '\n'; break;
                    case 't': c = '\t'; break;
                    case 'r': c = '\r'; break;
                    case 'b': c = '\b'; break;
                    case 'f': c = '\f'; break;
                    case 'u': {
                        char hex[5] = {0};
                        for (int i = 0; i < 4 && *p; i++) hex[i] = *p++;
                        c = (uint32_t)strtoul(hex, nullptr, 16);
                        if (c < 0x80) put((uint8_t)c);
                        else if (c < 0x800) { put((uint8_t)(0xC0 | (c >> 6))); put((uint8_t)(0x80 | (c & 63))); }
                        else { put((uint8_t)(0xE0 | (c >> 12))); put((uint8_t)(0x80 | ((c >> 6) & 63))); put((uint8_t)(0x80 | (c & 63))); }
                        continue;
                    }
                    case 0: ok = false; return n;
                    default: c = (uint8_t)e; break;
                }
            }
            put((uint8_t)c);
        }
        if (*p != '"') { ok = false; return n; }
        p++;
        return n;
    }
    // Skips any JSON value.
    void skip() {
        ws();
        if (*p == '"') { char tmp[1]; str(tmp, 0); return; }
        if (*p == '{' || *p == '[') {
            char close = *p == '{' ? '}' : ']';
            p++;
            ws();
            if (*p == close) { p++; return; }
            while (ok) {
                if (close == '}') { char tmp[1]; str(tmp, 0); if (!eat(':')) { ok = false; return; } }
                skip();
                if (eat(',')) continue;
                if (!eat(close)) ok = false;
                return;
            }
            return;
        }
        while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ') p++;
    }
    int countArray() {   // elements of the array at p (p at '[')
        Json c{p};
        c.p++;
        c.ws();
        if (*c.p == ']') return 0;
        int n = 0;
        while (c.ok) {
            c.skip();
            n++;
            if (c.eat(',')) continue;
            break;
        }
        return n;
    }
};

void nbtName(Writer& w, uint8_t type, const char* name) {
    w.u8(type);
    size_t l = strlen(name);
    w.u16((uint16_t)l);
    w.bytes((const uint8_t*)name, l);
}
void nbtStr(Writer& w, const char* s, size_t l) {
    w.u16((uint16_t)l);
    w.bytes((const uint8_t*)s, l);
}

void component(Json& j, Writer& w);

// The payload of a compound from a JSON object (j.p at '{'); the end tag included.
// click: the object is a click event (value is renamed after the action).
void object(Json& j, Writer& w, bool click) {
    j.eat('{');
    char action[32] = "";
    j.ws();
    if (*j.p == '}') { j.p++; w.u8(NBT_END); return; }
    while (j.ok) {
        char key[32];
        j.str(key, sizeof(key));
        if (!j.eat(':')) { j.ok = false; break; }
        j.ws();
        const char* name = key;
        if (!strcmp(key, "clickEvent")) name = "click_event";
        else if (!strcmp(key, "hoverEvent")) name = "hover_event";
        if (click && !strcmp(key, "value")) {
            if (!strcmp(action, "open_url")) name = "url";
            else if (!strcmp(action, "change_page")) name = "page";
            else if (!strcmp(action, "run_command") || !strcmp(action, "suggest_command")) name = "command";
        }
        if (*j.p == '"') {
            if (!strcmp(key, "action")) {
                Json peek{j.p};
                peek.str(action, sizeof(action));
            }
            if (!strcmp(name, "page")) {
                char num[16];
                j.str(num, sizeof(num));
                nbtName(w, NBT_INT, name);
                w.i32(atoi(num));
            } else {
                nbtName(w, NBT_STRING, name);
                j.strNbt(w);
            }
        } else if (*j.p == '{') {
            bool isClick = !strcmp(name, "click_event");
            if (!strcmp(name, "hover_event") || isClick || !strcmp(name, "style")) {
                nbtName(w, NBT_COMPOUND, name);
                object(j, w, isClick);
            } else {
                nbtName(w, NBT_COMPOUND, name);
                component(j, w);
            }
        } else if (*j.p == '[') {
            int n = j.countArray();
            nbtName(w, NBT_LIST, name);
            w.u8(n ? NBT_COMPOUND : NBT_END);
            w.i32(n);
            j.p++;
            j.ws();
            if (*j.p == ']') j.p++;
            else
                while (j.ok) {
                    component(j, w);
                    if (j.eat(',')) continue;
                    if (!j.eat(']')) j.ok = false;
                    break;
                }
        } else if (!strncmp(j.p, "true", 4) || !strncmp(j.p, "false", 5)) {
            bool v = *j.p == 't';
            j.p += v ? 4 : 5;
            nbtName(w, NBT_BYTE, name);
            w.u8(v ? 1 : 0);
        } else {   // a number
            char* end;
            double v = strtod(j.p, &end);
            if (end == j.p) { j.ok = false; break; }
            j.p = end;
            if (!strcmp(name, "page") || v == (int32_t)v) { nbtName(w, NBT_INT, name); w.i32((int32_t)v); }
            else { nbtName(w, NBT_DOUBLE, name); w.f64(v); }
        }
        if (j.eat(',')) continue;
        if (!j.eat('}')) j.ok = false;
        break;
    }
    w.u8(NBT_END);
}

// One component as a compound payload (list element / compound value).
void component(Json& j, Writer& w) {
    j.ws();
    if (*j.p == '{') { object(j, w, false); return; }
    if (*j.p == '[') {   // [first, rest...]: first with the rest as extra
        int n = j.countArray();
        j.p++;
        if (n == 0) { j.eat(']'); nbtName(w, NBT_STRING, "text"); nbtStr(w, "", 0); w.u8(NBT_END); return; }
        nbtName(w, NBT_STRING, "text");
        nbtStr(w, "", 0);
        nbtName(w, NBT_LIST, "extra");
        w.u8(NBT_COMPOUND);
        w.i32(n);
        while (j.ok) {
            component(j, w);
            if (j.eat(',')) continue;
            if (!j.eat(']')) j.ok = false;
            break;
        }
        w.u8(NBT_END);
        return;
    }
    nbtName(w, NBT_STRING, "text");
    if (*j.p == '"') j.strNbt(w);
    else {   // a number or literal as text
        const char* s = j.p;
        j.skip();
        nbtStr(w, s, (size_t)(j.p - s));
    }
    w.u8(NBT_END);
}

}  // namespace

void writeTextPlain(Writer& w, const char* text) {
    w.u8(NBT_STRING);
    nbtStr(w, text, strlen(text));
}

void writeTextNbt(Writer& w, const char* json) {
    // validate first: a broken JSON text must not produce a broken packet
    {
        Json v{json};
        v.skip();
        v.ws();
        if (!v.ok || *v.p) { writeTextPlain(w, json); return; }
    }
    Json j{json};
    j.ws();
    if (*j.p == '"') {
        w.u8(NBT_STRING);
        j.strNbt(w);
        return;
    }
    w.u8(NBT_COMPOUND);
    component(j, w);
}

namespace {

struct Out {
    char* p;
    size_t cap, n = 0;
    void put(const char* s, size_t l) {
        for (size_t i = 0; i < l; i++)
            if (n + 1 < cap) p[n++] = s[i];
    }
    void put(const char* s) { put(s, strlen(s)); }
    void esc(const char* s, size_t l) {
        put("\"");
        for (size_t i = 0; i < l; i++) {
            char c = s[i];
            if (c == '"' || c == '\\') { char e[2] = {'\\', c}; put(e, 2); }
            else if (c == '\n') put("\\n");
            else if ((uint8_t)c < 0x20) { char e[8]; snprintf(e, sizeof(e), "\\u%04x", c); put(e); }
            else put(&c, 1);
        }
        put("\"");
    }
};

bool payloadJson(Reader& r, uint8_t type, Out& o, int depth);

bool compoundJson(Reader& r, Out& o, int depth) {
    o.put("{");
    bool first = true;
    while (r.ok()) {
        uint8_t t = r.u8();
        if (t == NBT_END) break;
        uint16_t l = r.u16();
        const uint8_t* name = r.take(l);
        if (!r.ok()) return false;
        if (!first) o.put(",");
        first = false;
        o.esc((const char*)name, l);
        o.put(":");
        if (t == NBT_BYTE && (l == 4 || l == 6 || l == 9 || l == 10 || l == 13)) {   // style flags
            o.put(r.u8() ? "true" : "false");
            continue;
        }
        if (!payloadJson(r, t, o, depth + 1)) return false;
    }
    o.put("}");
    return r.ok();
}

bool payloadJson(Reader& r, uint8_t type, Out& o, int depth) {
    if (depth > 16) return false;
    char num[32];
    switch (type) {
        case NBT_STRING: {
            uint16_t l = r.u16();
            const uint8_t* s = r.take(l);
            if (!r.ok()) return false;
            o.esc((const char*)s, l);
            return true;
        }
        case NBT_COMPOUND: return compoundJson(r, o, depth);
        case NBT_LIST: {
            uint8_t e = r.u8();
            int32_t n = r.i32();
            if (n < 0 || n > 4096) return false;
            o.put("[");
            for (int32_t i = 0; i < n && r.ok(); i++) {
                if (i) o.put(",");
                if (!payloadJson(r, e, o, depth + 1)) return false;
            }
            o.put("]");
            return r.ok();
        }
        case NBT_BYTE: snprintf(num, sizeof(num), "%d", r.i8()); break;
        case NBT_SHORT: snprintf(num, sizeof(num), "%d", r.i16()); break;
        case NBT_INT: snprintf(num, sizeof(num), "%d", (int)r.i32()); break;
        case NBT_LONG: snprintf(num, sizeof(num), "%lld", (long long)r.i64()); break;
        case NBT_FLOAT: snprintf(num, sizeof(num), "%g", r.f32()); break;
        case NBT_DOUBLE: snprintf(num, sizeof(num), "%g", r.f64()); break;
        default: return nbtSkipPayload(r, type, depth) && (o.put("null"), true);
    }
    o.put(num);
    return r.ok();
}

}  // namespace

bool readTextNbtAsJson(Reader& r, char* out, size_t cap) {
    if (!cap) return false;
    Out o{out, cap};
    uint8_t type = r.u8();
    if (!r.ok() || (type != NBT_STRING && type != NBT_COMPOUND && type != NBT_LIST)) return false;
    bool ok = payloadJson(r, type, o, 0);
    out[o.n] = 0;
    return ok && o.n + 1 < cap;
}

}  // namespace mc
