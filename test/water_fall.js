'use strict';
// Falling into water, swimming to the edge and climbing out onto land must not hurt.
//   node water_fall.js --host 192.168.1.160 [--x 9000 --z 9000]
//   SERVER_BIN=/tmp/mc-host-build/mcserver node water_fall.js --local
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
if (!host && !local) { console.error('usage: node water_fall.js --host <ip> | --local'); process.exit(2); }
const PORT = local ? 25600 + Math.floor(Math.random() * 300) : 25565;
const X = Number(opt('x', local ? '40' : '9000')), Z = Number(opt('z', local ? '40' : '9000')), Y = 190;
let server, op, diver, worldFile;

async function command(text, pattern, ms = 15000) {
  const r = nextChat(op, pattern, ms);
  op.chat(text);
  const line = await r;
  await sleep(650);
  return line;
}

(async () => {
  try {
    if (local) {
      worldFile = path.join(os.tmpdir(), `mc-water-${process.pid}.img`);
      server = startServer(['--port', String(PORT), '--ops', 'Tester', '--file', worldFile, '--size', '256', '--no-mobs',
        '--format']);
      await server.ready;
      host = 'localhost';
    }
    op = await connectBot(PORT, 'Tester', { host, checkTimeoutInterval: 600000 });
    op.on('error', () => {});
    op.physicsEnabled = false;
    await command('/gamemode spectator', /game mode/i);
    await command(`/tp Tester ${X} ${Y + 10} ${Z - 6}`, /Teleported/);
    await sleep(3000);
    // a 5 x 5 pool, 10 deep (the diver never touches its floor), its surface level with the land around it (land top at Y)
    await command(`/fill ${X - 6} ${Y - 11} ${Z - 6} ${X + 6} ${Y + 12} ${Z + 6} air`, /filled|Success/i).catch(() => {});
    await command(`/fill ${X - 6} ${Y - 11} ${Z - 6} ${X + 6} ${Y} ${Z + 6} stone`, /filled/i);
    await command(`/fill ${X - 2} ${Y - 9} ${Z - 2} ${X + 2} ${Y} ${Z + 2} water`, /filled/i);

    diver = await connectBot(PORT, 'Diver', { host, checkTimeoutInterval: 600000 });
    diver.on('error', () => {});
    await command('/gamemode survival Diver', /game mode/i);
    await command('/heal Diver', /heal/i).catch(() => {});
    await command('/feed Diver', /fed|feed/i).catch(() => {});
    await sleep(1000);
    const h0 = diver.health;
    // fall 10 blocks into the pool
    await command(`/tp Diver ${X}.5 ${Y + 11} ${Z}.5`, /Teleported/);
    await waitFor(() => diver.entity.isInWater, 15000, 'Diver in the water');
    diver.setControlState('jump', true);   // swim up at once, as a player does
    let lowest = diver.entity.position.y;
    const t = Date.now();
    while (Date.now() - t < 2500) {
      lowest = Math.min(lowest, diver.entity.position.y);
      assert(!diver.entity.onGround, 'Diver touched the pool floor');
      await sleep(50);
    }
    console.log(`  sank to y ${lowest.toFixed(1)} (pool floor ${Y - 9})`);
    const hWater = diver.health;
    // swim to the +x edge and climb out
    await diver.lookAt(new Vec3(X + 5, Y + 1.6, Z + 0.5), true);
    diver.setControlState('forward', true);
    diver.setControlState('jump', true);
    await waitFor(() => diver.entity.onGround && !diver.entity.isInWater && diver.entity.position.x > X + 2.6, 15000,
      'Diver on land').catch(() => {});
    diver.clearControlStates();
    await sleep(2000);
    const p = diver.entity.position;
    console.log(`  health ${h0} -> ${hWater} in the water -> ${diver.health} on land at ${p.floored()}`);
    assert(p.x > X + 2.6 && p.y >= Y + 0.9, `Diver did not get out of the water (at ${p})`);
    assert.strictEqual(hWater, h0, 'hurt by landing in water');
    assert.strictEqual(diver.health, h0, 'hurt climbing out of the water');
    console.log(`WATER FALL OK (${host})`);
  } finally {
    for (const b of [diver, op]) if (b) b.quit();
    if (server) await server.stop();
    if (worldFile) fs.rmSync(worldFile, { force: true });
  }
})().catch((err) => { console.error(err.message || err); process.exitCode = 1; });
