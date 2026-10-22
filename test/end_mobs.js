'use strict';
// The End's mobs with a real client, in an arena built over the End's void:
//  - an enderman leaves the player alone until the player looks it in the eyes; then it attacks
//  - a shulker opens and shoots bullets that hurt and make the player float up (levitation)
//  - a thrown ender pearl takes the player where it lands
//  - natural spawning brings endermen to the main island
//   node end_mobs.js --host 192.168.1.160 [--x 300 --z 0]
//   SERVER_BIN=~/mc-host-build/mcserver node end_mobs.js --local
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
if (!host && !local) { console.error('usage: node end_mobs.js --host <ip> | --local'); process.exit(2); }
const PORT = local ? 25600 + Math.floor(Math.random() * 300) : 25565;
const X = Number(opt('x', '300')), Z = Number(opt('z', '0')), Y = 80;
let server, op, runner, worldFile;
const results = [];

async function command(text, pattern, ms = 15000) {
  const r = nextChat(op, pattern, ms);
  op.chat(text);
  const line = await r;
  await sleep(650);
  return line;
}

async function arrive(bot, dim) {
  await waitFor(() => bot.game.dimension === dim, 30000, `${bot.username} in ${dim}`);
  await waitFor(() => bot.entity && bot.blockAt(bot.entity.position.offset(0, -1, 0)), 20000, 'chunks');
  await sleep(1000);
}

const mobsNear = (bot, name, r = 64) => Object.values(bot.entities).filter((e) => e.name === name && e.position &&
  e.position.distanceTo(bot.entity.position) < r);
const report = (line) => { results.push(line); console.log('  ' + line); };

async function clearArena() {
  await command(`/kill @e[type=!player]`, /Killed|No entity/);
}

