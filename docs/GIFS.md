# Feature GIFs: capture plan

The README shows what the server does with short recordings from the **real board**
(Waveshare ESP32-S3-Touch-AMOLED-1.8, 8 MB PSRAM, WiFi, world on NBD). This page plans
one GIF per implemented feature: what each shows, how it is staged and recorded, and
what is done.

## How they are made

Two ways, depending on what has to be seen:

- **Scripted scenes** (most of them): a camera bot (an operator, in spectator mode,
  physics off) connects to the board, a script builds the scene with commands, then
  runs it while prismarine-viewer draws what the camera bot knows in headless Chromium
  (Playwright screencast) and ffmpeg makes the GIF. Repeatable and real time: what you
  see is the board's packets. `test/record_redstone_gif.js` is the first of them.
  The viewer draws blocks and entities (mobs, items, arrows, falling blocks, primed
  TNT), not the HUD: no chat, inventories, boss bars, particles or the hand.
- **The real client** (for what the viewer cannot show: windows, the HUD, boss bars):
  played by hand with Minecraft 1.16.5 and screen-recorded, like `gameplay.gif`.

Rules for all of them:

- Recorded on the board, never in an emulator, from a **fresh scratch world** on NBD
  (`build/hw/gif-*.img`), not a world anyone plays on; seed 42.
- Each scene script has a `--check` mode that builds the scene and asserts the outcome
  through the bot's world without recording, and runs with `--local` against the PC
  server, so scenes are developed on the PC and only recorded on the board.
- Real time unless the caption says otherwise; 640 x 360, 10-15 fps, at most 12-20 s
  and about 2 MB each. The caption names the build (commit) it was recorded with.
- Built high in the air (y 150+) on a smooth-stone platform when terrain does not
  matter, so nothing else is in the frame.

Plan for the tooling: move the shared parts of `record_redstone_gif.js` (camera pose,
viewer, screencast, ffmpeg, `--check`/`--local`/`--host`) into `test/gif/capture.js`,
one small scene file per GIF in `test/gif/scenes/`, and `node test/gif/record.js
--host <ip> [scene ...]` to record all or some of them in one go.

## The list

| # | GIF | Feature shown | Scene | How | Status |
|---|---|---|---|---|---|
| 1 | `gameplay.gif` | real client, digging, building, flying, `/perfbar` | played by hand, 4x speed | client | done |
| 2 | `redstone.gif` | repeaters, lamps, dust, sticky piston | repeater chain lighting 8 lamps in sequence, a piston pushing a gold block, power toggled every 2 s | scene | **done** |
| 3 | `terrain.gif` | world generation on the chip, biomes, chunk streaming | the camera flies at 10 blocks/s over fresh terrain across biome borders (plains, forest, desert, mountains); terrain appears as the board generates it | scene | planned (replaces the old QEMU recording) |
| 4 | `nether_portal.gif` | portal frames, flint and steel, linked portals, the Nether | an obsidian frame is filled with `/fill`, lit with flint and steel (portal blocks appear), a player walks in and arrives in the Nether: netherrack, lava sea, glowstone | scene, two cameras (overworld, then Nether) | planned |
| 5 | `the_end.gif` | the End island, obsidian spikes, end crystals, the dragon | the camera circles the main island while the dragon flies its paths between the spikes | scene | planned (check that the viewer draws the dragon and crystals; else client) |
| 6 | `dragon_fight.gif` | boss bar, crystals exploding, the dragon's death, exit portal, egg | a player shoots crystals and fights the dragon | client | planned |
| 7 | `mobs_pathfinding.gif` | A* path finding on the workers | a zombie walks around a wall with one gap (and up steps) to reach a player on the other side; next to it a zombie without a path would walk into the wall | scene | planned |
| 8 | `creeper.gif` | creeper fuse and explosion, terrain damage | a creeper walks up to a player and explodes, leaving a crater; dropped blocks fly | scene | planned |
| 9 | `nether_mobs.gif` | ghasts, fireballs, magma cubes splitting | a ghast shoots at a player, the player hits the fireball back; magma cubes jump and split when killed | scene (fireball deflection scripted) | planned |
| 10 | `tnt.gif` | primed TNT with fuse, chain reactions | a line of TNT lit at one end; each explosion primes the next with a short fuse; flashing primed TNT | scene | planned |
| 11 | `flying_machine.gif` | observers, slime blocks, sticky pistons, quasi-connectivity | a slime-block flying machine travels 20 blocks across the platform | scene | planned (first check the contraption on the PC: slime branching is implemented, update order may still differ from vanilla) |
| 12 | `hoppers.gif` | hoppers, droppers, item transfer, comparators | items dropped on a hopper line flow into a chest; a comparator lights a lamp when the chest fills; a dropper ejects items | scene | planned |
| 13 | `fluids.gif` | flowing water and lava, falling sand | water poured on a terraced slope flows down, lava spreads slower, sand and gravel fall when the block below is broken | scene | planned |
| 14 | `farm.gif` | crops, sugar cane, grass spreading | time-lapse of a field growing (random ticks; frames sampled every few seconds, captioned as time-lapse) | scene, time-lapse | planned |
| 15 | `lighting.gif` | sky and block light across chunk borders | torches placed in a cave and a tunnel across a chunk border at night; light spreads into the neighbour chunk | client (the viewer does not draw light levels) | planned |
| 16 | `survival.gif` | crafting, furnace, inventory, eating | crafting a pickaxe, smelting ore, eating; health and hunger bars | client | planned |
| 17 | `operator_menu.gif` | `/menu` | opening the menu, the statistics page, changing difficulty and time, the player page | client | planned |
| 18 | `dashboard.gif` | the status dashboard (`MC_DASHBOARD`) | the dashboard page in a browser while players join and fly: TPS graph, memory, the player list updating every second | Playwright screencast of the real page | planned |
| 19 | `persistence.gif` | NBD persistence across a reset | a structure is built, the board is reset over USB (the camera bot reconnects), the structure is still there | scene with a cut | planned |

Order of work: the tooling refactor, then the scene GIFs that need only blocks (3, 10,
11, 12, 13, 4), then those with mobs (7, 8, 9, 5), then the client recordings (6, 15,
16, 17) and the dashboard (18). The README gets a small gallery (two per row, linked
to their sections) instead of one long column.
