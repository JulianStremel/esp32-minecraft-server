'use strict';
// The operator menu as dialogs (1.21.6+): /menu shows the main dialog; its buttons come
// back as custom_click_action with the form's values, and the server answers with the
// next page or carries the action out. Only operators may use it. Ends with a world
// reset through the confirmation: the server restarts into a new world with that seed.
//   SERVER_BIN=.../mcserver node menu_dialogs.js
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { Vec3 } = require('vec3');
const { startServer, connectBot, sleep, waitFor, nextChat } = require('./lib');

// network NBT of a compound of strings and bytes (no root name)
function nbtCompound(values) {
  const parts = [Buffer.from([10])];
  for (const [k, v] of Object.entries(values)) {
    const key = Buffer.from(k, 'utf8');
    const head = (type) => { const b = Buffer.alloc(3); b[0] = type; b.writeUInt16BE(key.length, 1); return Buffer.concat([b, key]); };
    if (typeof v === 'boolean') parts.push(head(1), Buffer.from([v ? 1 : 0]));
    else {
      const s = Buffer.from(String(v), 'utf8');
      const l = Buffer.alloc(2);
      l.writeUInt16BE(s.length);
      parts.push(head(8), l, s);
    }
  }
  parts.push(Buffer.from([0]));
  return Buffer.concat(parts);
}
function varint(n) {
  const out = [];
  do { let b = n & 0x7f; n >>>= 7; if (n) b |= 0x80; out.push(b); } while (n);
  return Buffer.from(out);
}
// custom_click_action (play 0x41): the id, then the payload with or without its length prefix
function click(bot, id, values = null, prefixed = true) {
  const idb = Buffer.from(id, 'utf8');
  let body = values ? nbtCompound(values) : Buffer.from([0]);
  if (prefixed) body = Buffer.concat([varint(body.length), body]);
  bot._client.writeRaw(Buffer.concat([varint(0x41), varint(idb.length), idb, body]));
}
const text = (t) => (t == null ? '' : typeof t === 'string' ? t : t.value?.text?.value ?? t.text ?? JSON.stringify(t));

