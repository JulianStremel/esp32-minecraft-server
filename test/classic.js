'use strict';
// The classic generator's own structures (generator version 100) with a real client:
//  - /locate structure village: at its place the client sees its roads, beds and villagers
//  - /locate structure stronghold: its portal room's twelve frames take eyes of ender and
//    the portal opens; an eye thrown far away flies towards the stronghold
//  - the Nether: a fortress; the End: an End gateway out to the outer islands (and one
//    built there to come back), an End tower of purpur with its shulkers
//   SERVER_BIN=~/mc-host-build/mcserver node classic.js --local
//   node classic.js --host 192.168.1.160     (a world made with generator 100)
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
if (!host && !local) { console.error('usage: node classic.js --host <ip> | --local'); process.exit(2); }
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
const at = (line) => {
  const m = /\[(-?\d+), ~, (-?\d+)\]/.exec(line);
  return m ? [Number(m[1]), Number(m[2])] : null;
};

(async () => {
  try {
    if (local) {
      const file = path.join(os.tmpdir(), `mc-classic-${process.pid}.img`);
      server = startServer(['--port', String(PORT), '--seed', '42', '--ops', 'Tester', '--generator', '100', '--file', file,
        '--size', '256', '--format', '--view', '8', '--radius', '4096'], { log: args.includes('--log') });
      await server.ready;
      host = 'localhost';
    }
    op = await connectBot(PORT, 'Tester', { host, checkTimeoutInterval: 600000 });
    op.on('error', () => {});
    await waitFor(() => op.entity, 30000, 'spawned');
    await command('/gamemode creative', /mode/i);
    op.creative.startFlying();

    // a village: its roads, beds and villagers
    const vl = await command('/locate structure village', /nearest|Could not/, 60000);
    const v = at(vl);
    assert(v, 'locate village: ' + vl);
    results.push(`locate: ${vl.trim()}`);
    await command(`/tp Tester ${v[0] + 8} 110 ${v[1] + 8}`, /Teleported/);
    await sleep(8000);
    let paths = [];
    await waitFor(() => (paths = op.findBlocks({ matching: (b) => b && (b.name === 'dirt_path' || b.name === 'smooth_sandstone'), maxDistance: 96, count: 100 })).length >= 50,
      60000, 'the roads');
    const beds = op.findBlocks({ matching: (b) => b && b.name.endsWith('_bed'), maxDistance: 96, count: 50 });
    let villagers = [];
    await waitFor(() => (villagers = Object.values(op.entities).filter((e) => e.name === 'villager')).length > 0, 30000, 'villagers');
    results.push(`the village: ${paths.length}+ road blocks, ${beds.length} bed blocks, ${villagers.length} villagers`);
    assert(beds.length >= 2, 'beds in the houses');

    // a stronghold: the portal room
    const sl = await command('/locate structure stronghold', /nearest|Could not/, 60000);
    const s = at(sl);
    assert(s, 'locate stronghold: ' + sl);
    results.push(`locate: ${sl.trim()}`);
    await command(`/tp Tester ${s[0] + 8} 40 ${s[1] + 8}`, /Teleported/);
    await sleep(8000);
    let frames = [];
    await waitFor(() => (frames = op.findBlocks({ matching: (b) => b && b.name === 'end_portal_frame', maxDistance: 128, count: 16 })).length >= 12,
      90000, 'the portal room in view');
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

    // an eye thrown 600 blocks east flies west, towards the stronghold
    await command(`/tp Tester ${s[0] + 600} 200 ${s[1]}`, /Teleported/);
    await sleep(4000);
    await op.equip(op.inventory.items().find((i) => i.name === 'ender_eye'), 'hand');
    op.activateItem();
    let eye;
    await waitFor(() => (eye = Object.values(op.entities).find((e) => e.name === 'eye_of_ender')), 10000, 'an eye in flight');
    const start = eye.position.clone();
    await sleep(1500);
    results.push(`the eye flew from x ${start.x.toFixed(1)} to ${eye.position.x.toFixed(1)} (the stronghold at ${s[0]})`);
    assert(eye.position.x < start.x - 1, 'the eye flies west, towards the stronghold');

    // the Nether: a fortress of nether bricks over the lava sea
    await command('/dimension the_nether Tester', /./);
    await sleep(6000);
    const fl = await command('/locate structure fortress', /nearest|Could not/, 60000);
    const f = at(fl);
    assert(f, 'locate fortress: ' + fl);
    results.push(`locate: ${fl.trim()}`);
    await command(`/tp Tester ${f[0] + 8} 80 ${f[1] + 8}`, /Teleported/);
    await sleep(6000);
    let bricks = [];
    await waitFor(() => (bricks = op.findBlocks({ matching: (b) => b && b.name === 'nether_bricks', maxDistance: 64, count: 300 })).length >= 300,
      60000, 'the fortress');
    results.push(`the fortress: ${bricks.length}+ nether bricks`);

    // the End: a gateway throws the player out to the outer islands, where a gateway back is built
    await command('/dimension the_end Tester', /./);
    await sleep(6000);
    await command('/setblock 60 80 0 end_gateway', /./);
    await sleep(1000);
    await command('/tp Tester 60.5 80 0.5', /Teleported/);
    await waitFor(() => Math.hypot(op.entity.position.x, op.entity.position.z) > 900, 20000, 'the gateway jump');
    const out = op.entity.position.clone();
    let back = [];
    await waitFor(() => (back = op.findBlocks({ matching: (b) => b && b.name === 'end_gateway', maxDistance: 16, count: 1 })).length > 0,
      60000, 'the gateway back');
    results.push(`through the gateway to ${out.x.toFixed(0)} ${out.y.toFixed(0)} ${out.z.toFixed(0)}, the gateway back at ${back[0]}`);
    await sleep(1000);
    await command(`/tp Tester ${back[0].x + 0.5} ${back[0].y} ${back[0].z + 0.5}`, /Teleported/);
    await waitFor(() => Math.hypot(op.entity.position.x, op.entity.position.z) < 120, 20000, 'the way back');
    results.push(`and back to ${op.entity.position.x.toFixed(0)} ${op.entity.position.y.toFixed(0)} ${op.entity.position.z.toFixed(0)}`);
    const el = await command('/locate structure end_city', /nearest|Could not/, 60000);
    const ec = at(el);
    assert(ec, 'locate end_city: ' + el);
    results.push(`locate: ${el.trim()}`);
    await command(`/tp Tester ${ec[0] + 8} 120 ${ec[1] + 8}`, /Teleported/);
    await sleep(6000);
    let purpur = [];
    await waitFor(() => (purpur = op.findBlocks({ matching: (b) => b && b.name.startsWith('purpur'), maxDistance: 64, count: 300 })).length >= 300,
      60000, 'the End tower');
    let shulkers = [];
    await waitFor(() => (shulkers = Object.values(op.entities).filter((e) => e.name === 'shulker')).length > 0, 30000, 'shulkers');
    results.push(`the End tower: ${purpur.length}+ purpur blocks, ${shulkers.length} shulkers`);
    console.log('PASS classic\n  ' + results.join('\n  '));
  } catch (e) {
    console.log('FAIL classic: ' + (e && e.stack || e) + '\n  ' + results.join('\n  '));
    process.exitCode = 1;
  } finally {
    try { op && op.quit(); } catch (e) {}
    if (server) server.stop();
    setTimeout(() => process.exit(), 500);
  }
})();
