'use strict';
// The dragon fight with a real client: the fight starts when a player comes to the End
// (boss bar, dragon, 10 crystals on the spikes); hitting the crystals destroys them;
// a hit on the dragon's own id does nothing (vanilla), hits on its body parts hurt it;
// it dies, the exit portal opens with the egg on it, the player gets the XP; after a
// restart (PC) it stays dead; the exit portal leads home.
//   node dragon_fight.js --host 192.168.1.160
//   SERVER_BIN=~/mc-host-build/mcserver node dragon_fight.js --local
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { Vec3 } = require('vec3');
const { startServer, connectBot, nextChat, sleep, waitFor } = require('./lib');

const args = process.argv.slice(2);
const local = args.includes('--local');
const opt = (name, fallback) => {
  const i = args.indexOf('--' + name);
  return i < 0 ? fallback : args[i + 1];
};
let host = opt('host');
if (!host && !local) { console.error('usage: node dragon_fight.js --host <ip> | --local'); process.exit(2); }
const PORT = local ? 25600 + Math.floor(Math.random() * 300) : 25565;
let server, op, worldFile;

async function command(text, pattern, ms = 15000) {
  const r = nextChat(op, pattern, ms);
  op.chat(text);
  const line = await r;
  await sleep(650);
  return line;
}

const entities = (name) => Object.values(op.entities).filter((e) => e.name === name);
const dragon = () => entities('ender_dragon')[0];

function hitId(id) {   // a raw attack: mineflayer would only ever send the dragon's own id
  op._client.write('use_entity', { target: id, mouse: 1, sneaking: false });
  op.swingArm();
}

async function join() {
  op = await connectBot(PORT, 'Tester', { host, checkTimeoutInterval: 600000 });
  op.on('error', () => {});
  op.myBars = {};
  op.on('bossBarCreated', (b) => { op.myBars[b.entityUUID] = b; });
  op.on('bossBarDeleted', (b) => { delete op.myBars[b.entityUUID]; });
}

async function startLocal(format) {
  server = startServer(['--port', String(PORT), '--seed', '42', '--ops', 'Tester', '--file', worldFile, '--size', '256',
    '--no-mobs', ...(format ? ['--format'] : [])], { log: args.includes('--log') });
  await server.ready;
}

