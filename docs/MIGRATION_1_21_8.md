# Research and plan: moving to the 1.21.8 protocol

Status (October 2026): **steps 1 to 3 are done**: the server speaks 1.21.8 (protocol
772) only. Done: the data generation (`tools/gen_data.js` from minecraft-data and the
official jar), the configuration state, and play with NBT text, data components, the
unified entity spawn, the new chunk and light format, block picking, sequence numbers.
Still to do: the world height −64..319 (step 4; the overworld is sent as 0..255 until
then), dialogs (step 5) and chunk batches. Stored worlds are not upgraded (storage
format 5): an older world is replaced by a new one. What changed in detail is under
[What the port changed](#what-the-port-changed) at the end of this page.

The original plan follows.

## Why

- **Server-driven UI.** Since 1.21.6 a server can open *dialogs* on the client: forms
  with text, buttons, check boxes, option lists and sliders, whose answers come back
  to the server. That is the natural home for an operator menu (world reset, new
  seed, statistics, settings), much better than a chest window of items. A dialog can
  also sit in the pause screen or behind the client's "Quick Actions" key.
- **The world height of today's game**: y −64 to 319 in the overworld (384 blocks,
  24 sections) instead of 0 to 255.
- Today's clients: players no longer need to install 1.16.5, and the test bots
  (mineflayer 4.39 lists 1.21.8 as tested) keep working.

## What 1.21.8 changes (from minecraft-data 3.117 and the Minecraft Wiki)

| | 1.16.5 (today) | 1.21.8 |
|---|---|---|
| protocol | 754 | 772 |
| connection states | handshake, status, login, play | + **configuration** between login and play (19 packets to the client, 9 back) |
| play packets | 92 to the client, 48 back | 134 to the client, 66 back |
| block states | 17 112 (global palette 15 bits) | 27 946 (still 15 bits) |
| blocks / items / entity types | 763 / 975 / 108 | 1 105 / 1 416 / 151 |
| world height (overworld) | 0..255, 16 sections | −64..319, 24 sections |

**Login and configuration.** After login success the client sends
`login_acknowledged`, and the connection enters the configuration state. There the
server sends `select_known_packs` (`minecraft:core` version `1.21.8`); a vanilla
client answers with the packs it has, and the server may then send the synchronized
registries (`registry_data`: dimension types, biomes, damage types, chat types,
painting/wolf/cat/pig/cow/frog/chicken variants, trim materials and patterns, banner
patterns, enchantments, jukebox songs, instruments, dialogs, ...) as **entry names
only, without NBT** for entries the client knows. That replaces today's 12 KB dimension
codec and is far smaller. Then `tags`, `feature_flags`, optionally `show_dialog`, and
`finish_configuration`; the client acknowledges and play starts. Keep-alive and pings
exist in configuration too.

**Play, the parts that touch most of our code**:
- `login` (Join Game) carries a `SpawnInfo` (dimension type id, world name, seed hash,
  game modes, debug/flat, death location, portal cooldown, sea level); `respawn`
  carries the same structure.
- Text is **NBT text components** (since 1.20.3), not JSON strings: chat, titles, kick
  messages, item names, the boss bar title, the tab list.
- **Item stacks are data components** (since 1.20.5): item id, count, then lists of
  added and removed components. Durability is the `damage` component; our item NBT
  gap (enchantments, names) becomes easier, but every slot encoder changes.
- One `spawn_entity` for all entities (mobs, players and objects); entity metadata
  indices and types renumbered.
- `chunk_batch_start` / `chunk_batch_finished`, and the client's
  `chunk_batch_received` with its rate: flow control the client drives, a good fit for
  the ESP32's limited send rate.
- Chunk data: 24 sections in the overworld (16 in the Nether and the End), biomes in a
  paletted container per section (4 × 4 × 4), heightmaps as a typed list, light
  masks as long arrays covering sections −1..24 (26 entries).
- `player_info` updates are a bitset of actions per player; `player_position` (the
  teleport) has velocity and relative flags; movement packets carry a flags byte.
- Chat: `system_chat` for everything the server says; secure chat off
  (`enforcesSecureChat` false), so no signed messages to handle.

