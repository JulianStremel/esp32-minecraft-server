# Roadmap: what full vanilla parity would take

The README's [comparison with vanilla 1.16.5](../README.md#compared-with-vanilla-1165)
lists what is missing. This page explains the big items: what they need from the
code base, what they cost on an ESP32, and how parity could be verified.

## Next up

- **Hardened generator.** The generator is already a pure function of seed and
  coordinates. It should also produce bit-identical worlds on the ESP32 and the PC,
  regardless of compiler floating-point choices or the order chunks generate in, and
  tests should prove it.
- **Lighting across chunk borders.** Today block light stops at the chunk edge. A
  chunk's light should be computed from its neighbours' blocks within 15 blocks of
  the border, and the neighbours' light should be resent when an edit near the
  border changes it.

## Building blocks several features need

| Building block | Needed by | Today |
|---|---|---|
| Item data (NBT on `ItemStack`) | enchanting, potions, brewing, books, banners, fireworks, shulker boxes, named items | items are id + count + damage |
| Per-block behaviour table (`neighborChanged`, `updateShape`, power queries, scheduled tick handlers) | Redstone, portals, most block mechanics | `Server::updateNeighbors` is a fixed work list for fluids, support and gravity |
| Persistent scheduled ticks with priorities | Redstone (repeaters, comparators), fluids across restarts | a 512-entry array, no priorities, not saved |
| Saved entities | mobs, item frames, armour stands, minecarts surviving restarts | only block entities (chests, signs, ...) are saved |
| Several dimensions | Nether, End | one `World`, one generator, one storage area |
| Vehicles (riding, `SetPassengers`, `VehicleMove`, `SteerVehicle`) | boats, minecarts, horses, striders | not handled |
| Path finding | most mob behaviour, villagers | mobs steer straight at their target |

## Redstone

**Foundation.** Vanilla Redstone is defined by how blocks react to updates, so the
first step is a behaviour table indexed by block id (vanilla's `Block` classes), which
the existing fluid, support and gravity rules move into as well. It needs:

- two kinds of updates: neighbour updates (`neighborChanged`, sent to the six
  neighbours in vanilla's order: west, east, down, up, north, south) and shape updates
  (`updateShape`), which are separate in vanilla
- power queries per side (`getWeakPower`, `getStrongPower`) and strong versus weak
  powering through solid blocks
- scheduled ticks ordered by due tick, then priority (repeaters use −3 to −1), then
  insertion order. They need to be stored per chunk, so they survive unloading and
  restarts. That means a chunk record format version bump.
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
- Vanilla has no update budget, and adding one would break parity.

**Rough size:** tier 1 about 2000 lines plus the behaviour-table refactor, tier 2 about
1000, pistons about 1500, hoppers and dispensers about 1000, minecarts about 1500.

## The Nether

**Protocol.**
- `tools/gen_data.js` deliberately keeps only the overworld dimension type and the
  overworld biomes (`KEEP_BIOMES`) in the dimension codec sent at login. The Nether
  needs `minecraft:the_nether` (no sky light, ceiling, ultrawarm, coordinate scale 8,
  logical height 128) and its five biomes.
- Changing dimension is a `Respawn` packet followed by resending chunks and entities.
- Light packets must not carry sky light in the Nether.

**Several worlds.**
- Each dimension needs its own `World`, generator and chunk cache, sharing the PSRAM
  chunk budget.
- Players and entities get a dimension. About 77 places in `lib/mcore/src/mc/server`
  use the single `world` and must become dimension-aware.
- Entity tracking, `broadcastNear`, chunk pinning, mob spawning and the background
  chunk jobs all filter by dimension.

**Storage.**
- World format version 2 gets one chunk area per dimension, listed in the superblock.
  The Nether's border is about an eighth of the overworld's, so its area is small.
- Player records store the dimension.
- Version 1 worlds keep their area as the overworld.

**Generator.**
- 3D density noise with a ceiling (unlike the overworld's height map), a lava sea at
  y = 31, bedrock floor and ceiling.
- Netherrack, soul sand and soul soil, basalt and blackstone.
- Crimson and warped nylium with huge fungi, roots and vines.
- Quartz, Nether gold and ancient debris; glowstone; fire.
- 3D noise costs several times the overworld's per chunk. `tools/qemu/run.sh --bench`
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

## Verifying parity

The vanilla server jar that `tools/fetch_vanilla.sh` already downloads (for the data
reports) can serve as a reference. Build the same structure in both servers, feed both
the same inputs (block changes, ticks, player actions), and let mineflayer compare
block states tick by tick. That catches update-order differences that are hard to spot
by reading code.
