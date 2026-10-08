'use strict';
// Nether mobs with a real client, in an arena built in the Nether:
//  - a ghast shoots fireballs at the player; hitting one back kills the ghast
//  - a magma cube jumps at the player and hurts them
//  - zombified piglins leave the player alone until one is hit; then they all attack
//  - natural spawning brings Nether mobs
//   node nether_mobs.js --host 192.168.1.160 [--x 2000 --z 2000]
//   SERVER_BIN=~/mc-host-build/mcserver node nether_mobs.js --local
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
if (!host && !local) { console.error('usage: node nether_mobs.js --host <ip> | --local'); process.exit(2); }
const PORT = local ? 25600 + Math.floor(Math.random() * 300) : 25565;
const X = Number(opt('x', local ? '300' : '2000')), Z = Number(opt('z', local ? '300' : '2000')), Y = 80;
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

async function clearArena() {
  await command(`/kill @e[type=!player]`, /Killed/);
}

(async () => {
  try {
    if (local) {
      worldFile = path.join(os.tmpdir(), `mc-nmobs-${process.pid}.img`);
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
    if (op.game.dimension !== 'the_nether') {
      await command('/dimension the_nether', /Mov(ed|ing) Tester/);
      await arrive(op, 'the_nether');
    }
    await command(`/tp Tester ${X} ${Y + 14} ${Z - 20}`, /Teleported/);
    await sleep(4000);
    // the arena: 37 x 37, 13 high inside, netherrack floor, glass walls and roof
    await command(`/fill ${X - 19} ${Y - 1} ${Z - 19} ${X + 19} ${Y + 14} ${Z + 19} glass`, /filled/i);
    await command(`/fill ${X - 18} ${Y} ${Z - 18} ${X + 18} ${Y + 13} ${Z + 18} air`, /filled/i);
    await command(`/fill ${X - 18} ${Y - 1} ${Z - 18} ${X + 18} ${Y - 1} ${Z + 18} netherrack`, /filled/i);

    runner = await connectBot(PORT, 'Runner', { host, checkTimeoutInterval: 600000 });
    runner.on('error', () => {});
    await command('/dimension the_nether Runner', /Mov(ed|ing) Runner/);
    await arrive(runner, 'the_nether');
    await command('/gamemode survival Runner', /game mode/i);
    await command(`/tp Runner ${X}.5 ${Y} ${Z}.5`, /Teleported/);
    await sleep(2000);

    // 1) a ghast
    await command('/heal Runner', /heal/i).catch(() => {});
    await command(`/summon ghast ${X - 16}.5 ${Y + 2} ${Z}.5`, /Summoned/i);
    let ghastDead = false, fireballs = 0, deflected = 0;
    const seen = new Set();
    const t0 = Date.now();
    while (Date.now() - t0 < 60000 && !ghastDead) {
      for (const f of mobsNear(runner, 'fireball', 40)) {
        if (!seen.has(f.id)) { seen.add(f.id); fireballs++; }
        if (f.position.distanceTo(runner.entity.position.offset(0, 1.6, 0)) < 3.5 && !f.hitBack) {
          f.hitBack = true;
          const g = mobsNear(runner, 'ghast', 40)[0];
          if (g) await runner.lookAt(g.position.offset(0, 2, 0), true);
          runner.attack(f);
          deflected++;
        }
      }
      ghastDead = mobsNear(runner, 'ghast', 60).length === 0;
      await sleep(50);
    }
    results.push(`ghast: ${fireballs} fireballs, ${deflected} hit back, ghast ${ghastDead ? 'killed' : 'alive'}, ` +
      `runner health ${runner.health}`);
    console.log('  ' + results[results.length - 1]);
    assert(fireballs > 0, 'the ghast did not shoot');
    assert(ghastDead, 'a fireball sent back did not kill the ghast');

    // 2) a magma cube
    await command('/heal Runner', /heal/i).catch(() => {});
    await clearArena();
    await sleep(1000);
    const h1 = runner.health;
    await command(`/summon magma_cube ${X + 6}.5 ${Y} ${Z}.5`, /Summoned/i);
    let hurt = false;
    await waitFor(() => (hurt = runner.health < h1), 40000, 'the magma cube hurting the player').catch(() => {});
    const cube = mobsNear(runner, 'magma_cube', 30)[0];
    results.push(`magma cube: ${hurt ? 'hurt the player' : 'did not hurt'} (health ${h1} -> ${runner.health}), ` +
      `${cube ? 'at ' + cube.position.floored() : 'gone'}`);
    console.log('  ' + results[results.length - 1]);
    assert(hurt, 'the magma cube did not hurt the player');

    // 3) zombified piglins: peaceful until one is hit
    await clearArena();
    await command('/heal Runner', /heal/i).catch(() => {});
    await command(`/tp Runner ${X}.5 ${Y} ${Z}.5`, /Teleported/);
    await sleep(1000);
    await command(`/summon zombified_piglin ${X + 2}.5 ${Y} ${Z}.5`, /Summoned/i);
    await command(`/summon zombified_piglin ${X - 12}.5 ${Y} ${Z + 10}.5`, /Summoned/i);
    await command(`/summon zombified_piglin ${X + 12}.5 ${Y} ${Z - 10}.5`, /Summoned/i);
    await sleep(6000);
    const h2 = runner.health;
    assert.strictEqual(h2, 20, 'zombified piglins attacked unprovoked');
    const piglins = mobsNear(runner, 'zombified_piglin', 30);
    const far0 = piglins.map((p) => p.position.distanceTo(runner.entity.position)).sort((a, b) => b - a)[0];
    const near = piglins.sort((a, b) => a.position.distanceTo(runner.entity.position) - b.position.distanceTo(runner.entity.position))[0];
    // it may have wandered off: step next to it, then hit it (within reach)
    if (near.position.distanceTo(runner.entity.position) > 2.5) {
      await command(`/tp Runner ${near.position.x.toFixed(1)} ${near.position.y} ${(near.position.z - 1.5).toFixed(1)}`, /Teleported/);
      await sleep(500);
    }
    await runner.lookAt(near.position.offset(0, 1.5, 0), true);
    const nearHealthHits = [];
    runner.on('entityHurt', (e) => { if (e === near) nearHealthHits.push(Date.now()); });
    runner.attack(near);
    await sleep(300);
    if (!nearHealthHits.length) { await runner.lookAt(near.position.offset(0, 1.5, 0), true); runner.attack(near); }
    await waitFor(() => runner.health < h2, 20000, 'the piglins attacking').catch(() => {});
    await sleep(3000);
    const far1 = mobsNear(runner, 'zombified_piglin', 40).map((p) => p.position.distanceTo(runner.entity.position)).sort((a, b) => b - a)[0];
    results.push(`zombified piglins: no attack for 6 s; after one was hit, health ${h2} -> ${runner.health}; ` +
      `the farthest came from ${far0.toFixed(1)} to ${far1 === undefined ? '-' : far1.toFixed(1)} blocks`);
    console.log('  ' + results[results.length - 1]);
    assert(runner.health < h2, 'the piglins did not attack after one was hit');
    assert(far1 !== undefined && far1 < far0 - 4, 'the others did not join in');

    // 4) natural spawning around the spectator (outside the arena)
    await clearArena();
    await command('/gamemode creative Runner', /game mode/i);
    const t1 = Date.now();
    let natural = [];
    await waitFor(() => {
      natural = Object.values(op.entities).filter((e) => ['zombified_piglin', 'ghast', 'magma_cube'].includes(e.name));
      return natural.length >= 2;
    }, 120000, 'natural Nether spawns').catch(() => {});
    const kinds = {};
    for (const e of natural) kinds[e.name] = (kinds[e.name] || 0) + 1;
    results.push(`natural spawning after ${((Date.now() - t1) / 1000).toFixed(0)} s: ${JSON.stringify(kinds)}`);
    console.log('  ' + results[results.length - 1]);
    assert(natural.length >= 2, 'no Nether mobs spawned');
    console.log(`NETHER MOBS OK (${host})`);
  } finally {
    for (const b of [runner, op]) if (b) b.quit();
    if (server) await server.stop();
    if (worldFile) fs.rmSync(worldFile, { force: true });
  }
})().catch((err) => { console.error(err.message || err); process.exitCode = 1; });
