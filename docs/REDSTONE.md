# Java 1.16.5 redstone implementation plan

## Published work-in-progress snapshot (2026-10-08)

Branch: `codex/redstone-1.16`. This snapshot preserves the current implementation;
it is **not ready to merge as full Java 1.16.5 redstone support**.

Implemented: the core signal/timing engine, dust, torches, repeaters, comparators,
observers, switches, buttons, plates, tripwire, targets, trapped chests, daylight
detectors, lamps, powered doors/trapdoors/gates, note blocks, pistons, hoppers,
droppers, initial dispenser actions, books and lecterns. Supporting work includes
item NBT, format-4 inventory persistence, network windows, diagnostics, native
tests, emulator circuit tests and recorded vanilla transition comparisons.
The detailed coverage and limitations are listed below.

Still open:

- Exact nested neighbor/shape callbacks and placement semantics; broader
  update-order, zero-tick and duplication conformance.
- Piston contextual collision, movement failure atomicity and unload/reload
  ordering, including scheduled ticks across chunk boundaries.
- Rails and minecarts, command blocks, item-frame/jukebox comparator providers,
  and composter, brewing-stand, shulker-box and vehicle inventory automation.
- Remaining specialized dispenser actions and NBT-dependent item effects.
- General entity persistence, dynamic book text and vanilla TNT explosion behavior.
- Broader vanilla traces, restart/unload coverage, active-versus-idle performance
  measurements and final firmware/hardware validation.
- Merged into `main` (from `d14b3aa`, with the operator menu, Nether mobs, the dragon
  fight, SD storage and the dashboard); see the validation history below.

Next steps: extend vanilla conformance tests while fixing
the remaining callback behavior, implement the missing components, then repeat
native, sanitizer, network, persistence and S3/P4 validation and performance checks.

## Compatibility target and acceptance plan

The compatibility target includes intermediate updates, tick priorities, pulse
lengths, locking, quasi-connectivity and update-order-dependent contraptions.
Passing steady-state truth tables does not establish compatibility.

Implementation and acceptance sequence:

1. State-dependent light, ordered persisted ticks, directional weak/strong power,
   separate neighbour and shape notifications, observable resource limits.
2. Dust (including dot/cross, stairs and Java position-dependent update order),
   torches/burnout, repeaters/locking, comparators and observers. Compare timed
   transition traces against the official 1.16.5 server.
3. Inputs: switches, buttons/projectiles, all plates, tripwire, daylight detectors,
   targets, trapped chests and lecterns. Outputs: lamps, doors, trapdoors, gates,
   note blocks and primed TNT.
4. Pistons: deferred block events, moving block entities, collision, 12-block
   limit, slime/honey branching, quasi-connectivity and sticky block dropping.
5. Automated inventory: hopper cooldown and sided slots, dropper transfers,
   dispenser item behaviours, comparator notifications. Rail shapes, powered and
   activator rails, detector rails and minecart interactions.
6. Command blocks and remaining comparator providers. Item NBT and books now have
   initial implementations; item frames and vehicle/entity persistence remain
   dependencies. These dependencies are part of the work, not implicit exclusions.
7. Restart/unload tests, cross-dimension isolation, network block-event tests,
   sanitizers and ESP32-S3/P4 builds. Measure dormant and active circuit workloads
   separately, including update counts, timer occupancy, PSRAM and tick latency.

The implementation is in progress. This document is a target and acceptance plan,
not a declaration that all these components or vanilla quirks are implemented.
In particular, deterministic directional order alone is insufficient for dust:
1.16.5 also iterates position hash sets. An ordered work list must reproduce the
observable nesting of updates, not merely process all updates eventually.

Reference artifacts are the official Mojang 1.16.5 server and server mappings,
downloaded under ignored `tools/vanilla/`. Their SHA-1 values are checked against
the Mojang version manifest. Decompiled reference code must remain untracked.

## Current implementation

Implemented and covered by focused tests:

- Directional weak/strong power, connected dust and attenuation, lever/button
  propagation, torch delay and burnout, repeater delay/locking, comparators and
  observer pulses. Existing container changes notify nearby comparators, including
  reads through conducting blocks. Comparator output is persisted separately from
  its powered state. Dust includes the seven-position Java hash-set notification order.
- Lamps, powered doors/trapdoors/gates, deferred note-block events and support-selected
  instruments. Note events check the cover when queued, then use the current note and
  instrument when played; replacement blocks cancel pending events.
- Piston structure planning in all six directions, the 12-block limit, slime/honey
  branching, push-only/immovable blocks, quasi-connectivity, deferred events, sticky
  pulling and short-pulse block dropping. Moving block entities animate over two
  steps, finalize on the following tick, have collision shapes and push/carry entities.
  Live block entities tick in insertion order. Their state and previous progress
  survive chunk storage; full chunk packets include moving-piston NBT.
- Stone/wood/weighted pressure plates, arrow-held wooden buttons, target hit strength
  and timed pulses, trapped-chest viewer power, tripwire attachment/triggering,
  ten-tick release and break pulses, shears disarming and the 40-string maximum span.
- Daylight detectors sample skylight every 20 world ticks, support inversion and
  account for time/weather. Open-sky samples avoid light computation; shaded samples
  share a regional computation per chunk and cache it until light filtering or chunk
  residency changes. `/lag` reports computation count, total time and peak time.
- Lectern book insertion/removal, page windows, two-tick page-turn pulses and
  page-dependent comparator output. Books can be edited and signed; pages and the
  selected lectern page survive storage. Dynamic JSON text resolution still needs
  the command/text-component foundation.
- Primed TNT with an 80-tick fuse, redstone ignition, manual ignition, unstable-block
  breaking and shorter chain fuses. Explosion destruction still uses the existing
  simplified explosion algorithm; vanilla explosion/exposure parity is pending.
- Hopper push/pull, whole-stack item collection, eight-tick transfer cooldowns,
  receiving-hopper tick-order adjustment, power locking, double-container access,
  furnace sided slots, comparator notifications and container windows. Hopper slots
  and cooldowns are saved. Droppers select a nonempty slot and transfer/eject one item
  after four ticks; a full destination does not cause ejection.
- Dispenser windows, four-tick triggering, ordinary item ejection, TNT priming,
  arrow launch, water/lava buckets and flint-and-steel actions. Other specialized
  dispenser actions and NBT-dependent item effects remain to be implemented.

This is **not full 1.16.5 compatibility**. Remaining foundation work includes exact
nested callback sequencing, complete shape/indirect-shape notifications, placement
semantics, unloaded-boundary behavior, loaded block-entity ordering, piston movement
failure atomicity and the remaining piston collision edge cases. The old support,
gravity and fluid neighbour work list still has its original bounds. Static collision
shapes are queried in an empty world at the origin; context-dependent adjustments
still need conformance work. Current tests do not establish zero-tick, duplication or
arbitrary update-order-dependent contraption compatibility.

The remaining dispenser actions, rail/minecart interactions and command blocks
are still open. General entity persistence and dynamic book text remain dependencies.
Item metadata now uses immutable shared NBT in PSRAM; compound keys are normalized
for stack equality and list order is preserved. The embedded limits are 64 KiB per
item tag and 24 levels of NBT nesting; oversized/malformed values are rejected rather
than truncated. The existing client packet-size limits also apply. Tag preservation
alone does not implement enchantment, potion or other item effects. None of these gaps is an accepted exclusion from the target.

`/lag` reports cumulative redstone updates, the peak work-list length, failures and
scheduled-tick capacity/refusals. Dormant combinational circuits perform no redstone update work per tick. Hoppers
remain periodic block entities; the tick list uses reusable PSRAM scratch and chunk
counters skip chunks without ticking block entities.
The work list is allocated in PSRAM, grows up to 65,536 entries and stops processing
on allocation failure or one million updates in one drain, with an error log and
failure counter. Restart is currently required after this fault. These resource
limits differ from vanilla; they must not be treated as successful circuit execution.
The existing scheduler capacity is 4,096 events. Its entire due batch now runs in
one tick, instead of spilling events after 256 into later ticks.

