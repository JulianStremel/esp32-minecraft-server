'use strict';
// A Nether fortress's mobs and the end of the game, with real clients:
//  - a blaze charges and shoots small fireballs that hurt; killed by a player it drops blaze rods
//  - a wither skeleton's hit withers the player (the effect, black hearts)
//  - on nether bricks natural spawning brings the fortress's mobs (blazes, wither skeletons)
//  - the End's exit portal shows the credits the first time, then sends the player home
//   node fortress_mobs.js --host 192.168.1.160 [--x 300 --z 0] [--from <step>]
//   SERVER_BIN=~/mc-host-build/mcserver node fortress_mobs.js --local
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
if (!host && !local) { console.error('usage: node fortress_mobs.js --host <ip> | --local'); process.exit(2); }
const PORT = local ? 25600 + Math.floor(Math.random() * 300) : 25565;
const X = Number(opt('x', '300')), Z = Number(opt('z', '0')), Y = 80;
const SX = -X;   // the nether brick floors for natural spawning
const FROM = Number(opt('from', '1'));   // the first step to run
let server, op, runner, worldFile;

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
const report = (line) => console.log('  ' + line);

async function clearArena() {
  await command(`/kill @e[type=!player]`, /Killed|No entity/);
}

async function heal() {
  await command('/heal Runner', /heal/i).catch(() => {});
  await command(`/tp Runner ${X}.5 ${Y} ${Z}.5`, /Teleported/);
  await sleep(1000);
}

