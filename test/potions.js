'use strict';
// Potions with a real client:
//  - a glass bottle used on water becomes a water bottle (the potion_contents component)
//  - a potion of healing drunk heals, the empty bottle stays
//  - a splash potion of harming thrown at the feet hurts
//  - a lingering potion of poison leaves a cloud that poisons
//   node potions.js --host 192.168.1.160
//   SERVER_BIN=~/mc-host-build/mcserver node potions.js --local
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
if (!host && !local) { console.error('usage: node potions.js --host <ip> | --local'); process.exit(2); }
const PORT = local ? 25600 + Math.floor(Math.random() * 300) : 25565;
const X = Number(opt('x', '40')), Z = Number(opt('z', '40'));
const POISON = 18;
let server, op, runner, worldFile;

async function command(text, pattern, ms = 15000) {
  const r = nextChat(op, pattern, ms);
  op.chat(text);
  const line = await r;
  await sleep(500);
  return line;
}

const give = (item, potion) => command(`/give Runner ${item}[potion_contents={potion:"minecraft:${potion}"}]`, /Gave/);
async function hold(name) {
  await sleep(300);
  const it = runner.inventory.items().find((i) => i.name === name);
  assert(it, 'no ' + name);
  await runner.equip(it, 'hand');
  return it;
}
// the potion an item holds, from its potion_contents component (as the client sees it)
function potionOf(item) {
  const c = item && (item.components || []).find((k) => k.type === 'potion_contents');
  return c && c.data && c.data.potionId !== undefined ? c.data.potionId : (c && c.data && c.data.potion);
}

(async () => {
  try {
    if (local) {
      worldFile = path.join(os.tmpdir(), `mc-potions-${process.pid}.img`);
      server = startServer(['--port', String(PORT), '--seed', '42', '--ops', 'Tester', '--file', worldFile, '--size', '256',
        '--format', '--flat'], { log: args.includes('--log') });
      await server.ready;
      host = 'localhost';
    }
    op = await connectBot(PORT, 'Tester', { host, checkTimeoutInterval: 600000 });
    op.on('error', () => {});
    op.physicsEnabled = false;
    await command('/gamemode spectator', /game mode/i);
    runner = await connectBot(PORT, 'Runner', { host, checkTimeoutInterval: 600000 });
    runner.on('error', () => {});
    await command('/gamemode survival Runner', /game mode/i);
    await command('/difficulty normal', /difficulty/i);
    // a stone platform high up with a pool in front
    const Y = 120;
    await command(`/tp Tester ${X} ${Y + 4} ${Z - 6}`, /Teleported/);
    await sleep(2000);
    await command(`/fill ${X - 4} ${Y - 1} ${Z - 4} ${X + 4} ${Y - 1} ${Z + 4} stone`, /filled/i);
    await command(`/fill ${X - 4} ${Y} ${Z - 4} ${X + 4} ${Y + 4} ${Z + 4} air`, /filled/i);
    await command(`/setblock ${X} ${Y - 1} ${Z + 2} water`, /Changed|set/i);
    await command(`/tp Runner ${X}.5 ${Y} ${Z}.5`, /Teleported/);
    await waitFor(() => runner.entity && Math.abs(runner.entity.position.y - Y) < 0.1, 10000, 'on the platform');
    await sleep(1500);

    // 1) a glass bottle on water
    await command('/give Runner glass_bottle 2', /Gave/);
    await hold('glass_bottle');
    await runner.lookAt(new Vec3(X + 0.5, Y - 0.1, Z + 2.5), true);
    await sleep(300);
    runner.activateItem();
    await waitFor(() => runner.inventory.items().some((i) => i.name === 'potion'), 5000, 'a water bottle').catch(() => {});
    runner.deactivateItem();
    const water = runner.inventory.items().find((i) => i.name === 'potion');
    const bottles = runner.inventory.items().filter((i) => i.name === 'glass_bottle').reduce((a, i) => a + i.count, 0);
    console.log(`  glass bottle on water: ${water ? 'a water bottle (potion ' + potionOf(water) + ')' : 'nothing'}, ${bottles} bottle left`);
    assert(water, 'no water bottle');
    assert.strictEqual(potionOf(water), 0, 'not water');
    assert.strictEqual(bottles, 1);
    await command('/clear Runner', /Cleared|Removed|clear/i);

    // 2) healing II drunk
    await command('/effect give Runner instant_damage 1 0', /Applied/);   // 6 damage
    await sleep(500);
    const h0 = runner.health;
    await give('potion', 'strong_healing');
    await hold('potion');
    runner.activateItem();
    await waitFor(() => runner.inventory.items().some((i) => i.name === 'glass_bottle'), 5000, 'drunk').catch(() => {});
    await sleep(500);
    runner.deactivateItem();
    const empty = runner.inventory.items().find((i) => i.name === 'glass_bottle');
    console.log(`  healing II drunk: health ${h0} -> ${runner.health}, ${empty ? 'the empty bottle' : 'no bottle'}`);
    assert(runner.health >= Math.min(20, h0 + 7), 'not healed');
    assert(empty, 'no empty bottle');

    // 3) a splash potion of harming at the feet
    await command('/heal Runner', /heal/i).catch(() => {});
    await sleep(500);
    await give('splash_potion', 'harming');
    await hold('splash_potion');
    await runner.lookAt(new Vec3(X + 0.5, Y - 1, Z + 1.0), true);
    const h1 = runner.health;
    runner.activateItem();
    runner.deactivateItem();
    await waitFor(() => runner.health < h1, 5000, 'splashed').catch(() => {});
    console.log(`  splash potion of harming at the feet: health ${h1} -> ${runner.health}`);
    assert(runner.health < h1, 'the splash potion did nothing');

    // 4) a lingering potion of poison: a cloud
    await command('/heal Runner', /heal/i).catch(() => {});
    await sleep(500);
    await give('lingering_potion', 'poison');
    await hold('lingering_potion');
    await runner.lookAt(new Vec3(X + 0.5, Y - 1, Z + 1.0), true);
    let poisoned = false;
    runner._client.on('entity_effect', (p) => { if (p.entityId === runner.entity.id && p.effectId === POISON) poisoned = true; });
    runner.activateItem();
    runner.deactivateItem();
    let cloud;
    await waitFor(() => (cloud = Object.values(runner.entities).find((e) => e.name === 'area_effect_cloud')), 5000, 'the cloud').catch(() => {});
    await waitFor(() => poisoned, 5000, 'poisoned').catch(() => {});
    console.log(`  lingering potion of poison: ${cloud ? 'a cloud' : 'no cloud'}, ${poisoned ? 'poisoned' : 'not poisoned'}`);
    assert(cloud, 'no cloud');
    assert(poisoned, 'the cloud did not poison');
    console.log(`POTIONS OK (${host})`);
  } finally {
    for (const b of [runner, op]) if (b) b.quit();
    if (server) await server.stop();
    if (worldFile) fs.rmSync(worldFile, { force: true });
  }
})().catch((err) => { console.error(err.message || err); process.exitCode = 1; });