Pending ticks mark chunks dirty when scheduled and consumed. Chunk serialization
visits timers in due/priority/insertion order, so reusing a timer slot cannot reorder
same-tick events within a saved chunk. Global cross-chunk reload ordering still needs
conformance tests. Chunks with new comparator, moving-piston or automated-container block entities
use format 4 alongside tagged inventory records. Variable-size player records use two
independent descriptors and payload copies, with a durability barrier before
publishing a new descriptor. The tests cover torn saves and recovery followed by
another failed save. Worlds in older formats are not migrated: opening one starts a
new world (decided when this branch was merged into `main`).

## Changes from 1.16 to 1.21.8

The server speaks 1.21.8 since October 2026. The engine above was built and traced
against 1.16.5; the 1.16.5 per-state values (light, conductors, signal sources, sturdy
faces, push reactions) are carried over to the 1.21.8 block states by block name and
properties, and the vanilla 1.16.5 traces still pass with their states remapped
(`tools/redstone/remap_traces.js`). What 1.17 to 1.21.8 changed for redstone, and what
the server does:

| Version | Change | Here |
|---|---|---|
| 1.17 | **Cauldrons** split into `cauldron`, `water_cauldron`, `lava_cauldron`, `powder_snow_cauldron`; comparators read the level (lava 3) | ✅ |
| 1.17 | **Lightning rods**: struck by lightning, 15 for 8 ticks, strong power into their support | ✅ the output and its end; nothing strikes them (no lightning yet) |
| 1.17 | **Copper** doors/trapdoors (1.21) open by hand and redstone; deepslate redstone ore | ✅ (generic by name; own sounds) |
| 1.19 | **Sculk sensors**: vibrations of game events within 8 blocks, power `max(1, 15 - floor(15 * d / 8))`, active 30 ticks then 10 ticks cooldown, strong power below, comparator = the last frequency; wool blocks vibrations | ✅ `sculk.cpp` |
| 1.20 | **Calibrated sculk sensors**: range 16, active 10 ticks, listen only to the frequency given on their input side | ✅ |
| 1.20 | **Mob heads on note blocks** play the mob (instrument from the head, played with a head on top) | ✅ |
| 1.20 | **Chiseled bookshelf**: six book slots chosen by the point clicked; comparator = last used slot (1-6); hoppers fill and empty it | ✅ |
| 1.20 | Decorated pots and jukeboxes as comparator providers | ❌ (no pot storage, no discs playing) |
| 1.21 | **Crafter**: crafts 4 ticks after a rising edge into the container in front or out of its face; slots can be disabled; hoppers fill it evenly; comparator = filled or disabled slots | ✅ window, placement, crafting; recipe remainders: buckets and honey bottles only |
| 1.21 | **Copper bulb**: toggles on each rising edge at once; light 15/12/8/4 by oxidation; comparator 15 when lit | ✅ |
| 1.21 | Wind charges switch levers, buttons, doors, trapdoors, gates, bells | ❌ (no wind charges, breezes or maces) |
| 1.21.2+ | Redstone and minecart experiments (new wire update order, ...) | not vanilla's default: not done |

Game events that sculk sensors hear (frequency): steps of players and mobs (1, not
while sneaking), arrows landing (2), damage (7), eating (8), containers, doors and
switches closing or turning off (9) and opening or turning on, note blocks and fuses
(10), blocks broken (12) and placed (13), explosions and deaths (15). Missing compared
with vanilla: swimming, item drops and pick-ups, fluids placed and picked up, entity
interactions, and the vibration selector's tie rules (a sensor takes the first
vibration that reaches it, not the closest of a tick).

## Reference-derived state properties

`python3 tools/redstone/generate.py` queries all 17,112 states from the checksum-pinned
official server jar using `tools/redstone/StateProperties.java`. It starts only the
registries and loads the pinned jar’s block tags, not a Minecraft server. Java source-file launch support is required.
The script verifies the committed block/state ID ranges and, when the test dependency
is installed, every property value against `minecraft-data`. The committed table
contains emission, conduction, signal-source, sturdy-face, note-instrument and piston
reaction flags (51,336 flash bytes). The 272 deduplicated base collision shapes use
another 38,288 bytes, including state offsets. Shape coordinates retain 1/32-block
precision. Runtime power and light queries do not allocate or invoke Java.

