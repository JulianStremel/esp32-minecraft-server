'use strict';
// Status effects with a real client:
//  - /effect give: the client gets the effect and, for speed, the movement speed modifier
//  - a golden apple: regeneration and absorption (extra hearts)
//  - a milk bucket takes every effect away
//  - effects are kept when the player leaves and comes back
//  - /effect clear
//  - another player sees the effects' particles (the entity's effect_particles metadata)
//   node effects.js --host 192.168.1.160
//   SERVER_BIN=~/mc-host-build/mcserver node effects.js --local
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
if (!host && !local) { console.error('usage: node effects.js --host <ip> | --local'); process.exit(2); }
const PORT = local ? 25600 + Math.floor(Math.random() * 300) : 25565;
const SPEED = 0, REGENERATION = 9, NIGHT_VISION = 15, ABSORPTION = 21, MOVEMENT_SPEED = 22;
let server, op, runner, worldFile;
let seen = null;   // the runner's effect particles as the other player was last told them

async function command(text, pattern, ms = 15000) {
  const r = nextChat(op, pattern, ms);
  op.chat(text);
  const line = await r;
  await sleep(500);
  return line;
}

// what the client was told: active effects by id, the last movement speed attribute
function watch(bot) {
  const st = { effects: new Map(), speedMods: null, absorption: 0 };
  bot._client.on('entity_effect', (p) => { if (p.entityId === bot.entity.id) st.effects.set(p.effectId, p); });
  bot._client.on('remove_entity_effect', (p) => { if (p.entityId === bot.entity.id) st.effects.delete(p.effectId); });
  bot._client.on('entity_update_attributes', (p) => {
    if (p.entityId !== bot.entity.id) return;
    // minecraft-data names the attributes after an older registry: its 'generic.scale' is
    // 1.21.8's id 22, movement_speed (checked against the server jar)
    for (const a of p.properties) if (a.key === MOVEMENT_SPEED || a.key === 'generic.scale') st.speedMods = a.modifiers;
  });
  bot._client.on('entity_metadata', (p) => {
    if (p.entityId !== bot.entity.id) return;
    for (const m of p.metadata) if (m.key === 15 && typeof m.value === 'number') st.absorption = m.value;
  });
  return st;
}

async function join() {
  runner = await connectBot(PORT, 'Runner', { host, checkTimeoutInterval: 600000 });
  runner.on('error', () => {});
  const st = watch(runner);
  await waitFor(() => runner.entity, 20000, 'spawned');
  return st;
}

(async () => {
  try {
    if (local) {
      worldFile = path.join(os.tmpdir(), `mc-effects-${process.pid}.img`);
      server = startServer(['--port', String(PORT), '--seed', '42', '--ops', 'Tester', '--file', worldFile, '--size', '256',
        '--format'], { log: args.includes('--log') });
      await server.ready;
      host = 'localhost';
    }
    op = await connectBot(PORT, 'Tester', { host, checkTimeoutInterval: 600000 });
    op.on('error', () => {});
    op.physicsEnabled = false;
    await command('/gamemode spectator', /game mode/i);
    let st = await join();
    op._client.on('entity_metadata', (p) => {
      if (!runner.entity || p.entityId !== runner.entity.id) return;
      for (const m of p.metadata) if (m.key === 10) seen = m.value;
    });
    await command('/gamemode survival Runner', /game mode/i);
    await command('/effect clear Runner', /Removed/);

    // 1) /effect give: the effect, and speed's attribute modifier
    await command('/effect give Runner speed 60 1', /Applied/);
    await waitFor(() => st.effects.has(SPEED) && st.speedMods, 5000, 'speed').catch(() => {});
    const sp = st.effects.get(SPEED);
    const mod = (st.speedMods || []).find((m) => /effect\.speed/.test(m.uuid || m.id || ''));
    console.log(`  /effect give speed II: amplifier ${sp && sp.amplifier}, ${sp && sp.duration} ticks, ` +
      `modifier ${mod ? mod.amount.toFixed(2) : 'none'}`);
    assert(sp && sp.amplifier === 1, 'no speed II');
    assert(mod && Math.abs(mod.amount - 0.4) < 1e-6, 'no movement speed modifier');
    await waitFor(() => seen && seen.length === 1, 5000, 'particles').catch(() => {});
    console.log(`  the other player sees: ${JSON.stringify(seen)}`);
    assert(seen && seen.length === 1, 'no effect particles for the other player');

    // 2) a golden apple: regeneration II and absorption (4 extra health)
    await command('/give Runner golden_apple', /Gave|give/i);
    await sleep(500);
    await runner.equip(runner.inventory.items().find((i) => i.name === 'golden_apple'), 'hand');
    runner.activateItem();
    await waitFor(() => st.effects.has(ABSORPTION), 5000, 'eaten').catch(() => {});
    await sleep(500);
    runner.deactivateItem();
    console.log(`  golden apple: regeneration ${st.effects.has(REGENERATION) ? st.effects.get(REGENERATION).amplifier + 1 : 'no'}, ` +
      `absorption ${st.absorption}`);
    assert(st.effects.has(REGENERATION) && st.effects.get(REGENERATION).amplifier === 1, 'no regeneration II');
    assert.strictEqual(st.absorption, 4, 'no absorption hearts');

    // 3) kept across a reconnect
    await command('/effect give Runner night_vision 600', /Applied/);
    runner.quit();
    await sleep(2000);
    st = await join();
    // (sent with the login, before this test's listeners: mineflayer's own record of them)
    for (const [id, e] of Object.entries(runner.entity.effects)) st.effects.set(Number(id), { effectId: e.id, amplifier: e.amplifier });
    console.log(`  after a reconnect: effects ${[...st.effects.keys()].sort((a, b) => a - b).join(', ')}`);
    assert(st.effects.has(NIGHT_VISION) && st.effects.has(SPEED) && st.effects.has(ABSORPTION), 'effects lost');

    // 4) milk: every effect goes, the bucket stays
    await command('/give Runner milk_bucket', /Gave|give/i);
    await sleep(500);
    await runner.equip(runner.inventory.items().find((i) => i.name === 'milk_bucket'), 'hand');
    runner.activateItem();
    await waitFor(() => st.effects.size === 0, 5000, 'milk').catch(() => {});
    await sleep(500);
    runner.deactivateItem();
    const bucket = runner.inventory.items().find((i) => i.name === 'bucket');
    console.log(`  milk: ${st.effects.size} effects left, absorption ${st.absorption}, ${bucket ? 'an empty bucket' : 'no bucket'}`);
    assert.strictEqual(st.effects.size, 0, 'the milk left effects');
    assert(bucket, 'no empty bucket');
    await waitFor(() => seen && seen.length === 0, 5000, 'no particles').catch(() => {});
    assert(seen && seen.length === 0, 'the particles stayed');
    const mods = (st.speedMods || []).filter((m) => /effect/.test(m.uuid || m.id || ''));
    assert.strictEqual(mods.length, 0, 'the speed modifier stayed');

    // 5) /effect clear
    await command('/effect give Runner regeneration 60', /Applied/);
    await command('/effect clear Runner', /Removed/);
    await sleep(500);
    assert.strictEqual(st.effects.size, 0, '/effect clear left effects');
    console.log(`EFFECTS OK (${host})`);
  } finally {
    for (const b of [runner, op]) if (b) b.quit();
    if (server) await server.stop();
    if (worldFile) fs.rmSync(worldFile, { force: true });
  }
})().catch((err) => { console.error(err.message || err); process.exitCode = 1; });
