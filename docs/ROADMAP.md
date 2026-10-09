# Roadmap: what full vanilla parity would take

The README's [comparison with vanilla 1.21.8](../README.md#compared-with-vanilla-1218)
lists what is missing. This page explains what the items need from the code base,
what they cost on an ESP32, and how parity could be verified. It is ordered from what
comes next to the long-term goal of a server on which the game can be beaten.

## Next up

In this order:

1. **Vehicles, then boats** ([below](#vehicles-boats-and-minecarts)): riding (getting
   in and out, the passenger packets, the client's steering and vehicle movement), then
   boats with vanilla's physics on water and ice, chest boats and rafts.
2. **Rails and minecarts** ([below](#vehicles-boats-and-minecarts)): rail shapes,
   powered, detector and activator rails, the classic minecart movement, and the chest,
   hopper, TNT and furnace variants.
3. **The server console on the dashboard** ([below](#server-control-from-the-dashboard)):
   the console output streamed to the page, and commands typed on it, behind the
   dashboard login (item 12).
4. **Start, stop and pause, saves to choose from** ([below](#server-control-from-the-dashboard)):
   stop with a snapshot of the game state that the next start resumes, pause that
   freezes the loop and the workers, and while stopped a full web interface to browse
   the storage and switch between saves, each with its own snapshot.
5. **The SD card, next steps** (it runs on the board now, as fast as NBD; see the
   README): 4-bit SDMMC on boards that wire it, the write cache's line size for the
   remaining small writes (16 KiB writes more bytes, 4 to 8 KiB fewer), and a world
   larger than FAT32's 4 GB file (two files, or a partition of its own).
6. **The rest of 1.21.8** (the protocol itself is done, see
   [MIGRATION_1_21_8.md](MIGRATION_1_21_8.md)): the remaining 1.17-1.21 blocks:
   pointed dripstone (shapes, falling, dripping into cauldrons), powder snow (sinking,
   freezing), lightning (it cleans copper; thunderstorms have no bolts yet), moss and
   azaleas, dripleaves, glow lichen, mud, brushing suspicious sand, sniffers, frogspawn,
   trial spawners and vaults. Done: chunk batches (the client's pace), the world height
   −64..319 (storage format 6, see [MIGRATION_1_21_8.md](MIGRATION_1_21_8.md#the-world-height-done)),
   the operator menu as dialogs, and copper (oxidation, waxing, scraping), candles,
   candle cakes and amethyst growth (`newer_blocks.cpp`).
7. **Vanilla terrain generation**: researched and prototyped in
   [TERRAIN_GENERATION.md](TERRAIN_GENERATION.md) (the 1.21.8 noise router compiled to
   C, 300-350 ms per chunk on the board, ~100 ms targeted); eight phases from the terrain
   shape to structures.
8. **Sleeping only when everyone is in bed** ([below](#sleeping-only-when-everyone-is-in-bed)),
   about 150 lines.
9. **Explosion parity** (blast resistance, fire, TNT fuse and chain reactions; see
   [bed explosions](#long-term-goal-beating-the-game)), about 250 lines.
10. **Saved entities** (mobs and dropped items survive restarts and unloading).
11. **Redstone loop time** (backlog, reported from play): a running circuit raises the
   board's loop time from about 10 ms to about 20 ms per tick. To measure first with
   `/lag` and a profile of a clock driving dust, repeaters and a piston; likely
   candidates are the per-change neighbour updates (six `blockAt` lookups each, through
   the chunk hash), wire power recalculation over the whole dust network, and a
   `BlockChange` packet per block where a `MultiBlockChange` per section would do.
12. **The status dashboard, next steps** (the read-only version is done, see below):
   a login (a token from `config.h`), then actions (kick, save, the operator menu's
   settings) as requests answered on the game loop; a history kept on the board (a
   ring of the last few minutes in PSRAM) so a newly opened page has its graphs at
   once; the storage figures as numbers rather than a status line; the dashboard in
   the web flasher's firmware.

### Done recently

1. **Lighting across chunk borders — implemented near players.** Within
   `exactLightDistance` (2 chunks) of a player, a chunk's light is computed from its
   neighbours' blocks within 14 blocks of the border (`ChunkLight::computeRegion`, on
   shared chunk snapshots), and edits near a border resend the neighbours' light.
   Farther chunks keep per-chunk light. Exact light costs 38–140 ms per chunk on the
   S3 (a 44×44 grid in PSRAM), so one such job runs at a time; making it cheaper (for
   example caching each snapshot's decoded filter grid for the 9 regions that use it)
   would allow a larger distance.
2. **Storage I/O on its own thread — implemented.** A bounded queue now moves
   chunk reads/writes, login records, autosaves and dirty eviction off the game
   loop. One thread owns the NBD connection, including reconnects. Writes retain
   their chunk pins until acknowledged; errors leave edits dirty. `/save-all`
   reports completion after the flush. See [measurements and remaining blocking
   compatibility calls](STORAGE_IO.md). Explicit synchronous world operations
   (such as editing an unloaded chunk) still wait for that thread.
3. **Unbounded world storage — implemented** (format 3, `lib/mcore/src/mc/storage/region_index.cpp`):
   slot pairs allocated when a chunk is first saved, a region directory (append-only
   log with a CRC per entry, replayed into RAM), A/B slot maps per 32 x 32 region with
   an LRU cache of 40, an allocation watermark logged before it is used, writes in the
   order record, map, directory. The world border is a setting up to vanilla's. The
   region key includes the dimension. Format 4 (with the redstone work) added item
   metadata; worlds in older formats are not converted but replaced by a new world.
   Not yet: reclaiming space (nothing is freed).
4. **The Nether and the End** (dimensions, portals, Nether mobs, the dragon fight):
   see [The Nether](#the-nether) and the [long-term goal](#long-term-goal-beating-the-game).
5. **Operator menu** (`/menu`, `menu.cpp`): statistics, settings, players, dimensions,
   the dragon fight, and a world reset with a new seed (the storage formats a new world,
   the server restarts).
6. **A web flasher** (`web/`, GitHub Pages built by `.github/workflows/pages.yml`)
   with WiFi set up over Improv Serial after flashing.
7. **World storage on a microSD card** (see item 1 above): a contiguous file on a
   FAT32 card (exFAT is not available in ESP-IDF 5.5) behind a write-back cache.
8. **A status dashboard served by the board** (build flag `MC_DASHBOARD`, see the
   README's [Status dashboard](../README.md#status-dashboard)): a read-only page with
   TPS, memory, chunks, entities, the world and the players online, pushed once a
   second as Server-Sent Events. The game loop only copies values into a snapshot
   (0.2 ms, in a pass with time before the next tick), a background worker job
   formats the JSON (1.1 ms), no task of its own; in a session with a player loading
   terrain and a page open the loop's figures were those of the build without it.
9. **Fixes found in play:** mobs jump a full block (they stalled at single-block
   steps), no fall damage after leaving water or on a gamemode change, items in water
   bob at vanilla's pace (the decompiled `ItemEntity` physics).

## Gameplay gaps on the way

Smaller items that players notice early. Each fits the existing structure (game loop,
worker jobs on snapshots, timer wheel).

### Mob spawning by light level — implemented

`lib/mcore/src/mc/server/spawning.cpp`: the game loop walks vanilla's spawn packs over
the live chunk with block checks only; a background job computes the chunk's light from
a snapshot and applies `Monster#isDarkEnoughToSpawn` / `Animal#checkAnimalSpawnRules`;
the game loop spawns what passed after checking again. Simplifications are listed in the
README. The text below is the original plan.

**Before:** every 100 ticks, each player gets one spawn attempt at a spot 24 to 48
blocks away, always on the surface. Hostile mobs spawn at night whatever the light,
passive mobs on grass by day. Torches do not stop spawns, and caves and dark
buildings stay empty.

**Vanilla 1.16.5** (`NaturalSpawner`):
- Every tick, for every loaded chunk near a player and every category below its cap,
  it picks a random column in the chunk and a random height from 0 to the surface + 1.
- From there it tries 3 packs of up to 4 mobs, each step moving up to 5 blocks.
- Nothing spawns within 24 blocks of a player.
- Caps per 17 × 17 chunks around the players: monsters 70, creatures 10, ambient 15,
  water creatures 5, water ambient 20.
- Hostile mobs need darkness: the sky light must not exceed a random 0..31, and the
  combined light must not exceed a random 0..7. So caves spawn mobs, torches stop
  them, and day and night follow from the sky light.

**What it needs:** the light values at the candidate spots. The light engine runs in
worker jobs and keeps no light in memory (64 KB per chunk, too much for 200 resident
chunks). So spawn attempts become worker jobs over chunk snapshots: a job computes
the light of a snapshot, picks the candidate positions and returns them, and the game
loop checks the caps and spawns. About 300 lines.

### Sleeping only when everyone is in bed

**Today:** one player using a bed at night skips the night for everyone at once, and
nobody lies down.

**Vanilla 1.16.5:**
- The player lies down: pose `SLEEPING` and the bed position in the entity metadata;
  the client shows the sleep screen and can leave the bed.
- The night passes only when every player who is not a spectator has been asleep for
  at least 100 ticks (5 s). Then the time jumps to the next morning (the next
  multiple of 24000), the rain stops and everyone wakes up.
- Sleeping is refused during the day, with monsters within 8 blocks horizontally and
  5 vertically of the bed (unless in creative), when the bed is too far away or
  obstructed, and outside the Overworld (where the bed explodes, see the long-term
  goal below).
- 1.16 has no percentage setting; `playersSleepingPercentage` came with 1.17.

About 150 lines.

### Path finding on the workers — first version implemented

`lib/mcore/src/mc/server/path.cpp` (A* over 3 x 3 chunk snapshots: step up 1, drops up
to 3, diagonals without cutting corners, lava/fire/cactus avoided) and `mob_paths.cpp`
(a PathJob per request, at most 2 in flight; chasing zombies, spiders and creepers follow
the waypoints and ask again when the target moves away from the path's end, a chunk on
the way changes or they get stuck). Not yet: wandering along paths, doors, water as a
separate node type, vanilla's longer drops while chasing. The text below is the plan.

**Before:** mobs steer straight at their target or wander toward a random point; they
get stuck behind walls and walk into holes.

**Plan:** an A* search on the worker threads, on the same kind of snapshots as the
other jobs:
- A path job gets copies of the chunks the search may cross (bounded by the mob's
  follow range, so at most a few chunks). Mobs searching in the same area share the
  snapshots of one tick (reference-counted).
- It returns a list of waypoints; the game loop only follows them.
- The result remembers the version of each chunk it used (`Chunk::version` changes
  with every block change). A path is recomputed only when one of those chunks
  changed, the target moved away from the path's end, or the mob got stuck, not
  every tick.
- Chasing mobs get high priority, wandering ones background priority, so path
  finding never delays the chunks under a player.
- Node types follow vanilla's walk evaluator (open, walkable, fences, doors, water,
  lava and other dangers), so mobs avoid cacti and lava and do not walk off drops
  higher than 3 blocks (more for a mob chasing a target while it has health to spare,
  as in vanilla's `Mob#getMaxFallDistance`).

About 600 lines. Needed before villagers, iron golems or anything that has to
navigate.

## Vehicles: boats and minecarts

Nothing of this exists yet: rails can be placed (they need a block below) but do not
connect or react to power, boat and minecart items do nothing, and the client's
`PlayerInput`, `VehicleMove` and `SteerBoat` packets are ignored. In build order:

1. **Vehicles** (about 400 lines). Entities a player (or a mob) rides: using one gets
   in, sneaking gets out, `SetPassengers` tells everyone. The rider's client moves the
   vehicle (`VehicleMove`), the server checks it like player movement (speed, collision)
   and moves the rider with it; mobs ride where vanilla lets them (a boat they bump into).
   Hitting a vehicle breaks it into its item. Riders keep their own view and chunk
   loading.
2. **Boats** (about 600 lines). Placed on water; vanilla's `Boat#tick`: buoyancy, paddling
   (`SteerBoat`), the friction of water, land and ice (packed and blue ice), sinking out
   of the world below, damage and breaking, falling onto land. Chest boats carry an
   inventory (a container window), rafts are bamboo boats. Two seats.
3. **Rails** (about 500 lines). A rail's shape follows its neighbours when it is placed
   and when they change (`RailState`: straight, curves, slopes up a block); powered and
   activator rails switch with redstone and pass their power along up to 8 rails;
   detector rails give a signal (and a comparator reading of a cart's contents) while a
   cart is on them.
4. **Minecarts** (about 1000 lines). The classic movement (`AbstractMinecart#moveAlongTrack`;
   1.21's new minecart physics is an experiment behind a feature flag, off in vanilla):
   following the rail shape, slopes, powered rails' boost and braking, derailing,
   carts pushing each other and entities. Riding as above. Variants: chest and hopper
   carts (a container; the hopper cart picks up items and feeds hoppers), TNT carts
   (activator rails, falls, fire), furnace carts (fuel pushes).

**Cost.** Carts and boats run on the game loop like mobs and count toward the 128
entities. A dozen should stay well under a millisecond per tick; to measure with a rail
loop and carts on it, as the Redstone loop time item asks for redstone. Until entities
are saved (item 10), vehicles vanish on a restart, like dropped items today.

**Tests.** Host: rail shapes for every placement case, power along powered rails, cart
movement on a straight, a slope and a curve against numbers from vanilla. End to end
(mineflayer can ride: `bot.mount`, `bot.moveVehicle`): a player boards a boat and a cart
and arrives where vanilla would put them; a detector rail lights a lamp.

## Server control from the dashboard

The dashboard (`/api/status`, pushed events) is read-only today. Four steps, the first
one a requirement for the others:

1. **A login** (item 12): nothing that changes the server may be reachable without it,
   since the dashboard is open to everyone on the network. A token from `config.h` (and
   for the prebuilt firmware, set with the WiFi over Improv or on first visit), checked on
   every request, sent only over the LAN (no TLS on the board: noted on the page).
2. **The console on the page.** The log lines the serial console shows (`MC_LOG*` and
   the server's messages) go to a ring buffer in PSRAM (say 64 KiB, a few thousand
   lines) as well; the page gets the backlog when it opens and new lines as pushed
   events. A command field sends lines to the same queue the serial console feeds (run on
   the game loop as console commands, with operator rights). Costs to measure: the
   formatting is already paid for the serial output; the ring copy is a memcpy; pushing
   is batched per event as today.
3. **Start, stop and pause.** States: *running*, *paused*, *stopped*.
   - **Pause** freezes the world: the game loop stops ticking (no time, no mobs, no
     redstone) and the workers take no new jobs, while queued and half-done jobs stay
     where they are; resume continues exactly there. Clients stay connected: the server
     keeps answering keep-alives and tells players the server is paused (a client drops
     after 30 s without them). Pausing is cheap and does not touch the storage.
   - **Stop** ends the game loop and writes a **snapshot**: every dirty chunk (as the
     normal save), the players, and the game state that is lost today: the entities
     (mobs, items, vehicles: the saved-entities format, item 10), scheduled ticks and
     block events, the redstone and fluid queues, weather and time, the dragon fight,
     and the workers' queue. Worker jobs are mostly results computed from the world
     (generated, lit and encoded chunks), so the snapshot keeps *which* work was queued
     (chunk loads, light, sends for players who come back) rather than half-finished
     results, and the next start re-queues it; generated chunks that are finished are
     saved like any chunk. **Start** loads the snapshot and continues as if the server
     had been paused. Players are kicked with "server stopped" and rejoin after the start.
   - The serial console gets the same commands (`pause`, `resume`, `stop`, `start`).
4. **While stopped: the full web interface.** With the game loop and the chunk cache
   gone, the PSRAM is free for a larger page and its work:
   - **Storage browser**: the files of the backend (the SD card's FAT32 directory; for
     NBD the export and the worlds in it), with sizes and free space; upload and download
     of world files; deleting with a confirmation.
   - **Saves**: several worlds side by side (on the SD card `worlds/<name>.img`, on NBD
     one world per export or a directory of worlds in one image), each with its own
     snapshot inside it, so switching to another save never resumes the previous world's
     game state. Create (seed, type, border), rename, duplicate, delete, and choose which
     one the next start loads.
   - Starting the server again from the page.

**Storage format.** The snapshot lives in the world file: a snapshot area next to the
existing world state (format 6), written in two copies with a sequence number and CRC like
the superblock, and marked valid only once complete, so a power cut while stopping leaves
the previous snapshot or none (then the start is a normal start from the saved chunks).

**Tests.** Host: stop and start round trips (entities, scheduled ticks, a redstone clock
mid-cycle, a cart on a rail, the job queue) compare the world tick by tick with a server
that never stopped; switching saves loads each world's own snapshot. End to end: pause
with a bot online (still connected after 60 s, nothing moved), the console on the page
(a command's output arrives), stop, switch saves, start. Board: the time a stop and a
start take with a full chunk cache, and the dashboard's cost in the console stream.

## Building blocks several features need

| Building block | Needed by | Today |
|---|---|---|
| Item data (NBT on `ItemStack`) | enchanting, potions, brewing, books, banners, fireworks, shulker boxes, named items | items are id + count + damage |
| Per-block behaviour table (`neighborChanged`, `updateShape`, power queries, scheduled tick handlers) | Redstone, portals, most block mechanics | `Server::updateNeighbors` is a hard-coded work list (a 64-entry queue, at most 256 steps per change) for fluids, support, gravity and connection shapes (fences, panes, walls, stairs, chests, snowy grass) |
| Persistent scheduled ticks with priorities | Redstone (repeaters, comparators), fluids across restarts | done: a timer wheel ordered by tick, priority and insertion; pending block ticks are saved with their chunk |
| Light emission per block state | redstone lamps and torches, lit furnaces | `BlockDef::emitLight` is per block: an unlit redstone lamp emits 15, and toggling `lit` does not re-light the chunk |
| Saved entities | mobs, item frames, armour stands, minecarts surviving restarts | only block entities (chests, signs, ...) are saved |
| Several dimensions | Nether, End | done: chunks keyed by dimension in one `World`, a generator per dimension, storage regions per dimension |
| Vehicles (riding, `SetPassengers`, `VehicleMove`, `SteerVehicle`) | boats, minecarts, horses, striders | not handled; next up, see [Vehicles](#vehicles-boats-and-minecarts) |
| Path finding | most mob behaviour, villagers | first version: A* on the workers for chasing mobs (see [above](#path-finding-on-the-workers)); wandering mobs still walk straight |
| Explosions with blast resistance | TNT, creepers, beds and respawn anchors outside their dimension | a random sphere that ignores blast resistance; fire is an option (ghast fireballs use it, beds not yet) (see the [long-term goal](#long-term-goal-beating-the-game)) |

## Redstone

**Foundation.** Vanilla Redstone is defined by how blocks react to updates, so the
first step is a behaviour table indexed by block id (vanilla's `Block` classes), which
the existing fluid, support, gravity and shape rules move into as well (dropping
today's cap of 256 steps per change). It needs:

- two kinds of updates: neighbour updates (`neighborChanged`, sent to the six
  neighbours in vanilla's order: west, east, down, up, north, south) and shape updates
  (`updateShape`), which are separate in vanilla
- power queries per side (`getWeakPower`, `getStrongPower`) and strong versus weak
  powering through solid blocks
- scheduled ticks ordered by due tick, then priority (repeaters use −3 to −1), then
  insertion order, stored per chunk so they survive unloading and restarts. The
  timer wheel already does this; Redstone components only add their handlers.
- block events, processed at the end of the tick and sent as `BlockAction` packets
  (pistons, note blocks)
- `MultiBlockChange` packets (unused today) to batch the many block changes a circuit
  makes

**Components, in tiers:**

1. **Power basics.**
   - Redstone wire: power 0 to 15, plus the connection states (side, up, none) the
     client renders.
   - Torches: 2-tick delay, burn out after 8 toggles within 60 ticks.
   - Sources: levers, buttons, pressure plates and tripwire (these need entity
     collision checks), daylight detectors, redstone blocks, target blocks, observers.
   - Consumers: lamps, doors, trapdoors, fence gates, note blocks, TNT, powered rails.
2. **Timing.**
   - Repeaters: delay and locking.
   - Comparators: compare and subtract modes; reading how full containers are, and
     cake, cauldrons, composters.
3. **Moving things.**
   - Pistons and sticky pistons: 12-block push limit, immovable blocks, slime and
     honey sticking, a moving-piston block entity for the 2-tick animation, pushing
     entities.
   - Hoppers: transfer every 8 ticks.
   - Droppers and dispensers: one behaviour per item.
   - Rails and minecarts, which need vehicles.

**Parity details players rely on.** Quasi-connectivity (pistons and dispensers powered
by the block above them), BUD switches and 0-tick tricks fall out of copying vanilla's
update semantics exactly. One catch: in 1.16, wire updates its neighbours in the order
of a Java `HashSet<BlockPos>`, so exact parity for order-dependent contraptions means
reproducing `BlockPos.hashCode` ordering.

**Cost on the ESP32.**
- Redstone has to run on the game loop, because its order is deterministic and it
  changes the world. The worker threads only help indirectly, by absorbing the extra
  light and packet work that lamps and torches cause.
- One wire toggle in vanilla causes hundreds of neighbour updates, at roughly 1 to
  3 µs each through palette lookups. Ordinary clocks and doors cost milliseconds per
  tick; lag machines could drop the TPS.
- Vanilla has no update budget, and keeping one would break parity: today's work
  list stops after 256 steps, so that cap has to go.
- Lamps and torches also need light emission per block state (see the building
  blocks).

**Rough size:** tier 1 about 2000 lines plus the behaviour-table refactor, tier 2 about
1000, pistons about 1500, hoppers and dispensers about 1000, minecarts about 1500.

## The Nether

**Plumbing — implemented** (see the README's [Dimensions](../README.md#dimensions)).
- The dimension codec carries the overworld, `the_nether` and `the_end` types and their
  biomes (`nether_wastes`, `the_end`). Join Game lists the three worlds; a dimension
  change is a `Respawn` packet followed by the view, entities and player state.
- One `World` keyed by (dimension, x, z) shares the PSRAM chunk budget. Players and
  entities have a dimension, and the server's `curDim` (set for a scope by `InDim`) is
  the dimension the `blockAt`/`setBlock`/`breakBlock` wrappers, timers and block
  updates work in. Entity tracking, `broadcastNear`, chunk pinning, mob spawning and
  the chunk jobs filter by dimension.
- Storage regions are keyed by dimension; player records (version 2) keep it.
- No sky light in the Nether or the End (the light engine skips the sky pass).
- `/dimension`, and `nether_portal`/`end_portal` blocks placed in creative or by
  command that move whoever stands in them (the arrival's chunks are loaded on the
  workers first).

**Generator — first version implemented.**
- Done: 3D density noise with a ceiling, a lava sea at y = 31, bedrock floor and
  ceiling; netherrack, soul sand, gravel and magma; quartz and Nether gold; glowstone.
- Missing: soul soil, basalt and blackstone (the other four biomes); crimson and
  warped nylium with huge fungi, roots and vines; ancient debris; fire.
- Fortresses, bastions and ruined portals would come later. Bastions are large jigsaw
  structures.

**Portals — implemented** (`portals.cpp`): frames of 2×3 up to 21×21 lit with flint and
steel, broken with their frame, 80 ticks in survival, coordinates ÷8 and ×8, linking
through a saved list of up to 96 portals searched within 128 blocks (16 in the
Nether), or a new portal built within 16 blocks (or on a platform). Missing: lighting
by fire spread or fire charges, portals found by scanning chunks (vanilla's points of
interest) when they are not in the list, the nausea effect and portal sounds.

**Mechanics.** Done: water evaporates; lava flows faster and further; beds explode.
Missing: respawn anchors; fire on netherrack burns forever.

**Mobs.** Done (`nether_mobs.cpp`): zombified piglins that anger as a group, ghasts
with fireballs (and hitting them back), magma cubes that split; natural spawning with
nether_wastes' weights. Missing:
- Piglins: bartering, gold armour.
- Hoglins.
- Striders, which can be ridden, so this needs vehicles.
- Blazes (flying, small fireballs) and wither skeletons, which come with fortresses.
- Endermen, and the other biomes' spawn lists.

**Unlocks.** Brewing (blaze powder, Nether wart), which also needs item data for
potions, and netherite through the smithing table.

**Rough size of what is left:**
- the other Nether biomes: about 600 lines
- portals: about 600
- mobs: 1500 or more

The End reuses the same plumbing; its generator has the main island and the spikes,
and the dragon fight is implemented (see the [long-term goal](#long-term-goal-beating-the-game)).

## Long-term goal: beating the game

Beating Minecraft in the classic sense means killing the Ender Dragon and taking the
exit portal to the credits. The overworld steps already work: crafting, smelting,
diamond tools, buckets, obsidian from water and lava, and flint and steel.

What is missing, in the order a player meets it:

| Step | Needed | Today |
|---|---|---|
| Nether portal | several dimensions, the Nether generator, portals (see [The Nether](#the-nether)) | ✅ obsidian frames lit with flint and steel, linked portals; the Nether's other biomes and fortresses are missing |
| Blaze rods | fortress structure, spawner block entity, blazes (flying; small fireballs that do 5 damage, set the target on fire for 5 s and set fire next to the block they hit) | ❌ |
| Ender pearls | endermen (teleporting; angered by a player looking at them for 5 ticks, unless the player wears a carved pumpkin), thrown pearls that teleport the thrower and deal 5 damage (5% chance of an endermite); optionally piglin bartering | ❌ |
| Eyes of ender | the recipe already exists; an eye entity that flies toward the nearest stronghold and breaks 20% of the time | ❌ |
| Stronghold | generation (at least the staircase and the portal room), 12 end portal frames that take eyes (each starts with an eye 10% of the time), and the portal once all 12 are filled | ❌ |
| The End | End generator: main island, 10 obsidian pillars on a ring of radius 42, 76 to 103 blocks high (the second and third shortest caged in iron bars) with end crystals, exit portal | ✅ the main island, the spikes (vanilla's for the seed), crystals, the exit portal |
| Dragon fight | the dragon, end crystals (healing, power-6 explosion), boss bar | ✅ simplified phases (`dragon.cpp`) |
| Winning | dragon egg (first kill only), exit portal, credits (Game State Change event 4, value 1), respawn in the Overworld | 🟡 the egg, the exit portal home; no credits |

None of these steps need item data (NBT): blaze rods, pearls and eyes are plain items.

**The Ender Dragon — implemented** (`dragon.cpp`, all of the below in a simpler form;
missing: the hovering phase, block breaking by the dragon, respawning it with four
crystals, end gateways, the credits).
- 8 hitbox parts (head, neck, body, 3 tail segments, 2 wings). On the vanilla server
  their entity ids follow the dragon's (dragon + 1 to + 8), but the client numbers
  them from the dragon's own id (+ 0 to + 7). A click on part *i* therefore arrives
  as id dragon + *i* and lands one part off: a click on the head hits the dragon
  entity itself, which ignores it, and a click on the neck hits the head. Parity
  means keeping that quirk.
- Hits on the head do full damage, hits on other parts damage ÷ 4 + min(damage, 1).
- AI phases: holding pattern (circling the pillars), strafing a player with
  fireballs, landing approach, landing, perching (breath, scanning, attacking),
  charging a player, taking off, hovering, and dying.
- Dying takes 200 ticks. The XP comes out from tick 150 on (12000 for the first kill,
  500 later), then the exit portal opens, and on the first kill the egg appears.
- Dragon breath can start as a simple area that damages over time; vanilla uses a
  lingering cloud of status effects.
- The nearest crystal within 32 blocks heals the dragon by 1 health every 10 ticks.
  Destroying the crystal that is healing it deals 10 damage to the dragon's head.
- The fight state (dragon killed, portal, egg) survives restarts in a small record in
  the world data, because entities are not saved.

**Bed explosions (and respawn anchors).**
- A bed used outside the Overworld explodes with power 5 and sets fire ("Intentional
  Game Design"). Implemented with today's explosions (no blast resistance; the fire
  option exists but beds do not use it yet).
- It needs explosion parity first. Today explosions ignore blast resistance; only
  ghast fireballs set fire. Vanilla casts 1352 rays (the surface of a 16 × 16 × 16 grid) of random
  strength, each weakened by the blast resistance of every block it passes. With fire
  on, every affected spot that is now air and has a solid block below catches fire
  with a chance of 1 in 3.
- Explosion parity is about 250 lines. It also fixes TNT and creepers, and costs an
  estimated 20–50 ms per blast on the ESP32, so chain reactions need a budget.
- A charged respawn anchor explodes the same way outside the Nether, so it could be
  the first playable step without any new dimension.

**Dependencies on other work.**
- *World border.* In 1.16.5 the first 3 strongholds lie 88 to 168 chunks
  (1408–2688 blocks) from the origin, and the biome search can move each one by
  up to 112 blocks. That is beyond the default border of 1024 blocks
  (`MC_WORLD_RADIUS`), which can now be set up to vanilla's (the unbounded storage is
  done), so a world meant for strongholds needs a border of at least about 200
  chunks.
- *Generator.* Structures span several chunks, so every chunk must find the structure
  pieces that overlap it from the seed alone. The generator is already a pure,
  bit-identical function of seed and coordinates, which this builds on.
- *Timer wheel (done).* Portal timers, spawner delays, fire spread and the dragon's
  death sequence all run on it.
- *Per-block behaviour table* (see the building blocks above). Portals break with
  their frame through a check of their own in the neighbour updates; Redstone will
  need the general table.
- *Protocol (done).* The dimension codec has the Nether and End dimension types and
  their biomes; a dimension change is a Respawn packet; the dragon fight has its boss
  bar.

The numbers on this page were checked against the decompiled 1.16.5 server (Mojang's
official mappings); the explosion cost and line counts are estimates.

## Verifying parity

The vanilla server jar that `tools/fetch_vanilla.sh` already downloads (today only to
extract the data-pack tags for the Tags packet) can serve as a reference:

- Build the same structure in both servers, feed both the same inputs (block changes,
  ticks, player actions), and let mineflayer compare block states tick by tick. That
  catches update-order differences that are hard to spot by reading code.
- For constants and rules, decompile the jar with Mojang's official mappings
  (`server_mappings` in the version manifest) and a decompiler such as CFR. The
  numbers in the long-term goal above were checked that way.