(async () => {
  try {
    if (local) {
      worldFile = path.join(os.tmpdir(), `mc-dragon-${process.pid}.img`);
      await startLocal(true);
      host = 'localhost';
    }
    await join();
    await command('/gamemode creative', /game mode/i);
    await command('/give Tester netherite_sword 1', /Gave|Given/i);
    await waitFor(() => op.inventory.items().some((i) => i.name === 'netherite_sword'), 5000, 'the sword');
    await op.equip(op.inventory.items().find((i) => i.name === 'netherite_sword'), 'hand');
    const xp0 = op.experience.points;
    await command('/dimension the_end', /Mov(ed|ing) Tester/);
    await waitFor(() => op.game.dimension === 'the_end', 30000, 'the End');
    await sleep(2000);
    await command('/tp Tester 0.5 100 20.5', /Teleported/);   // over the island: every spike in view
    // 1) the fight starts
    await waitFor(() => dragon() && Object.keys(op.myBars).length > 0 && entities('end_crystal').length >= 10, 30000,
      'the dragon, its boss bar and 10 crystals').catch((e) => {
      console.log(`  dragon ${!!dragon()}, boss bars ${Object.keys(op.myBars).length}, crystals ${entities('end_crystal').length}, ` +
        `at ${op.entity.position.floored()}; entities: ${[...new Set(Object.values(op.entities).map((x) => x.name))].join(' ')}`);
      throw e;
    });
    const bar = Object.values(op.myBars)[0];
    console.log(`  the fight started: dragon at ${dragon().position.floored()}, ${entities('end_crystal').length} crystals, ` +
      `boss bar ${JSON.stringify(bar.title && bar.title.toString ? bar.title.toString() : bar.title)} ${bar.health}`);

    // 2) the crystals
    for (const c of entities('end_crystal')) {
      await command(`/tp Tester ${c.position.x.toFixed(1)} ${(c.position.y + 1).toFixed(1)} ${(c.position.z + 3).toFixed(1)}`, /Teleported/);
      await sleep(400);
      hitId(c.id);
      await sleep(600);
    }
    await waitFor(() => entities('end_crystal').length === 0, 10000, 'the crystals gone').catch(() => {});
    console.log(`  crystals left after hitting each: ${entities('end_crystal').length}`);
    assert.strictEqual(entities('end_crystal').length, 0, 'crystals survived being hit');

    // 3) the dragon: its own id does nothing, a body part does
    const healthOf = () => { const b = Object.values(op.myBars)[0]; return b ? b.health * 200 : 0; };
    const near = async () => {
      const d = dragon();
      if (!d) return false;
      await command(`/tp Tester ${d.position.x.toFixed(1)} ${(d.position.y + 6).toFixed(1)} ${d.position.z.toFixed(1)}`, /Teleported/);
      return true;
    };
    await near();
    let h = healthOf();
    hitId(dragon().id);
    await sleep(1500);
    const afterOwnId = healthOf();
    console.log(`  a hit on the dragon's own id: health ${h.toFixed(1)} -> ${afterOwnId.toFixed(1)}`);
    assert(afterOwnId >= h - 0.01, 'the dragon\'s own id took damage');
    let hits = 0;
    const t0 = Date.now();
    while (dragon() && healthOf() > 0.5 && Date.now() - t0 < 120000) {
      if (hits % 3 === 0 && !(await near())) break;
      const d = dragon();
      if (!d) break;
      hitId(d.id + 1 + (hits % 3 === 2 ? 2 : 0));   // the neck (= the head on the server), now and then the body
      hits++;
      await sleep(700);
    }
    console.log(`  ${hits} hits; boss bar at ${healthOf().toFixed(1)} after ${((Date.now() - t0) / 1000).toFixed(0)} s`);
    // 4) its death: the portal, the egg, the XP; the boss bar goes
    await waitFor(() => !dragon(), 60000, 'the dragon gone');
    await waitFor(() => Object.keys(op.myBars).length === 0, 10000, 'the boss bar gone').catch(() => {});
    await command(`/tp Tester 5.5 75 5.5`, /Teleported/);
    await sleep(3000);
    let portalY = -1, egg = -1;
    for (let y = 30; y < 100; y++) {
      const b = op.blockAt(new Vec3(1, y, 1));
      if (b && b.name === 'end_portal') portalY = y;
      const e = op.blockAt(new Vec3(0, y, 0));
      if (e && e.name === 'dragon_egg') egg = y;
    }
    console.log(`  dead: exit portal at y ${portalY}, egg at y ${egg}, XP ${xp0} -> ${op.experience.points}, ` +
      `boss bars ${Object.keys(op.myBars).length}`);
    assert(portalY > 0, 'the exit portal did not open');
    assert.strictEqual(egg, portalY + 4, 'no dragon egg on the portal');
    assert(op.experience.points - xp0 >= 12000, 'no 12000 XP for the first kill');
    assert.strictEqual(Object.keys(op.myBars).length, 0, 'the boss bar stayed');

    // 5) after a restart it stays dead (PC only: the board keeps running)
    if (local) {
      op.quit();
      await server.stop();
      await startLocal(false);
      await join();
      await waitFor(() => op.game.dimension === 'the_end', 30000, 'back in the End');
      await sleep(6000);
      assert(!dragon(), 'the dragon came back after a restart');
      const e = op.blockAt(new Vec3(0, egg, 0));
      assert(e && e.name === 'dragon_egg', 'the egg is gone after a restart');
      console.log('  after a restart: no dragon, the egg is still there');
    }
    // 6) home through the exit portal
    await command(`/tp Tester 1.5 ${portalY} 1.5`, /Teleported/);
    await waitFor(() => op.game.dimension === 'overworld', 30000, 'home through the exit portal');
    console.log(`  through the exit portal: ${op.game.dimension}`);
    // 7) reset, as if never fought (leaves the board's world as it was)
    const st = await command('/dragon reset', /reset/);
    const status = await command('/dragon status', /Dragon fight/);
    console.log(`  ${status}`);
    assert(/not started/.test(status) && /killed before: no/.test(status), 'the fight was not reset');
    console.log(`DRAGON FIGHT OK (${host})`);
  } finally {
    if (op) op.quit();
    if (server) await server.stop();
    if (worldFile) fs.rmSync(worldFile, { force: true });
  }
})().catch((err) => { console.error(err.message || err); process.exitCode = 1; });
