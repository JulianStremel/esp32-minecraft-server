'use strict';
// Vanilla's structures (generator version 15) with a real client:
//  - /locate structure finds a desert pyramid; at its place the client sees the pyramid
//    (the blue terracotta in the middle of the floor, its towers)
//  - its chests below hold loot, and a chest opens with it
//  - a stronghold: its portal room's twelve frames take eyes of ender and the portal
//    opens; its spawner brings silverfish; an eye thrown far away flies towards the
//    stronghold
//  - a mineshaft: its rails and supports
//  - a plains village (its bell, beds, villagers) and a pillager outpost
//  - trial chambers' trial spawners
//  - a Nether fortress: /locate in the Nether, its bricks, a spawner that brings blazes; a bastion remnant
//  - a woodland mansion: its dark oak, chests and illagers
//  - an End city: its purpur and shulkers, its ship's elytra in an item frame
//   node structures.js --host 192.168.1.160     (a world made with generator 20)
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
      server = startServer(['--port', String(PORT), '--seed', '42', '--ops', 'Tester', '--generator', '20', '--file', file,
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
    // the pyramid's chests: those within 15 blocks of the blue terracotta (other structures may be near)
    const chests = op.findBlocks({ matching: (b) => b && b.name === 'chest', maxDistance: 96, count: 16 })
      .filter((c) => blue.length && c.distanceTo(blue[0]) <= 15);
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

    // trial chambers deep underground: their trial spawners
    const tl = await command('/locate structure trial_chambers', /nearest|Could not/, 60000);
    const tm = /\[(-?\d+), ~, (-?\d+)\]/.exec(tl);
    assert(tm, 'locate trial chambers: ' + tl);
    results.push(`locate: ${tl.trim()}`);
    await command(`/tp Tester ${Number(tm[1]) + 8} -30 ${Number(tm[2]) + 8}`, /Teleported/);
    await sleep(10000);
    let tsp = [];
    await waitFor(() => (tsp = op.findBlocks({ matching: (b) => b && b.name === 'trial_spawner', maxDistance: 96, count: 10 })).length > 0,
      60000, 'trial spawners');
    results.push(`the trial chambers: ${tsp.length} trial spawners`);

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
    // a bastion remnant
    const bl = await command('/locate structure bastion_remnant', /nearest|Could not/, 60000);
    const bm = /\[(-?\d+), ~, (-?\d+)\]/.exec(bl);
    assert(bm, 'locate bastion: ' + bl);
    results.push(`locate: ${bl.trim()}`);
    await command(`/tp Tester ${Number(bm[1]) + 8} 100 ${Number(bm[2]) + 8}`, /Teleported/);
    await sleep(10000);
    let bricks2 = [];
    await waitFor(() => (bricks2 = op.findBlocks({ matching: (b) => b && b.name === 'polished_blackstone_bricks', maxDistance: 96, count: 200 })).length >= 100,
      60000, 'the bastion');
    results.push(`the bastion: ${bricks2.length}+ polished blackstone bricks`);

    // a woodland mansion: its dark oak, its chests, its vindicators and evokers
    await command('/dimension overworld', /./);
    await waitFor(() => op.game.dimension === 'overworld', 30000, 'back in the overworld');
    await sleep(3000);
    const ml2 = await command('/locate structure mansion', /nearest|Could not/, 120000);
    const mm2 = /\[(-?\d+), ~, (-?\d+)\]/.exec(ml2);
    assert(mm2, 'locate mansion: ' + ml2);
    results.push(`locate: ${ml2.trim()}`);
    op.creative.startFlying();
    await command(`/tp Tester ${Number(mm2[1]) + 8} 110 ${Number(mm2[2]) + 8}`, /Teleported/);
    await sleep(10000);
    let dark = [];
    await waitFor(() => (dark = op.findBlocks({ matching: (b) => b && b.name === 'dark_oak_planks', maxDistance: 96, count: 500 })).length >= 500,
      60000, 'the mansion');
    let illagers = [];
    await waitFor(() => (illagers = Object.values(op.entities).filter((e) => e.name === 'vindicator' || e.name === 'evoker')).length > 0,
      30000, 'illagers');
    const mchests = op.findBlocks({ matching: (b) => b && b.name === 'chest', maxDistance: 96, count: 20 });
    results.push(`the mansion: ${dark.length}+ dark oak planks, ${mchests.length} chests, ${illagers.length} illagers in view`);

    // an End city: purpur, its shulkers, and if it has a ship, the elytra in its item frame
    await command('/dimension end', /./);
    await waitFor(() => op.game.dimension === 'the_end', 30000, 'in the End');
    await sleep(3000);
    const el = await command('/locate structure end_city', /nearest|Could not/, 120000);
    const em = /\[(-?\d+), ~, (-?\d+)\]/.exec(el);
    assert(em, 'locate end city: ' + el);
    results.push(`locate: ${el.trim()}`);
    op.creative.startFlying();   // above the void
    await command(`/tp Tester ${Number(em[1]) + 8} 120 ${Number(em[2]) + 8}`, /Teleported/);
    await sleep(10000);
    let purpur = [];
    await waitFor(() => (purpur = op.findBlocks({ matching: (b) => b && b.name === 'purpur_block', maxDistance: 96, count: 200 })).length >= 100,
      60000, 'the End city');
    let shulkers = [];
    await waitFor(() => (shulkers = Object.values(op.entities).filter((e) => e.name === 'shulker')).length > 0, 30000, 'shulkers');
    results.push(`the End city: ${purpur.length}+ purpur blocks, ${shulkers.length} shulkers`);
    // seed 42's nearest End city has a ship: its dragon head, and the elytra in an item frame
    let head;
    await waitFor(() => (head = op.findBlocks({ matching: (b) => b && b.name.startsWith('dragon_'), maxDistance: 160, count: 1 })[0]),
      60000, "the ship's dragon head");
    await command(`/tp Tester ${head.x + 0.5} ${head.y + 2} ${head.z + 0.5}`, /Teleported/);
    let frame;
    await waitFor(() => (frame = Object.values(op.entities).find((e) => e.name === 'item_frame')), 30000, 'the item frame');
    await sleep(1000);
    const held = frame.metadata && frame.metadata.find((m) => m && typeof m === 'object' && (m.itemId !== undefined || m.name));
    const heldName = held ? (held.name || (op.registry.items[held.itemId] || {}).name) : 'nothing';
    results.push(`the ship: its dragon head at ${head}, an item frame at ${frame.position} holding ${heldName}`);
    assert.strictEqual(heldName, 'elytra', 'the elytra in its frame');
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
