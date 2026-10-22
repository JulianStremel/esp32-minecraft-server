'use strict';
// A village's friends and foes with a real client (difficulty hard):
//  - a zombie in a pen with a librarian: the librarian becomes a zombie villager that keeps
//    its profession (the VillagerData metadata)
//  - a splash potion of weakness and a golden apple: its cure starts (the Converting
//    metadata, the cure's entity event); the cure's end is in the unit tests (3 to 5 minutes)
//  - four iron blocks and a carved pumpkin placed by the player make an iron golem, which
//    kills a zombie near it
//   node village_defense.js --host 192.168.1.160
//   SERVER_BIN=~/mc-host-build/mcserver node village_defense.js --local
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
if (!host && !local) { console.error('usage: node village_defense.js --host <ip> | --local'); process.exit(2); }
const PORT = local ? 25600 + Math.floor(Math.random() * 300) : 25565;
const X = Number(opt('x', '160')), Z = Number(opt('z', '160')), Y = 120;
const VILLAGER_DATA = 18, ZV_CONVERTING = 19, ZV_DATA = 20, LIBRARIAN = 9;
let server, op, runner, worldFile;

async function command(text, pattern, ms = 15000) {
  const r = nextChat(op, pattern, ms);
  op.chat(text);
  const line = await r;
  await sleep(400);
  return line;
}
const meta = new Map();   // entity id -> {key: value}
const statuses = [];
function watch(bot) {
  bot._client.on('entity_metadata', (p) => {
    const m = meta.get(p.entityId) || {};
    for (const e of p.metadata) m[e.key] = e.value;
    meta.set(p.entityId, m);
  });
  bot._client.on('entity_status', (p) => statuses.push(p));
}
const professionOf = (v) => v && (v.villagerProfession !== undefined ? v.villagerProfession : v.profession);
const mobs = (name) => Object.values(runner.entities).filter((e) => e.name === name);
const near = (e, x, z, r) => e && Math.abs(e.position.x - x) < r && Math.abs(e.position.z - z) < r;

