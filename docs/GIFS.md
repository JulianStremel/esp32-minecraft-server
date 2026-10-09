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
  played by hand with Minecraft 1.21.8 and screen-recorded, like `gameplay.gif`.

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

The tool: `test/gif/capture.js` (server, camera, viewer, screencast, ffmpeg, web
pages), one scene per file in `test/gif/scenes/` (`build`, `play` while recording,
`check`), and the runner:

```sh
cd test && npm ci --ignore-scripts && npx playwright install chromium   # once; ffmpeg on PATH or FFMPEG=...
node gif/record.js --list
node gif/record.js --host <board ip> --serial COM5 --python <idf python> [scene ...]   # into docs/images
node gif/record.js --host <board ip> --serial COM5 --check [scene ...]                # build and verify only
SERVER_BIN=.../mcserver node gif/record.js --local --check                           # on the PC
```

On the board each scene gets a fresh world image (`build/hw/gif-<scene>.img`, served by
`tools/nbd_server.py`) and the board is reset into it over USB; commands go through the
serial console (no chat spam limit). Long camera moves go through `/tp` (the server
refuses moves over 100 blocks). A scene can set its window (`viewport`), GIF width,
frame rate and colours.

## The list

| # | GIF | Feature shown | Scene | How | Status |
|---|---|---|---|---|---|
| 1 | `gameplay.gif` | real client, digging, building, flying, `/perfbar` | played by hand, 4x speed | client | done |
| 2 | `redstone.gif` | repeaters, lamps, dust, sticky piston | repeater chain lighting 8 lamps in sequence, a piston pushing a gold block, power toggled every 2 s | scene `redstone` | **done** |
| 3 | `terrain.gif` | world generation on the chip, biomes, chunk streaming | the camera flies at 9 blocks/s over fresh terrain; terrain appears as the board generates it | scene `terrain` | **done** (400 px, 8 fps: 3 MB) |
| 4 | `nether_portal.gif` | portal frames, flint and steel, linked portals | a player lights an obsidian frame with flint and steel and walks through | scene `nether_portal` | **done** (the Nether side: a second camera, later) |
| 5 | `the_end.gif` | the End island, obsidian spikes, end crystals, the dragon | the camera circles the main island while the dragon flies its paths between the spikes | scene | planned (check that the viewer draws the dragon and crystals; else client) |
| 6 | `dragon_fight.gif` | boss bar, crystals exploding, the dragon's death, exit portal, egg | a player shoots crystals and fights the dragon | client | planned |
| 7 | `pathfinding.gif` | A* path finding on the workers | a zombie walks along a wall to its gap and back to the player behind it | scene `pathfinding` | **done** (found and fixed: mobs jumped onto 1-block fences) |
| 8 | `creeper.gif` | creeper fuse and explosion, terrain damage | a creeper walks up to a player and explodes, leaving a crater | scene `creeper` | **done** (dropped items render magenta in the viewer) |
| 9 | `nether_mobs.gif` | ghasts, fireballs, magma cubes splitting | a ghast shoots at a player, the player hits the fireball back; magma cubes jump and split when killed | scene (fireball deflection scripted) | planned |
| 10 | `tnt.gif` | primed TNT with fuse, chain reactions | a line of TNT lit at one end; each explosion primes the next with a short fuse | scene `tnt` | scene ready; the chain stops before the last TNT (to investigate) |
| 11 | `flying_machine.gif` | observers, slime blocks, sticky pistons, quasi-connectivity | a slime-block flying machine travels 20 blocks across the platform | scene | planned (first check the contraption on the PC: slime branching is implemented, update order may still differ from vanilla) |
| 12 | `hoppers.gif` | hoppers, droppers, item transfer, comparators | items dropped on a hopper line flow into a chest; a comparator lights a lamp when the chest fills; a dropper ejects items | scene | planned |
| 13 | `fluids.gif` | flowing water and lava, falling sand | water and lava flow down glass-walled steps (lava slower), a sand and gravel tower falls | scene `fluids` | **done** |
| 14 | `farm.gif` | crops, sugar cane, grass spreading | time-lapse of a field growing (random ticks; frames sampled every few seconds, captioned as time-lapse) | scene, time-lapse | planned |
| 15 | `lighting.gif` | sky and block light across chunk borders | torches placed in a cave and a tunnel across a chunk border at night; light spreads into the neighbour chunk | client (the viewer does not draw light levels) | planned |
| 16 | `survival.gif` | crafting, furnace, inventory, eating | crafting a pickaxe, smelting ore, eating; health and hunger bars | client | planned |
| 17 | `operator_menu.gif` | `/menu` | opening the menu, the statistics page, changing difficulty and time, the player page | client | planned |
| 18 | `dashboard.gif` | the status dashboard (`MC_DASHBOARD`) | the dashboard page while three players join and fly off | scene `dashboard` (the page itself) | **done** |
| 19 | `persistence.gif` | NBD persistence across a reset | a structure is built, the board is reset over USB (the camera bot reconnects), the structure is still there | scene with a cut | planned |

Order of work: the tooling refactor, then the scene GIFs that need only blocks (3, 10,
11, 12, 13, 4), then those with mobs (7, 8, 9, 5), then the client recordings (6, 15,
16, 17) and the dashboard (18). The README gets a small gallery (two per row, linked
to their sections) instead of one long column.
