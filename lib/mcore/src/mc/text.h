// Text components on the wire. Since 1.20.3 the protocol sends text as network NBT,
// not as JSON strings. The server builds its messages as JSON text (the format of
// /tellraw), and writeTextNbt converts them while writing the packet.
#pragma once
#include <stddef.h>
#include "mc/io.h"

namespace mc {

// Writes a JSON text component (object, string or array) as a network NBT text
// component: a root tag without a name. Keys follow 1.21.5+: clickEvent becomes
// click_event (with command, url or page instead of value), hoverEvent becomes
// hover_event. Invalid JSON is written as plain text.
void writeTextNbt(Writer& w, const char* json);
// Plain text (no JSON): a string tag.
void writeTextPlain(Writer& w, const char* text);
// Converts a network NBT text component back into JSON text (the subset this server
// writes and stores: text, translate, with, extra, color and the style flags).
// Returns false when the input is not a text component.
bool readTextNbtAsJson(Reader& r, char* out, size_t cap);

}  // namespace mc