(async () => {
  try {
    if (local) {
      worldFile = path.join(os.tmpdir(), `mc-village-${process.pid}.img`);
      server = startServer(['--port', String(PORT), '--seed', '42', '--ops', 'Tester', '--file', worldFile, '--size', '256',
        '--no-mobs', '--flat', '--format'], { log: args.includes('--log') });
      await server.ready;
      host = 'localhost';
    }
    op = await connectBot(PORT, 'Tester', { host, checkTimeoutInterval: 600000 });
    op.on('error', () => {});
    op.physicsEnabled = false;
    await command('/gamemode spectator', /game mode/i);
    await command('/difficulty hard', /difficulty/i);
    runner = await connectBot(PORT, 'Runner', { host, checkTimeoutInterval: 600000 });
    runner.on('error', () => {});
    watch(runner);
    await command('/gamemode creative Runner', /game mode/i);
    await command(`/tp Tester ${X} ${Y + 6} ${Z - 6}`, /Teleported/);
    await sleep(2000);
    await command(`/fill ${X - 6} ${Y - 1} ${Z - 6} ${X + 26} ${Y - 1} ${Z + 8} stone`, /filled/i);
    await command(`/fill ${X - 6} ${Y} ${Z - 6} ${X + 26} ${Y + 4} ${Z + 8} air`, /filled/i);
    // a glass pen (3 x 3 inside) with a lectern, open to the runner on one side by a glass pane
    await command(`/fill ${X - 2} ${Y} ${Z + 1} ${X + 2} ${Y + 2} ${Z + 5} glass`, /filled/i);
    await command(`/fill ${X - 1} ${Y} ${Z + 2} ${X + 1} ${Y + 1} ${Z + 4} air`, /filled/i);
    await command(`/setblock ${X + 1} ${Y} ${Z + 4} lectern`, /Changed|set/i);
    await command(`/summon villager ${X}.5 ${Y} ${Z + 3}.5`, /Summoned/);
    await command(`/tp Runner ${X}.5 ${Y} ${Z - 1}.5`, /Teleported/);
    const villager = () => mobs('villager').find((e) => near(e, X + 0.5, Z + 3.5, 2));
    await waitFor(() => villager() && professionOf((meta.get(villager().id) || {})[VILLAGER_DATA]) === LIBRARIAN, 30000,
      'a librarian');

    // 1) a zombie in the pen: the librarian turns
    await command(`/summon zombie ${X - 1}.5 ${Y} ${Z + 2}.5`, /Summoned/);
    await waitFor(() => mobs('zombie_villager').some((e) => near(e, X + 0.5, Z + 3.5, 3)), 30000, 'a zombie villager')
      .catch(() => {});
    const zv = mobs('zombie_villager').find((e) => near(e, X + 0.5, Z + 3.5, 3));
    await sleep(500);
    const zdata = zv && (meta.get(zv.id) || {})[ZV_DATA];
    console.log(`  the zombie in the pen: ${zv ? 'a zombie villager, ' + JSON.stringify(zdata) : 'no zombie villager'}`);
    assert(zv, 'the villager did not turn');
    assert.strictEqual(professionOf(zdata), LIBRARIAN, 'the zombie villager forgot its profession');
    await command('/kill @e[type=zombie]', /Killed/);

    // 2) the cure: weakness from a splash potion at its feet, then a golden apple
    await command(`/tp Runner ${X}.5 ${Y} ${Z + 1}.5`, /Teleported/);   // (in the pen's doorway)
    await command(`/setblock ${X} ${Y} ${Z + 1} air`, /Changed|set/i);
    await command(`/setblock ${X} ${Y + 1} ${Z + 1} air`, /Changed|set/i);
    await command('/give Runner splash_potion[potion_contents={potion:"minecraft:weakness"}]', /Gave/);
    await command('/give Runner golden_apple', /Gave/);
    await sleep(800);
    await runner.equip(runner.inventory.items().find((i) => i.name === 'splash_potion'), 'hand');
    await runner.lookAt(zv.position.offset(0, 0.2, 0), true);
    runner.activateItem();
    await sleep(2500);
    await runner.equip(runner.inventory.items().find((i) => i.name === 'golden_apple'), 'hand');
    await runner.lookAt(zv.position.offset(0, 1.2, 0), true);
    runner.activateEntity(zv);
    await waitFor(() => (meta.get(zv.id) || {})[ZV_CONVERTING] === true, 5000, 'converting').catch(() => {});
    const cureEvent = statuses.some((s) => s.entityId === zv.id && s.entityStatus === 16);
    console.log(`  weakness and a golden apple: converting ${(meta.get(zv.id) || {})[ZV_CONVERTING]}, the cure's event ${cureEvent}`);
    assert.strictEqual((meta.get(zv.id) || {})[ZV_CONVERTING], true, 'the cure did not start');

    // 3) an iron golem built by the player, which kills a zombie
    const gx = X + 20, gz = Z;   // (beyond the zombie villager: golems go for those too)
    await command(`/tp Runner ${gx}.5 ${Y} ${gz - 3}.5`, /Teleported/);
    await command(`/setblock ${gx} ${Y} ${gz} iron_block`, /Changed|set/i);
    await command(`/fill ${gx - 1} ${Y + 1} ${gz} ${gx + 1} ${Y + 1} ${gz} iron_block`, /filled/i);
    await command('/give Runner carved_pumpkin', /Gave/);
    await sleep(800);
    await runner.equip(runner.inventory.items().find((i) => i.name === 'carved_pumpkin'), 'hand');
    await runner.placeBlock(runner.blockAt(new Vec3(gx, Y + 1, gz)), new Vec3(0, 1, 0)).catch(() => {});
    await waitFor(() => mobs('iron_golem').some((e) => near(e, gx + 0.5, gz + 0.5, 3)), 5000, 'a golem').catch(() => {});
    const golem = mobs('iron_golem').find((e) => near(e, gx + 0.5, gz + 0.5, 3));
    const top = runner.blockAt(new Vec3(gx, Y + 1, gz));
    console.log(`  iron blocks and a pumpkin: ${golem ? 'an iron golem' : 'no golem'}, the blocks ${top ? top.name : '?'}`);
    assert(golem, 'no golem');
    await command(`/summon zombie ${gx + 3}.5 ${Y} ${gz + 2}.5`, /Summoned/);
    const zombieThere = () => mobs('zombie').some((e) => near(e, gx + 3.5, gz + 2.5, 8));
    await waitFor(() => zombieThere(), 5000, 'the zombie');
    const t0 = Date.now();
    const trace = setInterval(() => {
      if (!args.includes('--trace')) return;
      const z = mobs('zombie')[0], g = mobs('iron_golem').find((e) => near(e, gx + 0.5, gz + 0.5, 12));
      console.log(`    ${((Date.now() - t0) / 1000).toFixed(1)} s: golem ${g && g.position.toString()} zombie ${z && z.position.toString()} ` +
        `${z ? JSON.stringify((meta.get(z.id) || {})[9]) : ''}`);
    }, 1000);
    await waitFor(() => !zombieThere(), 30000, 'the zombie killed').catch(() => {});
    clearInterval(trace);
    console.log(`  a zombie near the golem: ${zombieThere() ? 'still there' : `killed after ${((Date.now() - t0) / 1000).toFixed(1)} s`}`);
    assert(!zombieThere(), 'the golem did not kill the zombie');
    console.log(`VILLAGE DEFENSE OK (${host})`);
  } finally {
    for (const b of [runner, op]) if (b) b.quit();
    if (server) await server.stop();
    if (worldFile) fs.rmSync(worldFile, { force: true });
  }
})().catch((err) => { console.error(err.stack || err); process.exitCode = 1; });