(async () => {
  try {
    if (local) {
      worldFile = path.join(os.tmpdir(), `mc-fmobs-${process.pid}.img`);
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
      await sleep(3000);
    }
    await command(`/tp Tester ${X} ${Y + 8} ${Z - 22}`, /Teleported/);
    await sleep(4000);
    // the arena: 37 x 37, 4 high inside (a blaze cannot rise out of reach), netherrack floor
    await command(`/fill ${X - 19} ${Y - 1} ${Z - 19} ${X + 19} ${Y + 4} ${Z + 19} glass`, /filled/i);
    await command(`/fill ${X - 18} ${Y} ${Z - 18} ${X + 18} ${Y + 3} ${Z + 18} air`, /filled/i);
    await command(`/fill ${X - 18} ${Y - 1} ${Z - 18} ${X + 18} ${Y - 1} ${Z + 18} netherrack`, /filled/i);

    runner = await connectBot(PORT, 'Runner', { host, checkTimeoutInterval: 600000 });
    runner.on('error', () => {});
    runner._client.on('error', () => {});
    await command('/dimension the_nether Runner', /Mov(ed|ing) Runner/);
    await sleep(2000);
    await command('/gamemode survival Runner', /game mode/i);
    await command(`/tp Runner ${X}.5 ${Y} ${Z}.5`, /Teleported/);
    await arrive(runner, 'the_nether');
    await clearArena();

    // 1) a blaze: charged, three fireballs, damage
    if (FROM <= 1) {
      const fireballs = new Set();
      let burning = false;
      runner._client.on('entity_metadata', (p) => {
        if (runner.entity && p.entityId === runner.entity.id &&
            p.metadata.some((m) => m.key === 0 && (Number(m.value) & 1))) burning = true;
      });
      // 6 blocks away (vanilla's spread makes a blaze miss most shots from farther)
      await command(`/summon blaze ${X + 6}.5 ${Y} ${Z}.5`, /Summoned/i);
      const h0 = runner.health, t0 = Date.now();
      let low = h0;   // (the runner heals between volleys)
      while (Date.now() - t0 < 60000 && !(fireballs.size >= 3 && low < h0)) {
        for (const f of mobsNear(runner, 'small_fireball', 40)) fireballs.add(f.id);
        low = Math.min(low, runner.health);
        await sleep(50);
      }
      report(`blaze: ${fireballs.size} small fireballs in ${((Date.now() - t0) / 1000).toFixed(1)} s, ` +
        `health ${h0} -> ${low} at the lowest, on fire: ${burning ? 'yes' : 'no'}`);
      assert(fireballs.size >= 3, 'the blaze did not shoot');
      assert(low < h0, 'the fireballs did not hurt');
    }

    // 2) killed by the player, blazes drop rods (one in two); each in a glass niche open
    // towards the player, so a hit does not push it away
    if (FROM <= 2) {
      await clearArena();
      await heal();
      await command(`/fill ${X + 2} ${Y} ${Z - 1} ${X + 3} ${Y + 3} ${Z + 1} glass`, /filled/i);
      await command(`/fill ${X + 2} ${Y} ${Z} ${X + 2} ${Y + 3} ${Z} air`, /filled/i);
      await command('/give Runner diamond_sword', /Gave|give/i);
      await sleep(500);
      await runner.equip(runner.inventory.items().find((i) => i.name === 'diamond_sword'), 'hand');
      let rods = 0, killed = 0;
      const dead = new Set();
      runner.on('entityDead', (e) => dead.add(e.id));
      for (let n = 0; n < 8 && rods === 0; n++) {
        await command('/heal Runner', /heal/i).catch(() => {});
        await command(`/summon blaze ${X + 2}.5 ${Y} ${Z}.5`, /Summoned/i);
        await sleep(300);
        const t = Date.now();
        const blaze = mobsNear(runner, 'blaze', 8)[0];
        while (blaze && Date.now() - t < 15000 && !dead.has(blaze.id)) {
          await runner.lookAt(blaze.position.offset(0, 1.2, 0), true);
          runner.attack(blaze);
          await sleep(650);
        }
        if (blaze && dead.has(blaze.id)) killed++;
        await sleep(1500);
        rods = runner.inventory.items().filter((i) => i.name === 'blaze_rod').reduce((a, i) => a + i.count, 0);
        rods += Object.values(runner.entities).filter((e) => e.name === 'item' && e.getDroppedItem &&
          (e.getDroppedItem() || {}).name === 'blaze_rod').length;   // not picked up yet
      }
      report(`blaze rods: ${rods} after ${killed} blazes killed`);
      assert(killed > 0, 'no blaze was killed');
      assert(rods > 0, 'no blaze rods');
      await command(`/fill ${X + 2} ${Y} ${Z - 1} ${X + 3} ${Y + 3} ${Z + 1} air`, /filled/i);
    }

    // 3) a wither skeleton: the wither effect
    if (FROM <= 3) {
      await clearArena();
      await heal();
      let withered = false;
      runner._client.on('entity_effect', (p) => { if (p.entityId === runner.entity.id && p.effectId === 19) withered = true; });
      await command(`/summon wither_skeleton ${X + 4}.5 ${Y} ${Z}.5`, /Summoned/i);
      const h2 = runner.health, t2 = Date.now();
      await waitFor(() => withered, 30000, 'the wither effect').catch(() => {});
      await sleep(500);
      report(`wither skeleton: health ${h2} -> ${runner.health}, wither ${withered ? 'yes' : 'no'} after ` +
        `${((Date.now() - t2) / 1000).toFixed(1)} s`);
      assert(withered, 'no wither effect');
      await clearArena();
    }

    // 4) natural spawning on nether bricks: four floors of 80 x 80 under the players
    if (FROM <= 4) {
      await command('/gamemode creative Runner', /game mode/i);   // creative players count for spawning, spectators not
      await command(`/tp Runner ${SX} 100 ${Z}`, /Teleported/);
      await command(`/tp Tester ${SX} 100 ${Z}`, /Teleported/);
      await sleep(5000);
      for (const fy of [64, 70, 76, 82]) {
        await command(`/fill ${SX - 40} ${fy} ${Z - 40} ${SX + 39} ${fy} ${Z + 39} nether_bricks`, /filled/i);
        await command(`/fill ${SX - 40} ${fy + 1} ${Z - 40} ${SX + 39} ${fy + 4} ${Z + 39} air`, /filled/i);
      }
      await clearArena();
      const t3 = Date.now();
      const kinds = () => ['blaze', 'wither_skeleton', 'skeleton', 'zombified_piglin', 'magma_cube']
        .map((k) => [k, mobsNear(runner, k, 128).filter((e) => e.position.y < 88 && Math.abs(e.position.x - SX) < 41).length]);
      let found = [];
      await waitFor(() => (found = kinds()).some(([k, n]) => (k === 'blaze' || k === 'wither_skeleton') && n > 0), 180000,
        'fortress mobs').catch(() => {});
      report(`natural spawning on nether bricks after ${((Date.now() - t3) / 1000).toFixed(0)} s: ` +
        found.filter(([, n]) => n).map(([k, n]) => `${n} ${k}`).join(', '));
      assert(found.some(([k, n]) => (k === 'blaze' || k === 'wither_skeleton') && n > 0), 'no fortress mobs spawned');
    }

    // 5) the End's exit portal: the credits, then home
    await clearArena();
    await command('/dimension the_end', /Mov(ed|ing) Tester/);   // commands act in the operator's dimension
    await sleep(2000);
    await command(`/tp Tester ${X} ${Y + 6} ${Z - 6}`, /Teleported/);
    await sleep(3000);
    await command(`/fill ${X - 1} ${Y - 1} ${Z - 1} ${X + 1} ${Y - 1} ${Z + 1} obsidian`, /filled/i);
    await command('/dimension the_end Runner', /Mov(ed|ing) Runner/);
    await arrive(runner, 'the_end');
    // mineflayer ends the credits at once: here the test does, after a look around
    runner._client.removeAllListeners('game_state_change');
    const wins = [];
    runner._client.on('game_state_change', (p) => {
      if (p.reason === 4 || p.reason === 'win_game') wins.push(p.gameMode);
    });
    for (let round = 0; round < 2; round++) {
      await command(`/setblock ${X} ${Y} ${Z} end_portal`, /Changed|set/i);
      await command(`/tp Runner ${X}.5 ${Y} ${Z}.5`, /Teleported/);
      await waitFor(() => wins.length > round, 15000, 'the game won');
      await sleep(1000);
      assert.strictEqual(runner.game.dimension, 'the_end', 'home before the credits ended');
      runner._client.write('client_command', { actionId: 0 });   // the credits are over
      await arrive(runner, 'overworld');
      report(`exit portal ${round + 1}: the game won (${wins[round] === 1 ? 'the credits' : 'no credits'}), home at ` +
        `${runner.entity.position.floored()}`);
      if (round === 0) {
        await command('/dimension the_end Runner', /Mov(ed|ing) Runner/);
        await arrive(runner, 'the_end');
      }
    }
    assert.deepStrictEqual(wins, [1, 0], 'the credits once, then not again');
    console.log(`FORTRESS MOBS OK (${host})`);
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