(async () => {
  try {
    if (local) {
      worldFile = path.join(os.tmpdir(), `mc-emobs-${process.pid}.img`);
      server = startServer(['--port', String(PORT), '--seed', '42', '--ops', 'Tester', '--file', worldFile, '--size', '256',
        '--format'], { log: args.includes('--log') });
      await server.ready;
      host = 'localhost';
    }
    op = await connectBot(PORT, 'Tester', { host, checkTimeoutInterval: 600000 });
    op.on('error', () => {});
    op.physicsEnabled = false;
    await command('/gamemode spectator', /game mode/i);
    await command('/difficulty normal', /difficulty/i);
    if (op.game.dimension !== 'the_end') {
      await command('/dimension the_end', /Mov(ed|ing) Tester/);
      await sleep(3000);
    }
    await command(`/tp Tester ${X} ${Y + 14} ${Z - 20}`, /Teleported/);
    await sleep(4000);
    // the arena: 37 x 37, 13 high inside, an end stone floor, glass walls and roof
    await command(`/fill ${X - 19} ${Y - 1} ${Z - 19} ${X + 19} ${Y + 14} ${Z + 19} glass`, /filled/i);
    await command(`/fill ${X - 18} ${Y} ${Z - 18} ${X + 18} ${Y + 13} ${Z + 18} air`, /filled/i);
    await command(`/fill ${X - 18} ${Y - 1} ${Z - 18} ${X + 18} ${Y - 1} ${Z + 18} end_stone`, /filled/i);

    runner = await connectBot(PORT, 'Runner', { host, checkTimeoutInterval: 600000 });
    runner.on('error', () => {});
    await command('/dimension the_end Runner', /Mov(ed|ing) Runner/);
    await sleep(2000);
    await command('/gamemode survival Runner', /game mode/i);
    await command(`/tp Runner ${X}.5 ${Y} ${Z}.5`, /Teleported/);
    await arrive(runner, 'the_end');

    // 1) an enderman: peaceful until stared at
    await command(`/summon enderman ${X + 8}.5 ${Y} ${Z}.5`, /Summoned/i);
    await sleep(500);
    await runner.lookAt(new Vec3(X - 10, Y, Z), true);   // away from it
    await sleep(6000);
    assert.strictEqual(runner.health, 20, 'the enderman attacked unprovoked');
    let em = mobsNear(runner, 'enderman', 40)[0];
    assert(em, 'the enderman is gone');
    const h0 = runner.health;
    const t0 = Date.now();
    // look it in the eyes until it is angry (its mouth opens: metadata), then look away
    let angry = false;
    runner._client.on('entity_metadata', (p) => {
      if (em && p.entityId === em.id && p.metadata.some((m) => m.key === 17 && m.value === true)) angry = true;
    });
    while (!angry && Date.now() - t0 < 15000) {
      em = mobsNear(runner, 'enderman', 40)[0] || em;
      await runner.lookAt(em.position.offset(0, 2.55, 0), true);
      await sleep(100);
    }
    await runner.lookAt(new Vec3(runner.entity.position.x, Y - 1, runner.entity.position.z + 3), true);
    await waitFor(() => runner.health < h0, 30000, 'the enderman attacking').catch(() => {});
    report(`enderman: no attack for 6 s; stared at: ${angry ? 'angry' : 'not angry'} after ${((Date.now() - t0) / 1000).toFixed(1)} s, ` +
      `health ${h0} -> ${runner.health}`);
    assert(angry, 'staring did not anger the enderman');
    assert(runner.health < h0, 'the angry enderman did not attack');

    // 2) a shulker: bullets, levitation
    await clearArena();
    await command('/heal Runner', /heal/i).catch(() => {});
    await command(`/tp Runner ${X}.5 ${Y} ${Z}.5`, /Teleported/);
    await sleep(1000);
    let levitated = false, bullets = 0;
    const seen = new Set();
    runner._client.on('entity_effect', (p) => { if (p.entityId === runner.entity.id && p.effectId === 24) levitated = true; });
    await command(`/summon shulker ${X + 6}.5 ${Y} ${Z}.5`, /Summoned/i);
    const h1 = runner.health, y1 = runner.entity.position.y;
    let maxY = y1;
    const t1 = Date.now();
    while (Date.now() - t1 < 40000 && !(levitated && runner.health < h1 && maxY > y1 + 1.5)) {
      for (const b of mobsNear(runner, 'shulker_bullet', 30)) seen.add(b.id);
      bullets = seen.size;
      maxY = Math.max(maxY, runner.entity.position.y);
      await sleep(50);
    }
    report(`shulker: ${bullets} bullets seen, health ${h1} -> ${runner.health}, levitation ${levitated ? 'yes' : 'no'}, ` +
      `rose ${(maxY - y1).toFixed(1)} blocks`);
    assert(bullets > 0, 'the shulker did not shoot');
    assert(runner.health < h1, 'the bullets did not hurt');
    assert(levitated, 'no levitation');

    // 3) an ender pearl
    await clearArena();
    await sleep(12000);   // the levitation ends, the runner lands
    await command('/heal Runner', /heal/i).catch(() => {});
    await command(`/tp Runner ${X}.5 ${Y} ${Z}.5`, /Teleported/);
    await sleep(1500);
    await command('/give Runner ender_pearl 2', /Gave|give/i);
    await sleep(500);
    await runner.equip(runner.inventory.items().find((i) => i.name === 'ender_pearl'), 'hand');
    await runner.lookAt(new Vec3(X - 12, Y + 2, Z + 0.5), true);
    const from = runner.entity.position.clone(), h2 = runner.health;
    runner.activateItem();
    await waitFor(() => runner.entity.position.distanceTo(from) > 5, 10000, 'the pearl teleport').catch(() => {});
    await sleep(500);
    report(`ender pearl: from ${from.floored()} to ${runner.entity.position.floored()}, health ${h2} -> ${runner.health}`);
    assert(runner.entity.position.distanceTo(from) > 5, 'the pearl did not teleport the player');
    assert(runner.health <= h2 - 4, 'the pearl did no fall damage');

    // 4) natural spawning on the main island
    await clearArena();
    await command('/gamemode creative Runner', /game mode/i);   // creative players count for spawning, spectators not
    await command('/tp Runner 0 90 40', /Teleported/);
    await command('/tp Tester 0 90 40', /Teleported/);
    const t3 = Date.now();
    let natural = [];
    await waitFor(() => (natural = mobsNear(runner, 'enderman', 128)).length >= 2, 120000, 'endermen spawning').catch(() => {});
    report(`natural spawning: ${natural.length} endermen after ${((Date.now() - t3) / 1000).toFixed(0)} s`);
    assert(natural.length >= 2, 'no endermen spawned');
    console.log(`END MOBS OK (${host})`);
  } finally {
    // back home: the next test's players start where these left off
    if (op) {
      try {
        await command('/dimension overworld Runner', /./, 5000);
        await command('/dimension overworld Tester', /./, 5000);
      } catch (e) {}
    }
    for (const b of [runner, op]) if (b) b.quit();
    if (server) await server.stop();
    if (worldFile) fs.rmSync(worldFile, { force: true });
  }
})().catch((err) => { console.error(err.message || err); process.exitCode = 1; });