**Dialogs** (1.21.6, the [Minecraft Wiki](https://minecraft.wiki/w/Dialog) and the
[1.21.6 article](https://www.minecraft.net/en-us/article/minecraft-java-edition-1-21-6)):
- `show_dialog` (play or configuration) with an inline dialog in NBT, or the id of one
  in the `minecraft:dialog` registry; `clear_dialog` closes it.
- Types: `notice`, `confirmation` (yes/no), `multi_action` (a grid of buttons),
  `dialog_list`, `server_links`. Common fields: `title`, `body` (text, items),
  `inputs`, `can_close_with_escape`, `pause`, `after_action`.
- Inputs: `text` (with `max_length`, multi-line), `boolean`, `single_option`,
  `number_range` (`start`, `end`, `step`).
- Actions: static click events (`run_command`, `show_dialog`, `copy_to_clipboard`,
  ...) and dynamic ones: `dynamic/run_command` (a command template filled with the
  inputs, `$(key)`), `dynamic/custom` (the inputs as an NBT payload, sent back in
  `custom_click_action`, in play and configuration).
- The tags `#minecraft:quick_actions` and `#minecraft:pause_screen_additions` put a
  dialog behind the Quick Actions key or in the pause menu.

An operator menu would be: `/menu` (or the Quick Actions key) opens a `multi_action`
dialog (Statistics, Settings, Players, World); "World" opens a form with a text input
for the seed, a world-type option list and a confirmation; the answer comes back as a
`custom_click_action`, and the server checks that the sender is an operator.

## The world height: −64..319

- **Memory.** Sections are allocated only where there are blocks, so the air above
  costs nothing; the four new sections below y 0 are full (deepslate). That is about
  +30% section memory per overworld chunk underground: the same PSRAM holds about a
  quarter fewer resident chunks (today the free heap stays above ~4.4 MB of the 8 MB
  under 8 players at view 32, so there is room, but it has to be measured).
- **Light.** The light engine's grid height grows by up to 8 sections, so the region
  light scratch (today 44 × 44 × H bytes) grows by up to 50% for deep caves; per-chunk
  light stays bounded by the highest section, as today.
- **Chunk packets** get 4 more sections of mostly one block type (a few bytes each
  with a single-value palette) plus 26 light entries.
- **Generator.** A new generator version (3) for the new range: the terrain above
  y 0 stays as it is (so old and new chunks line up), y −64..−1 becomes deepslate with
  its ores, caves reaching down, and the bedrock floor moves from y 0..4 to −64..−60.
  Mountains could use the extra height later.
- **Existing worlds.** Chunks stored with y 0..255 are upgraded when loaded (as
  vanilla 1.18 did): the four sections below are generated, the bedrock layer at
  y 0..4 becomes deepslate, block state ids are mapped from 1.16.5 to 1.21.8 (a table
  generated from block names and properties), and the chunk is saved in the new
  record version. The Nether and the End keep 0..255.

## Costs on the ESP32-S3

- **Flash**: the data tables grow by roughly half (states, items, recipes, tags):
  ~200–300 KB more; the 16 MB flash has room.
- **Internal RAM** (the tight one, ~38 KB free at the minimum): configuration-state
  packets are streamed like today's Join Game; registry data without NBT is small.
  Nothing new needs internal RAM if large buffers stay in PSRAM.
- **CPU**: NBT text components instead of JSON strings are cheaper to write; chunk
  encoding grows with the sections below y 0 (about +25% for a typical chunk).
- **One protocol at a time.** Supporting both 1.16.5 and 1.21.8 would double the data
  tables and every encoder. The plan moves to 1.21.8 only; the last 1.16.5 build is
  kept as a git tag.

## Plan in steps (each a branch, tested on the PC and the board, measured)

1. **Data generation** (~2 days): `tools/gen_data.js` for 1.21.8 (blocks, states,
   items, entities, packets, tags, registries as names), and a 1.16.5 → 1.21.8 state
   and item remap table. Unit tests: every 1.16.5 state maps to a 1.21.8 state.
2. **Configuration state** (~2 days): login acknowledged, known packs, registry data
   by name, tags, feature flags, finish; keep-alive. A mineflayer 1.21.8 bot reaches
   play.
3. **Play basics** (~1 week): Join Game / Respawn with SpawnInfo, NBT text components,
   item stacks with data components (damage, custom name), entity spawn and
   metadata, player info, teleport and movement, chat, commands tree, inventories and
   windows. The existing test suite (smoke, gameplay, persistence, mobs, dimensions)
   moves to 1.21.8 bots and must pass.
4. **Chunks and the world height** (~1 week): 24 sections, biome containers,
   heightmaps, light masks, chunk batches; generator version 3; the lazy upgrade of
   stored chunks and player inventories (remap tables); storage record version 3.
   Measured against today's chunks/s, heap and light times.
5. **Dialogs and the operator menu** (~3 days): `/menu`, the Quick Actions tag, the
   world reset / new seed flow with confirmation, statistics, settings, players.
6. **Cut-over**: README and roadmap, the 1.16.5 build tagged, the board's world
   upgraded and checked (portals, the dragon fight state, players).

Rough size: 4 000 to 6 000 changed lines, most in the protocol layer and the
generated tables; the world, light, storage and game logic stay.

## Until then

The operator menu exists on 1.16.5 as a chest window (`/menu`, items as buttons, the
seed typed in the chat; see the README); its actions are plain server functions, so
step 5 only replaces the window with dialogs.

## What the port changed

**Data** (`tools/gen_data.js`, `tools/fetch_vanilla.sh`):
- Blocks (1105, 27 946 states), items (1416), entities (151) with their metadata indices,
  packet ids of the login, configuration and play states, data component, parser and
  particle ids from minecraft-data 3.117; block entity and menu types read from the
  official jar's constant pool (minecraft-data lacks them).
- Registries and tags from the jar's data pack. Each registry exists twice: names only
  (vanilla clients have the `minecraft:core` pack, as `select_known_packs` tells) and
  with every entry's data (mineflayer and other clients), each also deflated at build
  time. Dimension types and biomes always carry their data (the overworld is 0..255).
- The per-state redstone and piston values of the 1.16.5 jar are carried over by block
  name and properties (`tools/redstone/states-1.16.5.json`); 17 359 states keep them,
  10 587 new ones get them from rules. Collision boxes are minecraft-data's (checked
  against the jar's 1.16.5 boxes: only snow, turtle eggs and sea pickles differ).
- The redstone traces of the official 1.16.5 server are remapped to the new state ids
  (`tools/redstone/remap_traces.js`; a filled cauldron is now `water_cauldron`).
- Int properties keep their labels (`repeater[delay=2]` is delay 2, not index 2).

**Protocol** (`lib/mcore`):
- Login → configuration (brand, feature flags, known packs, registries, tags) → play.
- Text: the JSON the server builds is written as NBT (`mc/text.h`), `clickEvent` as
  `click_event`; text from clients is read back into JSON.
- Item stacks: the server keeps its item NBT and writes data components (damage, name,
  lore, enchantments, writable and written books); creative stacks come back as NBT.
- Join Game / Respawn with the spawn info; the "chunks load start" game event; system
  chat for all messages (no signing); player info as action bits; teleports with
  velocity and flags; one spawn packet for all entities; metadata by generated index;
  damage events for the hurt flash; the new explosion, sound, window, sign and book
  packets; acknowledged block sequences; sneaking from `player_input`; block picking on
  the server; window clicks resynchronised (the client's hashed slots are not used).
- Chunks: all 16 sections with biome containers, no data length (1.21.5), light inside
  the chunk packet.

**Checks**: 235 unit tests (new: text, slots, the configuration payloads, a chunk parsed
like a client, the deflated packets); the end-to-end suite with mineflayer 1.21.8; on
the board: smoke, dimensions, redstone, dashboard, stress. The generated terrain is the
same block for block (hashed by block names on both versions, all three dimensions).

**On the board** (A/B, a player flying 70 s over new terrain, fresh worlds):

| firmware | ms/tick median | loop stall median / max | internal RAM min | firmware size |
|---|---|---|---|---|
| 1.16.5 | 3.2 | 14 / 73 ms | 40 KB | 1.41 MB |
| 1.21.8 | 3.2 | 16 / 21 ms | 44 KB | 1.65 MB |

The 73 ms of 1.16.5 is the join (the 12 KB dimension codec compressed on the loop); with
the configuration packets deflated at build time a 1.21.8 join takes one 64 ms pass.
