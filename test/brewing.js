'use strict';
// The brewing stand with a real client: three water bottles, nether wart and blaze powder
// put in through its window; 20 s later three awkward potions, then gunpowder makes them
// splash potions; the bottles show on the block; a hopper above feeds the ingredient.
//   node brewing.js --host 192.168.1.160
//   SERVER_BIN=~/mc-host-build/mcserver node brewing.js --local
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { Vec3 } = require('vec3');
const { startServer, connectBot, nextChat, sleep, waitFor } = require('./lib');

const args = process.argv.slice(2);
const opt = (name, fallback) => {
  const i = args.indexOf('--' + name);
  return i < 0 ? fallback : args[i + 1];
};
const local = args.includes('--local');
let host = opt('host');
if (!host && !local) { console.error('usage: node brewing.js --host <ip> | --local'); process.exit(2); }
const PORT = local ? 25600 + Math.floor(Math.random() * 300) : 25565;
const X = Number(opt('x', '60')), Z = Number(opt('z', '60')), Y = 120;
let server, op, runner, worldFile;

async function command(text, pattern, ms = 15000) {
  const r = nextChat(op, pattern, ms);
  op.chat(text);
  const line = await r;
  await sleep(500);
  return line;
}
const potionOf = (item) => {
  const c = item && (item.components || []).find((k) => k.type === 'potion_contents');
  return c && c.data ? (c.data.potionId !== undefined ? c.data.potionId : c.data.potion) : undefined;
};
const AWKWARD = 3, WATER = 0;

// moves what is in the window's slot `from` into slot `to` (pick up, put down)
async function move(win, from, to) {
  await runner.clickWindow(from, 0, 0);
  await sleep(150);
  await runner.clickWindow(to, 0, 0);
  await sleep(150);
  if (runner.inventory.selectedItem || win.selectedItem) await runner.clickWindow(from, 0, 0);   // anything left back
  await sleep(150);
}
const invSlot = (win, name) => win.slots.findIndex((s, i) => i >= 5 && s && s.name === name);

(async () => {
  try {
    if (local) {
      worldFile = path.join(os.tmpdir(), `mc-brewing-${process.pid}.img`);
      server = startServer(['--port', String(PORT), '--seed', '42', '--ops', 'Tester', '--file', worldFile, '--size', '256',
        '--format', '--flat'], { log: args.includes('--log') });
      await server.ready;
      host = 'localhost';
    }
    op = await connectBot(PORT, 'Tester', { host, checkTimeoutInterval: 600000 });
    op.on('error', () => {});
    op.physicsEnabled = false;
    await command('/gamemode spectator', /game mode/i);
    runner = await connectBot(PORT, 'Runner', { host, checkTimeoutInterval: 600000 });
    runner.on('error', () => {});
    await command('/gamemode survival Runner', /game mode/i);
    await command(`/tp Tester ${X} ${Y + 4} ${Z - 6}`, /Teleported/);
    await sleep(2000);
    await command(`/fill ${X - 3} ${Y - 1} ${Z - 3} ${X + 3} ${Y - 1} ${Z + 3} stone`, /filled/i);
    await command(`/fill ${X - 3} ${Y} ${Z - 3} ${X + 3} ${Y + 3} ${Z + 3} air`, /filled/i);
    await command(`/setblock ${X} ${Y} ${Z + 2} brewing_stand`, /Changed|set/i);
    await command(`/tp Runner ${X}.5 ${Y} ${Z}.5`, /Teleported/);
    await waitFor(() => runner.entity && Math.abs(runner.entity.position.y - Y) < 0.1, 10000, 'on the platform');
    await command('/clear Runner', /Cleared|Removed|clear/i);
    for (let i = 0; i < 3; i++) await command('/give Runner potion[potion_contents={potion:"minecraft:water"}]', /Gave/);
    await command('/give Runner nether_wart 1', /Gave/);
    await command('/give Runner blaze_powder 1', /Gave/);
    await command('/give Runner gunpowder 1', /Gave/);
    await sleep(800);

    // 1) water bottles, nether wart, blaze powder into the stand
    const stand = runner.blockAt(new Vec3(X, Y, Z + 2));
    assert(stand && stand.name === 'brewing_stand', 'no brewing stand');
    let win = await runner.openBlock(stand);
    for (let b = 0; b < 3; b++) await move(win, invSlot(win, 'potion'), b);
    await move(win, invSlot(win, 'nether_wart'), 3);
    await move(win, invSlot(win, 'blaze_powder'), 4);
    await sleep(1000);
    const filled = [0, 1, 2].map((i) => potionOf(win.slots[i]));
    console.log(`  in the stand: bottles ${filled.join(', ')}, ingredient ${win.slots[3] ? win.slots[3].name : 'none'}, ` +
      `fuel slot ${win.slots[4] ? win.slots[4].name : 'empty'}`);
    assert.deepStrictEqual(filled, [WATER, WATER, WATER], 'the bottles did not go in');
    const t0 = Date.now();
    await waitFor(() => [0, 1, 2].every((i) => potionOf(win.slots[i]) === AWKWARD), 30000, 'the brew').catch(() => {});
    const took = (Date.now() - t0) / 1000;
    const brewed = [0, 1, 2].map((i) => potionOf(win.slots[i]));
    console.log(`  after ${took.toFixed(1)} s: ${brewed.join(', ')} (3: awkward), nether wart ${win.slots[3] ? 'left' : 'used'}`);
    assert.deepStrictEqual(brewed, [AWKWARD, AWKWARD, AWKWARD], 'no awkward potions');
    assert(took > 15 && took < 25, 'not 20 s');
    const block = runner.blockAt(new Vec3(X, Y, Z + 2));
    const props = block.getProperties ? block.getProperties() : {};
    console.log(`  the block: has_bottle ${props.has_bottle_0}, ${props.has_bottle_1}, ${props.has_bottle_2}`);
    assert(props.has_bottle_0 && props.has_bottle_1 && props.has_bottle_2, 'the bottles do not show');

    // 2) gunpowder through a hopper above: splash potions
    runner.closeWindow(win);
    await command(`/setblock ${X} ${Y + 1} ${Z + 2} hopper[facing=down]`, /Changed|set/i);
    win = await runner.openBlock(runner.blockAt(new Vec3(X, Y + 1, Z + 2)));
    await move(win, win.slots.findIndex((s, i) => i >= 5 && s && s.name === 'gunpowder'), 0);
    runner.closeWindow(win);
    await sleep(1500);
    win = await runner.openBlock(runner.blockAt(new Vec3(X, Y, Z + 2)));
    await waitFor(() => win.slots[0] && win.slots[0].name === 'splash_potion', 30000, 'splash potions').catch(() => {});
    console.log(`  gunpowder from a hopper: ${[0, 1, 2].map((i) => win.slots[i] && win.slots[i].name).join(', ')}`);
    assert([0, 1, 2].every((i) => win.slots[i] && win.slots[i].name === 'splash_potion' && potionOf(win.slots[i]) === AWKWARD),
      'no splash potions');
    runner.closeWindow(win);
    console.log(`BREWING OK (${host})`);
  } finally {
    for (const b of [runner, op]) if (b) b.quit();
    if (server) await server.stop();
    if (worldFile) fs.rmSync(worldFile, { force: true });
  }
})().catch((err) => { console.error(err.message || err); process.exitCode = 1; });
