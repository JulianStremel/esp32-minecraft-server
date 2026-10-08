'use strict';
// Nether portals as a player builds them: an obsidian frame lit with flint and steel,
// 4 s in it (survival) to the Nether, where a linked portal is built; back through that
// one to the first; breaking the frame removes the portal.
//   node nether_portal.js --host 192.168.1.160 [--x 9300 --z 9300]
//   SERVER_BIN=/tmp/mc-host-build/mcserver node nether_portal.js --local
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
if (!host && !local) { console.error('usage: node nether_portal.js --host <ip> | --local'); process.exit(2); }
const PORT = local ? 25600 + Math.floor(Math.random() * 300) : 25565;
const X = Number(opt('x', local ? '200' : '9300')), Z = Number(opt('z', local ? '200' : '9300')), Y = 150;
let server, op, worldFile;

async function command(text, pattern, ms = 15000) {
  const r = nextChat(op, pattern, ms);
  op.chat(text);
  const line = await r;
  await sleep(650);
  return line;
}

const blockName = (v) => { const b = op.blockAt(v); return b ? b.name : '?'; };

async function arrive(dim, what) {
  await waitFor(() => op.game.dimension === dim, 30000, `${what}: dimension ${dim}`);
  await waitFor(() => op.entity && op.blockAt(op.entity.position.offset(0, -1, 0)), 20000, `${what}: chunks`);
  await sleep(1500);
  return op.entity.position.clone();
}

(async () => {
  try {
    if (local) {
      worldFile = path.join(os.tmpdir(), `mc-portal-${process.pid}.img`);
      server = startServer(['--port', String(PORT), '--seed', '42', '--ops', 'Tester', '--file', worldFile, '--size', '256',
        '--no-mobs', '--format'], { log: args.includes('--log') });
      await server.ready;
      host = 'localhost';
    }
    op = await connectBot(PORT, 'Tester', { host, checkTimeoutInterval: 600000 });
    op.on('error', () => {});
    await command('/gamemode creative', /game mode/i);
    if (op.game.dimension !== 'overworld') {
      await command('/dimension overworld', /Mov(ed|ing) Tester/);
      await arrive('overworld', 'start');
    }
    await command(`/tp Tester ${X + 1}.5 ${Y} ${Z - 3}.5`, /Teleported/);
    await sleep(3000);
    // a stone floor, and a 4 x 5 frame along x: inside x X..X+1, y Y..Y+2, at z Z
    await command(`/fill ${X - 3} ${Y - 2} ${Z - 4} ${X + 4} ${Y + 5} ${Z + 3} air`, /filled|Success/i).catch(() => {});
    await command(`/fill ${X - 3} ${Y - 2} ${Z - 4} ${X + 4} ${Y - 2} ${Z + 3} stone`, /filled/i);
    await command(`/fill ${X - 1} ${Y - 1} ${Z} ${X + 2} ${Y + 3} ${Z} obsidian`, /filled/i);
    await command(`/fill ${X} ${Y} ${Z} ${X + 1} ${Y + 2} ${Z} air`, /filled/i);
    await command(`/tp Tester ${X + 1}.5 ${Y - 1} ${Z - 3}.5`, /Teleported/);   // onto the new floor
    await sleep(1500);
    await command('/give Tester flint_and_steel 1', /Gave|Given/i);
    await waitFor(() => op.inventory.items().some((i) => i.name === 'flint_and_steel'), 5000, 'flint and steel');
    await op.equip(op.inventory.items().find((i) => i.name === 'flint_and_steel'), 'hand');
    await waitFor(() => blockName(new Vec3(X, Y - 1, Z)) === 'obsidian', 5000, 'the frame on the client');
    // light it: use the flint and steel on the top face of the frame's bottom
    await op.activateBlock(op.blockAt(new Vec3(X, Y - 1, Z)), new Vec3(0, 1, 0));
    await waitFor(() => blockName(new Vec3(X, Y + 2, Z)) === 'nether_portal', 5000, 'the portal to light').catch(() => {});
    let lit = 0;
    for (let dx = 0; dx <= 1; dx++) for (let dy = 0; dy <= 2; dy++) lit += blockName(new Vec3(X + dx, Y + dy, Z)) === 'nether_portal';
    console.log(`  lit the frame: ${lit}/6 portal blocks`);
    assert.strictEqual(lit, 6, 'the frame did not light');

    // survival: 4 s in the portal
    await command('/gamemode survival', /game mode/i);
    const t0 = Date.now();
    await command(`/tp Tester ${X + 1}.0 ${Y} ${Z}.5`, /Teleported/);
    let pos = await arrive('the_nether', 'through the portal');
    const took = (Date.now() - t0) / 1000;
    const inside = blockName(pos.floored());
    console.log(`  in the Nether after ${took.toFixed(1)} s at ${pos.floored()}, standing in ${inside} on ` +
      `${blockName(pos.floored().offset(0, -1, 0))}`);
    assert(took > 3.5, `travelled too soon (${took.toFixed(1)} s, survival needs 4 s)`);
    assert.strictEqual(inside, 'nether_portal', 'not arrived in a portal');
    assert(Math.abs(pos.x - (X + 1) / 8) < 18 && Math.abs(pos.z - Z / 8) < 18, `not near the overworld position / 8 (${pos})`);
    const netherPortal = pos.floored();

    // out of it, wait for the cooldown, back in: to the first portal
    await command(`/gamemode creative`, /game mode/i);
    await command(`/tp Tester ${netherPortal.x + 0.5} ${netherPortal.y + 6} ${netherPortal.z + 4.5}`, /Teleported/);
    await sleep(16000);
    await command(`/tp Tester ${pos.x} ${pos.y} ${pos.z}`, /Teleported/);
    pos = await arrive('overworld', 'back through the portal');
    console.log(`  back in the overworld at ${pos.floored()}, standing in ${blockName(pos.floored())}`);
    assert(Math.abs(pos.x - (X + 1)) <= 1.5 && Math.abs(pos.z - (Z + 0.5)) <= 1.5 && Math.abs(pos.y - Y) <= 1,
      `not back at the first portal (${pos})`);

    // break the frame: the portal goes
    await command(`/tp Tester ${X + 1}.5 ${Y + 1} ${Z - 3}.5`, /Teleported/);
    await sleep(1000);
    await command(`/setblock ${X + 2} ${Y + 1} ${Z} air`, /Changed/i);
    await sleep(1500);
    let left = 0;
    for (let dx = 0; dx <= 1; dx++) for (let dy = 0; dy <= 2; dy++) left += blockName(new Vec3(X + dx, Y + dy, Z)) === 'nether_portal';
    console.log(`  frame broken: ${left} portal blocks left`);
    assert.strictEqual(left, 0, 'the portal survived its frame');
    console.log(`NETHER PORTAL OK (${host})`);
  } finally {
    if (op) op.quit();
    if (server) await server.stop();
    if (worldFile) fs.rmSync(worldFile, { force: true });
  }
})().catch((err) => { console.error(err.message || err); process.exitCode = 1; });
