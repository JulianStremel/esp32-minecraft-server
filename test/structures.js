'use strict';
// Vanilla's structures (generator version 14) with a real client:
//  - /locate structure finds a desert pyramid; at its place the client sees the pyramid
//    (the blue terracotta in the middle of the floor, its towers)
//  - its chests below hold loot, and a chest opens with it
//  - a stronghold: its portal room's twelve frames take eyes of ender and the portal
//    opens; its spawner brings silverfish; an eye thrown far away flies towards the
//    stronghold
//  - a mineshaft: its rails and supports
//  - a plains village (its bell, beds, villagers) and a pillager outpost
//  - a Nether fortress: /locate in the Nether, its bricks, a spawner that brings blazes
//   node structures.js --host 192.168.1.160     (a world made with generator 14)
//   SERVER_BIN=~/mc-host-build/mcserver node structures.js --local
const assert = require('assert');
const os = require('os');
const path = require('path');
const { startServer, connectBot, nextChat, sleep, waitFor } = require('./lib');

const args = process.argv.slice(2);
const opt = (name, fallback) => {
  const i = args.indexOf('--' + name);
  return i < 0 ? fallback : args[i + 1];
};
const local = args.includes('--local');
let host = opt('host');
if (!host && !local) { console.error('usage: node structures.js --host <ip> | --local'); process.exit(2); }
const PORT = local ? 25600 + Math.floor(Math.random() * 300) : 25565;
let server, op;
const results = [];

async function command(text, pattern, ms = 30000) {
  const r = nextChat(op, pattern, ms);
  op.chat(text);
  const line = await r;
  await sleep(500);
  return line;
}

