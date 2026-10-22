'use strict';
// A villager's day with a real client, in a glass pen with three beds, a lectern and a bell:
//  - by day: bread on the floor, two villagers eat and share it, then have a child (the
//    Baby metadata)
//  - at night: they walk to their beds and sleep (the sleeping pose, the bed in their
//    metadata)
//  - in the morning: the librarian walks to its lectern
//   node villager_day.js --host 192.168.1.160
//   SERVER_BIN=~/mc-host-build/mcserver node villager_day.js --local
const assert = require('assert');
const fs = require('fs');
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
if (!host && !local) { console.error('usage: node villager_day.js --host <ip> | --local'); process.exit(2); }
const PORT = local ? 25600 + Math.floor(Math.random() * 300) : 25565;
const X = Number(opt('x', '220')), Z = Number(opt('z', '220')), Y = 120;
const POSE = 6, SLEEPING_POS = 14, BABY = 16, VILLAGER_DATA = 18, SLEEPING = 2, LIBRARIAN = 9;
let server, op, runner, worldFile;

async function command(text, pattern, ms = 15000) {
  const r = nextChat(op, pattern, ms);
  op.chat(text);
  const line = await r;
  await sleep(400);
  return line;
}
const meta = new Map();
function watch(bot) {
  bot._client.on('entity_metadata', (p) => {
    const m = meta.get(p.entityId) || {};
    for (const e of p.metadata) m[e.key] = e.value;
    meta.set(p.entityId, m);
  });
}
const inPen = (e) => e.position.x > X - 4 && e.position.x < X + 5 && e.position.z > Z && e.position.z < Z + 7;
const villagers = () => Object.values(runner.entities).filter((e) => e.name === 'villager' && inPen(e));
const m = (e) => meta.get(e.id) || {};
const poseName = (v) => (typeof v === 'string' ? v : v === SLEEPING ? 'sleeping' : String(v));

(async () => {
  try {
    if (local) {
      worldFile = path.join(os.tmpdir(), `mc-villager-day-${process.pid}.img`);
      server = startServer(['--port', String(PORT), '--seed', '42', '--ops', 'Tester', '--file', worldFile, '--size', '256',
        '--no-mobs', '--flat', '--format'], { log: args.includes('--log') });
      await server.ready;
      host = 'localhost';
    }
    op = await connectBot(PORT, 'Tester', { host, checkTimeoutInterval: 600000 });
    op.on('error', () => {});
    op.physicsEnabled = false;
    await command('/gamemode spectator', /game mode/i);
    runner = await connectBot(PORT, 'Runner', { host, checkTimeoutInterval: 600000 });
    runner.on('error', () => {});
    watch(runner);
    await command('/gamemode creative Runner', /game mode/i);
    await command('/time set 1000', /time/i);
    await command(`/tp Tester ${X} ${Y + 8} ${Z - 4}`, /Teleported/);
    await sleep(2000);
    await command(`/fill ${X - 6} ${Y - 1} ${Z - 4} ${X + 6} ${Y - 1} ${Z + 9} stone`, /filled/i);
    await command(`/fill ${X - 6} ${Y} ${Z - 4} ${X + 6} ${Y + 4} ${Z + 9} air`, /filled/i);
    // the pen: inside x -3..3, z 1..5
    await command(`/fill ${X - 4} ${Y} ${Z} ${X + 4} ${Y + 2} ${Z + 6} glass`, /filled/i);
    await command(`/fill ${X - 3} ${Y} ${Z + 1} ${X + 3} ${Y + 2} ${Z + 5} air`, /filled/i);
    for (const bx of [X - 3, X - 1, X + 1]) {
      await command(`/setblock ${bx} ${Y} ${Z + 4} red_bed[part=foot,facing=south]`, /Changed|set/i);
      await command(`/setblock ${bx} ${Y} ${Z + 5} red_bed[part=head,facing=south]`, /Changed|set/i);
    }
    await command(`/setblock ${X + 3} ${Y} ${Z + 1} lectern`, /Changed|set/i);
    await command(`/setblock ${X - 3} ${Y} ${Z + 1} bell`, /Changed|set/i);
    await command(`/summon villager ${X - 1}.5 ${Y} ${Z + 2}.5`, /Summoned/);
    await command(`/summon villager ${X + 1}.5 ${Y} ${Z + 2}.5`, /Summoned/);
    await command(`/tp Runner ${X}.5 ${Y} ${Z + 2}.5`, /Teleported/);
    await waitFor(() => villagers().length === 2, 10000, 'two villagers');

    // 1) bread: eaten, shared, a child
    await command('/give Runner bread 10', /Gave/);
    await sleep(800);
    const bread = runner.inventory.items().find((i) => i.name === 'bread');
    await runner.toss(bread.type, null, 10);
    const t0 = Date.now();
    await waitFor(() => villagers().some((v) => m(v)[BABY] === true), 60000, 'a child').catch(() => {});
    const child = villagers().find((v) => m(v)[BABY] === true);
    console.log(`  bread for two: ${child ? `a child after ${((Date.now() - t0) / 1000).toFixed(1)} s` : 'no child'}, ` +
      `${villagers().length} villagers in the pen`);
    assert(child, 'no child');
    await command(`/tp Runner ${X}.5 ${Y} ${Z - 2}.5`, /Teleported/);   // (out of their way)

    // 2) night: to bed
    await command('/time set 13000', /time/i);
    await waitFor(() => villagers().filter((v) => poseName(m(v)[POSE]) === 'sleeping').length >= 2, 60000, 'asleep')
      .catch(() => {});
    const asleep = villagers().filter((v) => poseName(m(v)[POSE]) === 'sleeping');
    console.log(`  at night: ${asleep.length} of ${villagers().length} asleep, beds ` +
      `${asleep.map((v) => JSON.stringify(m(v)[SLEEPING_POS])).join(' ')}`);
    assert(asleep.length >= 2, 'not asleep');

    // 3) morning: the librarian to its lectern
    await command('/time set 3000', /time/i);
    const librarian = () => villagers().find((v) => {
      const d = m(v)[VILLAGER_DATA];
      return d && (d.villagerProfession !== undefined ? d.villagerProfession : d.profession) === LIBRARIAN;
    });
    await waitFor(() => librarian(), 30000, 'a librarian').catch(() => {});
    const lib = librarian();
    assert(lib, 'no librarian');
    const atWork = () => Math.hypot(lib.position.x - (X + 3.5), lib.position.z - (Z + 1.5)) < 3;   // (beside it)
    await waitFor(() => poseName(m(lib)[POSE]) !== 'sleeping' && atWork(), 30000, 'at work').catch(() => {});
    console.log(`  morning: the librarian ${poseName(m(lib)[POSE])}, ` +
      `${Math.hypot(lib.position.x - (X + 3.5), lib.position.z - (Z + 1.5)).toFixed(1)} blocks from its lectern`);
    assert(atWork(), 'the librarian did not go to work');
    console.log(`VILLAGER DAY OK (${host})`);
  } finally {
    for (const b of [runner, op]) if (b) b.quit();
    if (server) await server.stop();
    if (worldFile) fs.rmSync(worldFile, { force: true });
  }
})().catch((err) => { console.error(err.stack || err); process.exitCode = 1; });