(async () => {
  const port = 25640 + Math.floor(Math.random() * 100);
  // a world file: the reset at the end restarts the server, which keeps the new seed
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'mcmenu-'));
  const srv = startServer(['--port', String(port), '--seed', '42', '--ops', 'Admin', '--no-mobs', '--file', path.join(tmp, 'world.img'), '--size', '256'],
                          { log: process.argv.includes('--log') });
  let bot, guest;
  try {
    await srv.ready;
    bot = await connectBot(port, 'Admin');
    await waitFor(() => bot.blockAt(bot.entity.position.offset(0, -1, 0)), 20000, 'terrain');
    // a dialog the client could not read completely would be dropped by the vanilla client
    let partial = 0;
    for (const b of [bot]) b._client.on('error', (e) => { if (/partial packet|Chunk size/.test(String(e))) partial++; });
    const origLog = console.log;
    console.log = (...a) => { if (/Chunk size is \d+ but only/.test(String(a[0]))) partial++; else origLog(...a); };
    let dialog = null;
    bot._client.on('show_dialog', (pk) => { dialog = require('prismarine-nbt').simplify(pk.dialog.data ?? pk.dialog); });
    const next = async (what) => { dialog = null; await waitFor(() => dialog, 5000, what); return dialog; };
    const labels = (d) => (d.actions || []).map((a) => text(a.label));
    // 1. the main page
    let wait = next('the main dialog');
    bot.chat('/menu');
    let d = await wait;
    console.log(`/menu: ${d.type} "${text(d.title)}" with ${labels(d).join(', ')}; ${d.body.length} lines of statistics`);
    assert.strictEqual(d.type, 'minecraft:multi_action');
    assert(labels(d).includes('Settings') && labels(d).includes('Players'));
    assert(d.actions.every((a) => a.action.type === 'minecraft:dynamic/custom' && a.action.id.startsWith('esp32mc:')));
    // 2. settings: the form, applied
    wait = next('the settings dialog');
    click(bot, 'esp32mc:settings');
    d = await wait;
    const keys = d.inputs.map((i) => i.key);
    console.log(`settings: inputs ${keys.join(', ')}`);
    assert.deepStrictEqual(keys, ['difficulty', 'mobs', 'pvp', 'perfbar', 'time', 'weather']);
    wait = next('the settings dialog again');
    click(bot, 'esp32mc:settings_apply', { difficulty: 'hard', mobs: false, pvp: true, perfbar: false, time: 'night', weather: 'rain' }, false);
    d = await wait;
    const diff = d.inputs.find((i) => i.key === 'difficulty').options.find((o) => o.initial);
    const mobs = d.inputs.find((i) => i.key === 'mobs').initial;
    await waitFor(() => bot.isRaining, 5000, 'rain');
    console.log(`applied (payload without length prefix): difficulty now ${diff && diff.id}, mob spawning ${mobs}, raining ${bot.isRaining}, time ${bot.time.timeOfDay}`);
    assert.strictEqual(diff.id, 'hard');
    assert(!mobs);
    assert(bot.time.timeOfDay >= 12000);
    // 3. a player: their game mode
    wait = next('the players dialog');
    click(bot, 'esp32mc:players');
    d = await wait;
    assert(labels(d).includes('Admin'));
    wait = next('the player dialog');
    click(bot, 'esp32mc:player', { name: 'Admin' });
    d = await wait;
    assert.strictEqual(text(d.title), 'Admin');
    wait = next('the player dialog after the change');
    click(bot, 'esp32mc:player_mode', { name: 'Admin', mode: 'creative' });
    await wait;
    await waitFor(() => bot.game.gameMode === 'creative', 5000, 'creative');
    console.log('player page: game mode set to creative');
    // 4. the world reset asks first; "No" goes back
    wait = next('the confirmation');
    click(bot, 'esp32mc:world_reset_ask', { seed: 'hello', type: 'flat' });
    d = await wait;
    console.log(`reset: ${d.type}, "${text(d.body[0].contents).slice(0, 60)}..."`);
    assert.strictEqual(d.type, 'minecraft:confirmation');
    assert(text(d.body[0].contents).includes(String(99162322)), 'the seed of "hello" (Java hashCode)');
    assert.strictEqual(d.yes.action.additions.type, 'flat');
    wait = next('the world dialog');
    click(bot, 'esp32mc:world');
    d = await wait;
    assert.strictEqual(text(d.title), 'World');
    // 5. not for everybody
    guest = await connectBot(port, 'Guest');
    let refused = false;
    guest.on('messagestr', (m) => { if (/Only operators/.test(m)) refused = true; });
    let guestDialog = false;
    guest._client.on('show_dialog', () => { guestDialog = true; });
    click(guest, 'esp32mc:settings_apply', { difficulty: 'peaceful', mobs: true, pvp: false, perfbar: false, time: 'day', weather: 'clear' });
    await waitFor(() => refused, 5000, 'the refusal');
    await sleep(500);
    assert(!guestDialog);
    wait = next('settings, unchanged');
    click(bot, 'esp32mc:settings');
    d = await wait;
    assert.strictEqual(d.inputs.find((i) => i.key === 'difficulty').options.find((o) => o.initial).id, 'hard');
    console.log('a non-operator is refused and changes nothing');
    assert.strictEqual(partial, 0, 'a dialog packet was not read completely');
    // 6. the reset itself: a block to see the old world go, then "Yes"
    guest.quit();
    guest = null;
    const p = bot.entity.position.floored();
    const say = async (c, re) => { const r = nextChat(bot, re, 30000).catch(() => null); bot.chat(c); const m = await r; await sleep(300); return m; };
    await say(`/setblock ${p.x} ${p.y + 3} ${p.z} gold_block`, /Changed/);
    await say('/save-all', /Saved/i);
    const gone = new Promise((resolve) => bot.once('end', resolve));
    click(bot, 'esp32mc:world_reset', { seed: 'hello', type: 'normal' });
    await gone;
    bot = null;
    console.log('reset confirmed: disconnected, the server restarts');
    let joined = null;
    for (let k = 0; k < 30 && !joined; k++) {
      await sleep(1000);
      joined = await connectBot(port, 'Admin').catch(() => null);
    }
    assert(joined, 'could not join after the reset');
    bot = joined;
    const seed = String(await say('/seed', /Seed/));
    await say(`/tp Admin ${p.x} ${p.y + 6} ${p.z}`, /Teleported/);
    await waitFor(() => bot.blockAt(new Vec3(p.x, p.y + 3, p.z)), 20000, 'the old spot');
    const b = bot.blockAt(new Vec3(p.x, p.y + 3, p.z));
    console.log(`after the reset: ${seed}; where the gold block was: ${b.name}`);
    assert(/99162322/.test(seed), 'the new seed');
    assert(b.name !== 'gold_block', 'the old world is gone');
    console.log('MENU DIALOGS OK');
  } catch (e) {
    console.error('FAILED:', e);
    process.exitCode = 1;
  } finally {
    if (bot) bot.quit();
    if (guest) guest.quit();
    await srv.stop();
    fs.rmSync(tmp, { recursive: true, force: true });
  }
})();