(async () => {
  try {
    if (local) {
      const file = path.join(os.tmpdir(), `mc-structures-${process.pid}.img`);
      server = startServer(['--port', String(PORT), '--seed', '42', '--ops', 'Tester', '--generator', '14', '--file', file,
        '--size', '256', '--format', '--view', '8', '--radius', '4096'], { log: args.includes('--log') });
      await server.ready;
      host = 'localhost';
    }
    op = await connectBot(PORT, 'Tester', { host, checkTimeoutInterval: 600000 });
    op.on('error', () => {});
    await waitFor(() => op.entity, 30000, 'spawned');
    await command('/gamemode creative', /mode/i);

    const line = await command('/locate structure minecraft:desert_pyramid', /nearest|Could not/);
    const m = /\[(-?\d+), ~, (-?\d+)\]/.exec(line);
    assert(m, 'locate: ' + line);
    const x = Number(m[1]), z = Number(m[2]);
    results.push(`locate: ${line.trim()}`);
    await command(`/tp Tester ${x + 10} 120 ${z + 10}`, /Teleported|tp/i);
    await waitFor(() => op.blockAt(op.entity.position.offset(0, -60, 0)), 30000, 'chunks there');
    await sleep(3000);
    const blue = op.findBlocks({ matching: (b) => b && b.name === 'blue_terracotta', maxDistance: 64, count: 4 });
    const chests = op.findBlocks({ matching: (b) => b && b.name === 'chest', maxDistance: 96, count: 8 });
    const tnt = op.findBlocks({ matching: (b) => b && b.name === 'tnt', maxDistance: 96, count: 16 });
    results.push(`the pyramid: ${blue.length} blue terracotta, ${chests.length} chests, ${tnt.length} tnt`);
    assert.strictEqual(blue.length, 1, 'one blue terracotta');
    assert.strictEqual(chests.length, 4, 'four chests');
    assert.strictEqual(tnt.length, 9, 'nine tnt');

    // open a chest: stand beside it (the treasure room is open above it)
    const c = chests[0];
    await command(`/tp Tester ${c.x + 0.5} ${c.y + 1} ${c.z + 0.5}`, /Teleported|tp/i);
    await sleep(1500);
    const win = await op.openContainer(op.blockAt(c));
    const items = win.containerItems();
    results.push(`a chest: ${items.map((i) => `${i.count} ${i.name}`).join(', ')}`);
    assert(items.length > 0, 'the chest holds loot');
    win.close();

    // a stronghold's portal room: an eye into each frame that has none, then the portal
    const sl = await command('/locate structure stronghold', /nearest|Could not/, 60000);
    const sm = /\[(-?\d+), ~, (-?\d+)\]/.exec(sl);
    assert(sm, 'locate stronghold: ' + sl);
    results.push(`locate: ${sl.trim()}`);
    const sx = Number(sm[1]), sz = Number(sm[2]);
    await command(`/tp Tester ${sx} 40 ${sz}`, /Teleported/);
    await sleep(8000);
    let frames = [];
    await waitFor(() => (frames = op.findBlocks({ matching: (b) => b && b.name === 'end_portal_frame', maxDistance: 128, count: 16 })).length >= 12,
      60000, 'the portal room in view');
    results.push(`portal room: ${frames.length} frames at ${frames[0]}`);
    const f0 = frames[0];
    await command(`/tp Tester ${f0.x + 0.5} ${f0.y + 1} ${f0.z + 0.5}`, /Teleported/);
    await sleep(2000);
    await command('/give Tester ender_eye 16', /Gave|give/i);
    await sleep(500);
    await op.equip(op.inventory.items().find((i) => i.name === 'ender_eye'), 'hand');
    let inserted = 0;
    for (const f of frames) {
      const b = op.blockAt(f);
      if (b.getProperties().eye === true || b.getProperties().eye === 'true') continue;
      await op.activateBlock(b);
      inserted++;
      await sleep(250);
    }
    await sleep(1500);
    const portal = op.findBlocks({ matching: (b) => b && b.name === 'end_portal', maxDistance: 16, count: 16 });
    results.push(`${inserted} eyes set, ${portal.length} end portal blocks`);
    assert.strictEqual(portal.length, 9, 'the portal opened');
    // the room's spawner brings silverfish while a player is near
    let fish = [];
    await waitFor(() => (fish = Object.values(op.entities).filter((e) => e.name === 'silverfish')).length > 0, 30000, 'silverfish');
    results.push(`the spawner: ${fish.length} silverfish`);

    // an eye thrown 600 blocks away flies towards the stronghold
    await command(`/tp Tester ${sx + 600} 200 ${sz}`, /Teleported/);
    await sleep(4000);
    await op.equip(op.inventory.items().find((i) => i.name === 'ender_eye'), 'hand');
    op.activateItem();
    let eye;
    await waitFor(() => (eye = Object.values(op.entities).find((e) => e.name === 'eye_of_ender')), 10000, 'an eye in flight');
    const start = eye.position.clone();
    await sleep(1500);
    const moved = eye.position.clone();
    results.push(`the eye flew from x ${start.x.toFixed(1)} to ${moved.x.toFixed(1)} (the stronghold at ${sx})`);
    assert(moved.x < start.x - 1, 'the eye flies west, towards the stronghold');

    // a mineshaft: its rails, supports and a chest minecart
    const ml = await command('/locate structure mineshaft', /nearest|Could not/, 60000);
    const mm = /\[(-?\d+), ~, (-?\d+)\]/.exec(ml);
    assert(mm, 'locate mineshaft: ' + ml);
    results.push(`locate: ${ml.trim()}`);
    await command(`/tp Tester ${Number(mm[1]) + 8} 30 ${Number(mm[2]) + 8}`, /Teleported/);
    await sleep(8000);
    const rails = op.findBlocks({ matching: (b) => b && b.name === 'rail', maxDistance: 96, count: 50 });
    const posts = op.findBlocks({ matching: (b) => b && b.name === 'oak_fence', maxDistance: 96, count: 50 });
    results.push(`the mineshaft: ${rails.length} rails, ${posts.length} fence posts`);
    assert(posts.length >= 10, 'the supports of the mineshaft');

    // a village: its bell, houses, villagers; a pillager outpost's tower
    const vl = await command('/locate structure village_plains', /nearest|Could not/, 60000);
    const vm = /\[(-?\d+), ~, (-?\d+)\]/.exec(vl);
    assert(vm, 'locate village: ' + vl);
    results.push(`locate: ${vl.trim()}`);
    await command(`/tp Tester ${Number(vm[1]) + 8} 120 ${Number(vm[2]) + 8}`, /Teleported/);
    await sleep(10000);
    let bell;
    await waitFor(() => (bell = op.findBlocks({ matching: (b) => b && b.name === 'bell', maxDistance: 96, count: 1 })[0]), 60000, 'the bell');
    const beds = op.findBlocks({ matching: (b) => b && b.name.endsWith('_bed'), maxDistance: 96, count: 50 });
    let villagers = [];
    await waitFor(() => (villagers = Object.values(op.entities).filter((e) => e.name === 'villager')).length > 0, 30000, 'villagers');
    results.push(`the village: a bell at ${bell}, ${beds.length} bed blocks, ${villagers.length} villagers`);
    const ol = await command('/locate structure pillager_outpost', /nearest|Could not/, 60000);
    const om = /\[(-?\d+), ~, (-?\d+)\]/.exec(ol);
    assert(om, 'locate outpost: ' + ol);
    results.push(`locate: ${ol.trim()}`);
    await command(`/tp Tester ${Number(om[1]) + 8} 140 ${Number(om[2]) + 8}`, /Teleported/);
    await sleep(10000);
    const logs = op.findBlocks({ matching: (b) => b && b.name === 'dark_oak_log', maxDistance: 96, count: 100 });
    results.push(`the outpost: ${logs.length} dark oak logs`);
    assert(logs.length >= 50, 'the outpost tower');

    // a Nether fortress: nether bricks and a blaze spawner that brings blazes
    await command('/dimension nether', /./);
    await waitFor(() => op.game.dimension === 'the_nether', 30000, 'in the Nether');
    await sleep(3000);
    const fl = await command('/locate structure fortress', /nearest|Could not/, 60000);
    const fm = /\[(-?\d+), ~, (-?\d+)\]/.exec(fl);
    assert(fm, 'locate fortress: ' + fl);
    results.push(`locate: ${fl.trim()}`);
    await command(`/tp Tester ${Number(fm[1]) + 10} 80 ${Number(fm[2]) + 10}`, /Teleported/);
    await sleep(8000);
    const bricks = op.findBlocks({ matching: (b) => b && b.name === 'nether_bricks', maxDistance: 64, count: 400 });
    let spawner;
    await waitFor(() => (spawner = op.findBlocks({ matching: (b) => b && b.name === 'spawner', maxDistance: 128, count: 1 })[0]),
      60000, 'a blaze spawner in view');
    results.push(`the fortress: ${bricks.length}+ nether bricks, a spawner at ${spawner}`);
    assert(bricks.length >= 400, 'nether bricks');
    await command(`/tp Tester ${spawner.x + 0.5} ${spawner.y + 1} ${spawner.z + 0.5}`, /Teleported/);
    let blazes = [];
    await waitFor(() => (blazes = Object.values(op.entities).filter((e) => e.name === 'blaze')).length > 0, 45000, 'blazes');
    results.push(`the spawner: ${blazes.length} blazes`);
    console.log('PASS structures\n  ' + results.join('\n  '));
  } catch (e) {
    console.log('FAIL structures: ' + (e && e.stack || e) + '\n  ' + results.join('\n  '));
    process.exitCode = 1;
  } finally {
    try { op && op.quit(); } catch (e) {}
    if (server) server.stop();
    setTimeout(() => process.exit(), 500);
  }
})();
