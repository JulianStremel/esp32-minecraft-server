'use strict';
// A raid with a real client (difficulty normal):
//  - an ominous bottle drunk: Bad Omen; in a village (a bell, villagers): Raid Omen
//  - 30 s later the raid: its boss bar, a horn, the first wave of pillagers 15 s later
//  - every wave beaten (killed with /kill): victory, Hero of the Village for the player
//   node raid.js --host 192.168.1.160
//   SERVER_BIN=~/mc-host-build/mcserver node raid.js --local
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
if (!host && !local) { console.error('usage: node raid.js --host <ip> | --local'); process.exit(2); }
const PORT = local ? 25600 + Math.floor(Math.random() * 300) : 25565;
const X = Number(opt('x', '280')), Z = Number(opt('z', '280')), Y = 120;
const BAD_OMEN = 30, HERO = 31, RAID_OMEN = 34;
const RAIDERS = ['pillager', 'vindicator', 'witch', 'evoker', 'ravager'];
let server, op, runner, worldFile;

async function command(text, pattern, ms = 15000) {
  const r = nextChat(op, pattern, ms);
  op.chat(text);
  const line = await r;
  await sleep(300);
  return line;
}

(async () => {
  try {
    if (local) {
      worldFile = path.join(os.tmpdir(), `mc-raid-${process.pid}.img`);
      server = startServer(['--port', String(PORT), '--seed', '42', '--ops', 'Tester', '--file', worldFile, '--size', '256',
        '--no-mobs', '--flat', '--format'], { log: args.includes('--log') });
      await server.ready;
      host = 'localhost';
    }
    op = await connectBot(PORT, 'Tester', { host, checkTimeoutInterval: 600000 });
    op.on('error', () => {});
    op.physicsEnabled = false;
    await command('/gamemode spectator', /game mode/i);
    await command('/difficulty normal', /difficulty/i);
    await command('/time set 1000', /time/i);
    runner = await connectBot(PORT, 'Runner', { host, checkTimeoutInterval: 600000 });
    runner.on('error', () => {});
    const effects = new Map();
    runner._client.on('entity_effect', (p) => { if (runner.entity && p.entityId === runner.entity.id) effects.set(p.effectId, p); });
    runner._client.on('remove_entity_effect', (p) => { if (runner.entity && p.entityId === runner.entity.id) effects.delete(p.effectId); });
    const bars = [];
    runner._client.on('boss_bar', (p) => bars.push(p));
    await command('/gamemode survival Runner', /game mode/i);
    await command(`/tp Tester ${X} ${Y + 10} ${Z - 8}`, /Teleported/);
    await sleep(2000);
    await command(`/fill ${X - 36} ${Y - 1} ${Z - 36} ${X + 36} ${Y - 1} ${Z + 36} stone`, /filled/i);
    await command(`/fill ${X - 36} ${Y} ${Z - 36} ${X + 36} ${Y + 4} ${Z + 36} air`, /filled/i);
    // the village: a bell and three villagers in a glass hut
    await command(`/fill ${X + 3} ${Y} ${Z + 3} ${X + 7} ${Y + 2} ${Z + 7} glass`, /filled/i);
    await command(`/fill ${X + 4} ${Y} ${Z + 4} ${X + 6} ${Y + 1} ${Z + 6} air`, /filled/i);
    await command(`/setblock ${X} ${Y} ${Z} bell`, /Changed|set/i);
    for (let i = 0; i < 3; i++) await command(`/summon villager ${X + 4 + i}.5 ${Y} ${Z + 5}.5`, /Summoned/);
    await command(`/tp Runner ${X - 2}.5 ${Y} ${Z}.5`, /Teleported/);
    await command('/effect give Runner resistance 600 4 true', /Applied/);   // (the raiders' arrows do nothing)
    await command('/give Runner ominous_bottle', /Gave/);
    await sleep(800);

    // 1) Bad Omen, then Raid Omen in the village
    await runner.equip(runner.inventory.items().find((i) => i.name === 'ominous_bottle'), 'hand');
    runner.activateItem();
    await waitFor(() => effects.has(BAD_OMEN) || effects.has(RAID_OMEN), 5000, 'an omen').catch(() => {});
    runner.deactivateItem();
    await waitFor(() => effects.has(RAID_OMEN), 5000, 'raid omen').catch(() => {});
    console.log(`  the bottle drunk in the village: ${effects.has(RAID_OMEN) ? 'Raid Omen' : effects.has(BAD_OMEN) ? 'Bad Omen only' : 'nothing'}`);
    assert(effects.has(RAID_OMEN), 'no Raid Omen');

    // 2) the raid: the bar, then the waves
    await waitFor(() => bars.some((b) => b.action === 0), 40000, 'the raid bar').catch(() => {});
    const add = bars.find((b) => b.action === 0);
    console.log(`  after the omen: ${add ? 'the raid bar ' + JSON.stringify(add.title) : 'no bar'}`);
    assert(add, 'no raid bar');
    let waves = 0;
    const raiders = () => Object.values(runner.entities).filter((e) => RAIDERS.includes(e.name));
    for (let w = 1; w <= 5; w++) {
      await waitFor(() => raiders().length > 0, 30000, `wave ${w}`).catch(() => {});
      const here = raiders();
      if (!here.length) break;
      waves++;
      const kinds = {};
      for (const e of here) kinds[e.name] = (kinds[e.name] || 0) + 1;
      console.log(`  wave ${w}: ${JSON.stringify(kinds)}`);
      await sleep(1500);
      for (const name of RAIDERS) await command(`/kill @e[type=${name}]`, /Killed|No entity/i);
      await waitFor(() => raiders().length === 0, 5000, 'cleared').catch(() => {});
    }
    assert.strictEqual(waves, 5, 'not five waves');
    await waitFor(() => effects.has(HERO), 20000, 'the hero').catch(() => {});
    const hero = effects.get(HERO);
    console.log(`  victory: Hero of the Village ${hero ? 'level ' + (hero.amplifier + 1) : 'missing'}`);
    assert(hero, 'no Hero of the Village');
    console.log(`RAID OK (${host})`);
  } finally {
    for (const b of [runner, op]) if (b) b.quit();
    if (server) await server.stop();
    if (worldFile) fs.rmSync(worldFile, { force: true });
  }
})().catch((err) => { console.error(err.stack || err); process.exitCode = 1; });
