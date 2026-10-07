# esp32-minecraft-server

A Minecraft Java Edition server that runs on an ESP32. It speaks the 1.16.5
protocol (754), so an unmodified client can join. The world is generated on the
chip and stored on the network through a **Network Block Device** (NBD) in a
compact, crash-safe binary format, so it is not limited by the board's flash.

This is a rewrite of [nikisalli/esp32-minecraft-server](https://github.com/nikisalli/esp32-minecraft-server).
The server core (`lib/mcore`) is portable C++17. The same code also builds as a PC
server, which runs the unit and end-to-end tests. The firmware itself runs in an
emulated ESP32-S3 for debugging, benchmarking and load tests ([docs/QEMU.md](docs/QEMU.md)).

![Four players exploring terrain the firmware generates on an emulated ESP32-S3](docs/images/exploring.gif)

*The firmware running on an emulated ESP32-S3 (QEMU): four players jump into unexplored
land, the terrain appears chunk by chunk as the device generates it, and they walk into
fresh terrain. Recorded with `node test/record_gif.js` (the browser-based viewer adds
its own delay, see [docs/QEMU.md](docs/QEMU.md#recording-a-gif)).*

## Features

- **Protocol 1.16.5**, offline mode:
  - server list with MOTD, player count and icon
  - zlib-compressed packets
  - vanilla registries and tags, keep-alive, tab list with ping
- **Terrain:** seeded generator with 25 biomes (oceans, rivers, beaches, deserts,
  badlands, jungles, taigas, mountains, ...), caves, ores and six tree types.
  Superflat and void worlds are also available. Chunks are streamed nearest-first
  within the view distance.
- **Lighting:** sky light and block light (the [comparison](#compared-with-vanilla-1165)
  lists where it differs from vanilla).
- **Survival:**
  - digging with server-side timing and tool tiers, drops and item pickup
  - health, hunger, saturation, fall damage, drowning, lava and fire, death and respawn, XP
- **Items and containers:**
  - player inventory, the vanilla crafting recipes (2x2 and 3x3), chests, trapped chests, barrels
  - furnaces, smokers and blast furnaces that smelt with fuel
  - beds (respawn point), signs, doors, trapdoors, levers, buttons, buckets, bows, food
- **World simulation:**
  - flowing water and lava, falling sand and gravel
  - crops, sugar cane and cactus growth, grass spreading
  - correct shapes for stairs and fences, support checks (plants, torches, ...)
- **Mobs:**
  - passive: cows, pigs, sheep, chickens
  - hostile: zombies, skeletons (they shoot), spiders, creepers (they explode)
  - hostile mobs burn in daylight; mobs spawn naturally, take damage and drop loot; PvP
- **Commands:** `help list msg tell w me seed spawn tps lag storage` for everybody;
  `gamemode tp give clear time weather kill setworldspawn spawnpoint say difficulty xp
  heal feed summon setblock fill op deop kick save-all stop fly workers` for operators
  (`teleport` and `experience` are aliases). Tab completion works.
- **Persistence** on any NBD server: chunks you changed, player data (position,
  inventory, health, XP, spawn point) and world metadata. Chunks that were never
  modified are not stored at all, because they are regenerated from the seed.
- **Two worker threads**, one per core, generate, light and compress chunks, so the
  game loop stays responsive (see [Threads](#threads)).

## Hardware

**A board with at least 8 MB of PSRAM is required.** Chunks, packet buffers and the
worker threads' buffers live in PSRAM, and the firmware refuses to start without it.

| board | PlatformIO environment |
|---|---|
| ESP32-S3 N8R8 / N16R8 (octal PSRAM), e.g. ESP32-S3-DevKitC-1 N8R8: recommended | `esp32-s3` (default) |
| ESP32 WROVER-E with 8 MB PSRAM (the classic ESP32 maps 4 MB of it) | `esp32-wrover` |
| virtual ESP32-S3 in QEMU | `esp32s3-qemu`, see [docs/QEMU.md](docs/QEMU.md) |

With 8 MB of PSRAM the ESP32-S3 build serves up to 10 players with a view
distance of up to 8 and keeps about 200 chunks resident.

## Quick start

1. **Start an NBD server** that holds the world, on any machine in your network
   (a PC, NAS or Raspberry Pi):

   ```sh
   # the bundled server (Python 3, no dependencies); the image is created sparse
   python3 tools/nbd_server.py --file world.img --size 2G --port 10809

   # or any standard NBD server
   nbdkit -f file world.img                      # truncate -s 2G world.img first
   qemu-nbd -t -f raw -p 10809 world.img
   nbd-server 10809 /path/to/world.img
   ```

   The world border is `MC_WORLD_RADIUS` chunks from the centre (64 by default),
   shrunk to what the export holds: 512 MiB fits 31 chunks (496 blocks) in every
   direction, 1 GiB fits 45 and 2 GiB fits 63. The image is sparse, so it only uses as much disk as the world needs,
   typically a few MB.

2. **Configure:** copy `include/config_edit_me.h` to `include/config.h` and set your
   WiFi credentials, `NBD_HOST` (the machine from step 1) and the server options
   (MOTD, game mode, difficulty, operators, whitelist, seed, world type, ...).

3. **Build and flash** with [PlatformIO](https://platformio.org/):

   ```sh
   pio run -e esp32-s3 -t upload && pio device monitor
   ```

4. **Connect** with Minecraft 1.16.5 to the IP address printed on the serial
   console, or to `esp32-minecraft.local` (mDNS).

Without `NBD_HOST` the server still runs, but the world resets on every reboot.

## Storage format

The world lives on the NBD export as raw binary records. NBD transfers fixed-size
binary blocks with almost no protocol overhead, and every NBD server can serve a
plain file:

```
0       superblock copy A  \  alternate writes with a sequence number;
512     superblock copy B  /  the newest valid copy wins
4096    player table       hashed by UUID, 512 bytes per player
64 KiB  chunk area         two slots per chunk inside the world border
```

- A chunk save goes to the older of its two slots, with a CRC32 and zlib
  compression. An interrupted write, for example from a power cut, never destroys
  the last good copy.
- A chunk record also holds the chunk's pending scheduled block ticks (flowing
  water and lava, buttons, fire) with their delays and priorities, like vanilla's
  `TileTicks`, so they continue after a restart. Records written before this
  (without the ticks flag) still load.
- Reads are pipelined: the storage lookups of all chunks the players need in a
  tick share one round trip. Writes are posted, so their replies are collected
  later. The client reconnects after network errors.
- The world autosaves every 60 s and saves on `/stop`.

## Threads

```
core 1:  server task (game loop, 20 TPS)  |  worker 1 (lower priority: runs while the loop sleeps)
core 0:  WiFi / lwIP                      |  worker 0
```

The game loop sleeps until there is something to do. It waits in `select()` on the
listening socket, the players' sockets (and, for sockets with unsent output, on
room to write) and an `eventfd`. A periodic 50 ms `esp_timer` notifies the game task
once per tick and signals the `eventfd`; a worker signals it when it finishes urgent
work. The PC build does the same with `poll()` and a pipe
(`plat::waitForWork()`). If the timer fired more than once since the loop last
looked, the loop overran a tick: late ticks are caught up, up to five at a time, and
more than a second behind, the backlog is dropped with vanilla's "Can't keep up!"
warning. `/tps` shows the wakeups per second and the overruns, late and skipped
ticks of the last 2 s; `/lag` also shows why the waits ended.

The game loop handles packets, game logic and storage I/O. Everything CPU-heavy
runs on the workers ([`lib/mcore/src/mc/jobs.h`](lib/mcore/src/mc/jobs.h),
[`server/chunk_jobs.h`](lib/mcore/src/mc/server/chunk_jobs.h)):

- generating chunks and decoding stored ones
- computing light and building and compressing the chunk and light packets
- re-lighting after block changes
- encoding and compressing chunk saves

Jobs work on snapshots (a copy of the chunk plus its neighbours' border heights),
never on the live world. Their results are applied by the game loop. A chunk with
a job in flight is not evicted, and a packet prepared from a chunk that changed in
the meantime is rebuilt.

The queue has four priority classes, each with a waiting budget: urgent (0 ms),
high (100 ms), normal (500 ms) and background (3 s). A job's deadline is the time it
was queued plus its budget, and the workers always take the job with the earliest
deadline. Urgent work therefore goes first, but a waiting background job's deadline
eventually comes before that of every newly queued urgent job, so a steady stream of
urgent work cannot starve it.

- urgent: the chunks under and next to a player (distance ≤ 1)
- high: chunks within 3 chunks, and re-lighting after block changes
- normal: the rest of the view distance
- background: saves

Loads and sends are promoted when a player comes closer and cancelled when every
player has moved out of range. Loads go through an admission step: a few slots are
reserved for urgent loads, and every player gets a share, so one fast player
cannot take every slot. `/workers` shows the queue per class and the longest wait.

Measured on the emulated ESP32-S3 ([docs/QEMU.md](docs/QEMU.md)):

- A new chunk costs about 52 ms of CPU time: generation 34 ms, light 8.5 ms,
  compression 8 ms. That is more than a whole 50 ms tick of one core.
- With players exploring new terrain, the two workers cut the longest game-loop
  stalls from 250–400 ms to roughly 25–90 ms and deliver about 1.5–2× as many
  chunks per second.
- With the priority queue instead of a single FIFO queue (6 players, 2 workers,
  3 runs each), the chunk under a player who jumps into new terrain arrives after
  about 680 ms instead of 1020 ms, all nine chunks around them after about 1.2 s
  instead of 4.3 s, and the throughput is about 10% higher (22 vs. 20 chunks/s).
- With the event-driven loop instead of polling every millisecond (6 players,
  2 workers, 3 runs of 2 phases each), the game loop wakes up about 35 instead of
  about 540 times a second. The worker on the game loop's core gets the time it no
  longer spends polling: about 15% more chunks per second (25 vs. 22 on average;
  the runs vary by about as much), with the same 20 TPS. The longest stalls are
  still the storage reads (see the [roadmap](docs/ROADMAP.md#next-up)).

`/workers N` changes the pool size at runtime (0 runs everything on the game loop).
`/lag` shows what the slowest recent loop iteration spent its time on.

### Scheduled ticks

Everything that should happen at a later tick goes into one hierarchical timing wheel
([`lib/mcore/src/mc/timer_wheel.h`](lib/mcore/src/mc/timer_wheel.h)) keyed by the
world age: scheduled block ticks (fluids, buttons, fire), furnaces and mob timers
(creeper fuses, despawning items, dead mobs, fleeing and wandering). Scheduling and
cancelling cost O(1), each tick only takes its own bucket, and events far in the
future sit in coarser levels until they come close. Within a tick, events run by
priority and then in the order they were scheduled, like vanilla's scheduled ticks,
and a block can only have one pending tick (an O(1) check).

Furnaces are not ticked at all: their state is brought up to date when someone looks
or clicks, and the wheel wakes them for the next item or when the fuel runs out. So
idle and busy furnaces cost nothing between those events, and there is no limit on
how many can burn at once. Hunger, regeneration and other per-player state stay in
the player tick.

## PC build and tests

```sh
make -C host test                         # unit tests (77 tests)
make -C host server                       # PC server: host/build/mcserver --help
host/build/mcserver --nbd 127.0.0.1:10809 # the same server, e.g. against tools/nbd_server.py
make -C host SAN=1 test                   # AddressSanitizer + UndefinedBehaviorSanitizer
make -C host TSAN=1 test                  # ThreadSanitizer (workers vs. game loop)

cd test && npm install                    # end-to-end tests with mineflayer bots
node run_all.js                           # smoke, gameplay, persistence, mobs and load
NBD_IMPL=nbdkit node persistence.js       # persistence against nbdkit (or qemu-nbd)
node qemu_load.js                         # load test of the firmware in QEMU
node record_gif.js                        # the GIF above (prismarine-viewer + headless Chromium)
```

`tools/gen_data.js` regenerates the registries (`lib/mcore/src/mc/data/`) from
minecraft-data. `tools/fetch_vanilla.sh` downloads the vanilla server jar and
extracts the data-pack tags that `gen_data.js` turns into the Tags packet.

## Compared with vanilla 1.16.5

✅ like vanilla · 🟡 partly or simplified · ❌ missing. [docs/ROADMAP.md](docs/ROADMAP.md)
describes what the bigger gaps (Redstone, the Nether, ...) would take.

**Protocol and server**

| | | |
|---|---|---|
| Clients | ✅ | 1.16.4 / 1.16.5 (protocol 754); server list with MOTD, player count and icon; compression |
| Authentication | ❌ | offline mode only: no Mojang login, encryption or skins. Names are not verified, so the whitelist and the operator list only keep out people who do not know a listed name: run the server on a trusted network |
| Players, view | 🟡 | up to 10 players (ESP32-S3) or 8 (WROVER); view distance up to 8 chunks (vanilla: 32); the 3 chunks around each player stay loaded and crops grow only there, fluids flow in any chunk still in memory |
| World size | 🟡 | world border 64 chunks (1024 blocks) from the centre by default (`MC_WORLD_RADIUS`), shrunk to fit the NBD export (2 GiB fits 63 chunks; vanilla: 30 million blocks); height 0-255 as in vanilla |
| Settings | 🟡 | set at build time in `include/config.h` (the PC server takes command-line options); no `server.properties` |
| Administration | 🟡 | operators and whitelist from the config; `/op` and `/deop` change an online player until they reconnect (not saved); `/kick`, `/save-all`, `/stop`; no `/whitelist`, bans, spawn protection, gamerules, RCON, query or resource packs |
| Movement checks | 🟡 | digging time, reach and a teleport back after huge jumps; no flying, noclip or speed checks, so a modified client can fly in survival |
| Chat | ✅ | chat, `/msg`, `/me`, `/say`, join, leave and death messages (simplified), vanilla's spam limit; no `/tellraw` |
| Mods | ❌ | no data packs or plugins |

**World generation**

| | | |
|---|---|---|
| Terrain | 🟡 | its own seeded generator: the same seed gives the same world with the same build, but not the world vanilla generates for that seed. Identical worlds on the ESP32 and the PC are not guaranteed yet ([roadmap](docs/ROADMAP.md#next-up)) |
| Biomes | 🟡 | 25 of the 68 overworld biomes |
| Caves, ores, plants | 🟡 | noise caves and caverns, ores, six tree types (small forms only: no 2x2 dark oak, jungle or spruce trees, no large oaks), grass, ferns, flowers, cactus, sugar cane, pumpkins, snow and ice; no ravines, lakes, springs, dungeons, mushrooms, kelp, seagrass, coral, vines, bamboo, ... |
| Structures | ❌ | no villages, mineshafts, strongholds, temples, monuments, ... |
| Dimensions | ❌ | overworld only: no Nether, no End |
| Vanilla worlds | ❌ | cannot import or export Anvil (region file) worlds |

**Blocks and world simulation**

| | | |
|---|---|---|
| Placing and breaking | 🟡 | block states and shapes (stairs, fences, walls, chests, ...), survival digging times, tool tiers, drops; doors always get the same hinge (no double doors); fences and panes do not connect to glass and some other full blocks |
| Lighting | 🟡 | sky and block light, but block light stops at chunk borders and sky light crosses them only from the neighbours' open-sky columns. Light is per block, not per state: unlit furnaces and redstone ore glow, while lanterns, soul torches, campfires, shroomlights and magma blocks give no light. Some opaque blocks (furnaces, barrels, pumpkins, melons, TNT, glowstone, ...) let light through, and slabs and stairs do not shade |
| Fluids | 🟡 | water and lava flow, sources, lava + water makes obsidian or cobblestone; simplified |
| Gravity | 🟡 | sand, gravel, concrete powder and anvils fall; concrete powder never hardens in water, falling anvils do no damage |
| Growth | 🟡 | wheat, carrots, potatoes, beetroots, sugar cane, cactus and grass grow, saplings grow into simple trees; growth ignores light and water, and farmland never dries; melon and pumpkin stems, sweet berries, cocoa, bamboo, kelp and vines never grow; no leaf decay, fire spread, or snow and ice in cold weather |
| Redstone | ❌ | levers and buttons only switch themselves, repeaters and comparators only change their setting: nothing carries power; no pistons, observers, hoppers, droppers, dispensers or rails |
| TNT, explosions | 🟡 | TNT explodes as soon as it is lit (no fuse, no chain reactions); explosions (TNT, creepers) damage players, mobs and terrain and ignore blast resistance: only bedrock, obsidian and fluids survive |
| Block entities | 🟡 | chests, barrels, furnaces, smokers and blast furnaces, signs; no hoppers, brewing stands, enchanting tables, beacons, shulker boxes, banners, spawners, lecterns, ... |

**Items**

| | | |
|---|---|---|
| Crafting | 🟡 | the vanilla crafting recipes in 2x2 and 3x3 grids, but each slot takes one exact item (no mixing plank or wood types); no special recipes (dyeing, fireworks, banners, copying maps and books, repairing tools in the grid); no recipe book |
| Smelting | 🟡 | 34 recipes plus logs and wood to charcoal (no glazed terracotta, cracked bricks or nuggets); about half the vanilla fuels (no stairs, doors, signs, ladders, bows, ...); smokers and blast furnaces smelt everything twice as fast; no XP from smelting |
| Item data (NBT) | ❌ | items are id, count and damage only: no enchantments, potions, custom names, books, dyed armour, banners or fireworks |
| Workstations | 🟡 | crafting table, furnace, smoker, blast furnace; no enchanting table, anvil, grindstone, smithing table, brewing stand, stonecutter, loom, cartography table |
| Tools and gear | 🟡 | tools, armour, durability, bows, buckets, food, shears, hoes, bone meal, flint and steel; no crossbow, trident, shield, elytra, totem, fishing rod, potions, ender pearls, snowballs, eggs |

**Entities**

| | | |
|---|---|---|
| Mobs | 🟡 | 8 of the 70 mob types behave like vanilla's: cows, pigs, sheep (shearing), chickens, zombies, skeletons, spiders, creepers. Spawn eggs and `/summon` create the others too, but they only wander (no attacks, no loot). Hostile mobs burn in daylight |
| Spawning | 🟡 | on the surface only, 24-48 blocks from a player, by time of day: hostile mobs at night whatever the light level (torches do not prevent them, caves stay empty), passive mobs on grass by day; natural spawning stops at 24 mobs; mobs despawn beyond 96 blocks ([roadmap](docs/ROADMAP.md#mob-spawning-by-light-level)) |
| AI | 🟡 | chasing, fleeing and wandering without path finding ([roadmap](docs/ROADMAP.md#path-finding-on-the-workers)); no breeding, taming, riding or villager trading |
| Other entities | 🟡 | dropped items, arrows and falling blocks; at most 128 entities in all (96 on WROVER): dropped items do not merge, and drops beyond the limit are lost; no experience orbs (XP is credited directly), paintings, item frames, armour stands, boats or minecarts |
| Status effects | ❌ | no potion effects; golden apples only heal |
| Saving | ❌ | mobs and dropped items are not saved: they vanish when their chunk unloads or the server restarts |

**Players and gameplay**

| | | |
|---|---|---|
| Survival | 🟡 | game modes, health, hunger, saturation, fall damage, drowning, fire and lava, death and respawn; experience (lost on death, not dropped); beds set the spawn point, and one player using a bed at night skips it for everyone at once (nobody lies down; [roadmap](docs/ROADMAP.md#sleeping-only-when-everyone-is-in-bed)) |
| Combat | 🟡 | melee with attack cooldown and critical hits, armour, bows, PvP; no sweep attacks, armour toughness is ignored, fists, hoes and some axes use the wrong attack speed |
| Difficulty | 🟡 | peaceful, easy, normal and hard affect spawning, mob damage, hunger and starvation; `/difficulty` is not saved; no hardcore mode or regional difficulty |
| Weather, time | 🟡 | day and night, a natural rain cycle; thunder only with `/weather thunder`; rain and thunder are visual only (no lightning) |
| Commands | 🟡 | 37 commands including aliases (see [Features](#features)); no target selectors except `@s`, no `/execute`, `/gamerule`, `/effect`, `/enchant`, `/tellraw`, `/title`, `/scoreboard`, `/locate` |
| Progress | ❌ | no advancements, statistics, scoreboards, teams, boss bars or maps |
| Saving | 🟡 | changed chunks, players (position, inventory, health, experience, spawn point) and world data, on any NBD server in its own format; scheduled block ticks are saved with their chunk; mobs, items, operator changes and the difficulty are not saved |

## License

GPL-3.0, like the original project (see [LICENSE](LICENSE)).
