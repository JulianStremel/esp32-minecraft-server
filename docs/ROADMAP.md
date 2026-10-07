# Roadmap: what full vanilla parity would take

The README's [comparison with vanilla 1.16.5](../README.md#compared-with-vanilla-1165)
lists what is missing. This page explains what the items need from the code base,
what they cost on an ESP32, and how parity could be verified. It is ordered from what
comes next to the long-term goal of a server on which the game can be beaten.

## Next up

In this order:

1. **Lighting across chunk borders.** Today block light stops at the chunk edge, and
   sky light only enters from the neighbours' open-sky columns. A chunk's light
   should be computed from its neighbours' blocks within 15 blocks of the border,
   and the neighbours' light should be resent when an edit near the border changes
   it.
2. **Storage I/O on its own thread — implemented.** A bounded queue now moves
   chunk reads/writes, login records, autosaves and dirty eviction off the game
   loop. One thread owns the NBD connection, including reconnects. Writes retain
   their chunk pins until acknowledged; errors leave edits dirty. `/save-all`
   reports completion after the flush. See [measurements and remaining blocking
   compatibility calls](STORAGE_IO.md). Explicit synchronous world operations
   (such as editing an unloaded chunk) still wait for that thread.
3. **Unbounded world storage.** Today every chunk inside the border has two fixed
   64 KiB slots, so the export size caps the world (2 GiB fits a radius of 63
   chunks). Instead:
   - Slots are allocated when a chunk is first saved (a bump allocator; chunks are
     never deleted), keeping the record format and the two copies per chunk.
   - A region directory in RAM, about 16 B per 32 × 32-chunk region that has a saved
     chunk, persisted as an append-only log with a CRC per entry. 1 MB of PSRAM covers
     about 65 000 regions.
   - A 4 KiB slot map per region on the device (A/B copies with a sequence number and
     CRC) and an LRU cache of about 40 maps in PSRAM.
   - An unknown region or an empty map entry means "never saved": generate it, with
     no device read. Batched loads fetch the missing maps in one round trip and the
     records in the next.
   - Writes go slot, record, map, directory, so a power cut leaves the old copy or
     nothing, never garbage.
   - The world border becomes its own setting (up to vanilla's 29 999 984 blocks). A
     full export makes saves fail without crashing: the chunk stays resident and
     dirty, with a warning in the log and in `/storage`.
   - Version 1 worlds are converted when opened.
   - The dimension can become part of the region key, which gives each dimension its
     own storage.

## Gameplay gaps on the way

Smaller items that players notice early. Each fits the existing structure (game loop,
worker jobs on snapshots, timer wheel).

### Mob spawning by light level

**Today:** every 100 ticks, each player gets one spawn attempt at a spot 24 to 48
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

### Path finding on the workers

**Today:** mobs steer straight at their target or wander toward a random point; they
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

## Building blocks several features need

| Building block | Needed by | Today |
|---|---|---|
| Item data (NBT on `ItemStack`) | enchanting, potions, brewing, books, banners, fireworks, shulker boxes, named items | items are id + count + damage |
| Per-block behaviour table (`neighborChanged`, `updateShape`, power queries, scheduled tick handlers) | Redstone, portals, most block mechanics | `Server::updateNeighbors` is a hard-coded work list (a 64-entry queue, at most 256 steps per change) for fluids, support, gravity and connection shapes (fences, panes, walls, stairs, chests, snowy grass) |
| Persistent scheduled ticks with priorities | Redstone (repeaters, comparators), fluids across restarts | done: a timer wheel ordered by tick, priority and insertion; pending block ticks are saved with their chunk |
| Light emission per block state | redstone lamps and torches, lit furnaces | `BlockDef::emitLight` is per block: an unlit redstone lamp emits 15, and toggling `lit` does not re-light the chunk |
| Saved entities | mobs, item frames, armour stands, minecarts surviving restarts | only block entities (chests, signs, ...) are saved |
| Several dimensions | Nether, End | one `World`, one generator, one storage area |
| Vehicles (riding, `SetPassengers`, `VehicleMove`, `SteerVehicle`) | boats, minecarts, horses, striders | not handled |
| Path finding | most mob behaviour, villagers | mobs steer straight at their target (see [above](#path-finding-on-the-workers)) |
| Explosions with blast resistance | TNT, creepers, beds and respawn anchors outside their dimension | a random sphere that ignores blast resistance and never sets fire (see the [long-term goal](#long-term-goal-beating-the-game)) |

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

**Protocol.**
- `tools/gen_data.js` deliberately keeps only the overworld dimension type and a list
  of 29 biomes (`KEEP_BIOMES`: 28 overworld biomes and `the_void`) in the dimension
  codec sent at login. The Nether
  needs `minecraft:the_nether` (no sky light, ceiling, ultrawarm, coordinate scale 8,
  logical height 128) and its five biomes.
- Changing dimension is a `Respawn` packet followed by resending chunks and entities.
- Light packets must not carry sky light in the Nether.

**Several worlds.**
- Each dimension needs its own `World`, generator and chunk cache, sharing the PSRAM
  chunk budget.
- Players and entities get a dimension. About 80 direct uses of the single `world`
  in `lib/mcore/src/mc/server`, plus more than 100 calls of the `blockAt`,
  `setBlock` and `breakBlock` wrappers that assume it, must become dimension-aware.
- Entity tracking, `broadcastNear`, chunk pinning, mob spawning and the background
  chunk jobs all filter by dimension.

**Storage.**
- With the unbounded world storage (next up), the dimension becomes part of the
  region key, so each dimension stores only the chunks that were saved in it.
- Player records store the dimension.
- Existing worlds become the overworld.

**Generator.**
- 3D density noise with a ceiling (unlike the overworld's height map), a lava sea at
  y = 31, bedrock floor and ceiling.
- Netherrack, soul sand and soul soil, basalt and blackstone.
- Crimson and warped nylium with huge fungi, roots and vines.
- Quartz, Nether gold and ancient debris; glowstone; fire.
- 3D noise costs several times the overworld's per chunk. `tools/emulator/run.sh --bench`
  would show how much, and the worker threads absorb it.
- Fortresses, bastions and ruined portals would come later. Bastions are large jigsaw
  structures.

**Portals.**
- Detecting obsidian frames (2×3 up to 21×21) and lighting them.
- Breaking the portal when its frame breaks, which needs the neighbour-update
  dispatch above.
- 80 ticks standing in the portal in survival.
- Coordinates scale ÷8 and ×8.
- Linking portals needs a saved index of portal locations (vanilla's points of
  interest), searched within 128 blocks (16 in the Nether), or a new portal is built.

**Mechanics.** Water evaporates; lava flows faster and further; beds explode; respawn
anchors; fire on netherrack burns forever.

**Mobs.**
- Zombified piglins, which anger as a group.
- Piglins: bartering, gold armour.
- Hoglins.
- Striders, which can be ridden, so this needs vehicles.
- Ghasts and blazes: flying, plus fireballs as new projectiles.
- Magma cubes, which split.
- Wither skeletons.
- Spawning rules per biome.

**Unlocks.** Brewing (blaze powder, Nether wart), which also needs item data for
potions, and netherite through the smithing table.

**Rough size:**
- multi-dimension plumbing: about 1000 lines, spread wide
- storage version 2: about 300
- generator without structures: about 800
- portals: about 600
- mobs: 1500 or more

The End reuses the same plumbing.

## Long-term goal: beating the game

Beating Minecraft in the classic sense means killing the Ender Dragon and taking the
exit portal to the credits. The overworld steps already work: crafting, smelting,
diamond tools, buckets, obsidian from water and lava, and flint and steel.

What is missing, in the order a player meets it:

| Step | Needed | Today |
|---|---|---|
| Nether portal | several dimensions, the Nether generator, portals (see [The Nether](#the-nether)) | ❌ |
| Blaze rods | fortress structure, spawner block entity, blazes (flying; small fireballs that do 5 damage, set the target on fire for 5 s and set fire next to the block they hit) | ❌ |
| Ender pearls | endermen (teleporting; angered by a player looking at them for 5 ticks, unless the player wears a carved pumpkin), thrown pearls that teleport the thrower and deal 5 damage (5% chance of an endermite); optionally piglin bartering | ❌ |
| Eyes of ender | the recipe already exists; an eye entity that flies toward the nearest stronghold and breaks 20% of the time | ❌ |
| Stronghold | generation (at least the staircase and the portal room), 12 end portal frames that take eyes (each starts with an eye 10% of the time), and the portal once all 12 are filled | ❌ |
| The End | End generator: main island, 10 obsidian pillars on a ring of radius 42, 76 to 103 blocks high (the second and third shortest caged in iron bars) with end crystals, exit portal | ❌ |
| Dragon fight | the dragon, end crystals (healing, power-6 explosion), boss bar | ❌ |
| Winning | dragon egg (first kill only), exit portal, credits (Game State Change event 4, value 1), respawn in the Overworld | ❌ |

None of these steps need item data (NBT): blaze rods, pearls and eyes are plain items.

**The Ender Dragon.**
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
  Game Design"). Once dimensions exist this is about 30 lines.
- It needs explosion parity first. Today explosions ignore blast resistance and never
  set fire. Vanilla casts 1352 rays (the surface of a 16 × 16 × 16 grid) of random
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
  up to 112 blocks. That is beyond today's default border of 1024 blocks. This needs
  the unbounded world storage ([next up](#next-up)) or strongholds placed closer.
  With it, the dimension can become part of the region key, so each dimension gets
  its own storage.
- *Generator.* Structures span several chunks, so every chunk must find the structure
  pieces that overlap it from the seed alone. The generator is already a pure,
  bit-identical function of seed and coordinates, which this builds on.
- *Timer wheel (done).* Portal timers, spawner delays, fire spread and the dragon's
  death sequence all run on it.
- *Per-block behaviour table* (see the building blocks above). A portal must break
  when its frame breaks.
- *Protocol.* The dimension codec needs the Nether and End dimension types and their
  biomes. Changing dimension uses the Respawn packet, and the dragon fight uses the
  boss-bar packet.

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