The runtime-property oracle establishes those static properties only. A separate
transition oracle, `tools/redstone/trace.py`, runs deterministic circuits in the
official 1.16.5 server and records per-tick observations. Sixteen recorded cases
currently match the native implementation: repeater delays and side locking,
observer pulses, torch inversion and burnout/recovery, sticky-piston short/long
pulses, comparator modes, cross-chunk dust and lamp/quasi-piston callback behavior.
These cases do not establish arbitrary intra-tick ordering or full compatibility.

Inputs and recorded observations are committed in `tools/redstone/circuits.json`
and `tools/redstone/traces.json`; native fixtures are generated into
`host/tests/data/redstone_traces.inc`. Native tests do not require Java. To record
again after caching the reference jar with `generate.py`, run
`python3 tools/redstone/trace.py --accept-eula` (accepting the Minecraft EULA).
`--from-recorded` regenerates native fixtures without starting the reference server.

## Validation history

- Merge into `main` (2026-10-08): 227 host tests / 90,636 checks; all end-to-end
  suites against the PC server (`run_all.js` with this test, dimensions, portals,
  dragon fight, water fall, path finding, item float, operator menu with world reset,
  dashboard). On the ESP32-S3 board (Waveshare AMOLED 1.8, NBD): `hardware_smoke.js`,
  `redstone.js --host` (13,480 updates, 0 failures, 0 refused ticks),
  `hardware_dimensions.js` and `dashboard.js` passed. A/B against `main`, each on a
  fresh world, 70 s with a player loading terrain: 2.7-4.6 ms/tick vs 2.8-3.6, longest
  loop pass 6-7 ms vs 5-6, internal RAM 106 KB (min 39) in both; about 110 KB more
  PSRAM in use; firmware +165 KB. With the test's circuits left at spawn: 3.6 ms/tick,
  6-7 ms. Fixed in the merge: the operator menu's world reset (the reset world was
  written as format 3 with format-4 player slots and refused on restart, so the chosen
  seed was lost; the player table was zeroed at the old slot size). Worlds in older
  formats are no longer migrated (a new world starts instead). `nether_mobs.js`
  (piglin group anger) is timing-sensitive on `main` as well: 3 of 5 runs passed
  merged, 2 of 5 on `main`.
- Publication check (2026-10-08): `make -C host -j4 test server` passed with
  **207 tests / 86,173 checks**, including the latest callback fixes, migration
  fault test and all sixteen recorded vanilla transition fixtures.
- Latest recorded vanilla comparisons: 16 tests / 1,058 checks passed, including
  torch burnout/recovery and lamp callbacks that must not wake quasi-powered pistons.
- ASan/UBSan: 61 redstone tests / 1,484 checks, four metadata tests / 35 checks,
  and 13 storage/IO tests / 323 checks passed.
- All seven native client suites passed before the latest callback fixes. The
  circuit test covers cross-chunk wire/lamp transitions,
  piston/note events, hopper/dropper transfers, dispenser windows, lectern windows,
  page changes and book metadata over actual network packets.
- S3/P4 firmware builds and expanded emulator circuit tests passed with the input,
  inventory, book and storage additions, with zero redstone failures or refused
  ticks on both targets. These checks preceded the latest callback fixes and need
  rerunning on the published snapshot. Earlier emulator checks also covered login,
  terrain, diagnostics, task tracing, NBD save and cold restart. These are not
  physical-board validation or full circuit benchmarks.
- The sanitizer results above also precede the latest callback fixes, migration
  fault test and recorded transition fixtures; a refreshed sanitizer run is open.

Run native checks with `make -C host test server` and `node test/run_all.js`.
The circuit test can also target a built firmware image with
`node test/redstone.js --emulator esp32s3-8` (or `esp32p4-8`).
