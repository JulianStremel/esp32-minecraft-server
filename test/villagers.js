'use strict';
// Villagers with a real client:
//  - an unemployed villager next to a lectern becomes a librarian (its clothes: the
//    VillagerData metadata)
//  - right-clicked: the merchant window and its two novice trades
//  - a trade chosen moves the payment in; the result taken pays the villager, counts the
//    use and earns experience
//  - (PC) after a restart it is still a librarian with the same trades and uses
//   node villagers.js --host 192.168.1.160
//   SERVER_BIN=~/mc-host-build/mcserver node villagers.js --local
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
if (!host && !local) { console.error('usage: node villagers.js --host <ip> | --local'); process.exit(2); }
const PORT = local ? 25600 + Math.floor(Math.random() * 300) : 25565;
const X = Number(opt('x', '120')), Z = Number(opt('z', '120')), Y = 120;
const VILLAGER_DATA = 18, LIBRARIAN = 9;
let server, op, runner, worldFile;

async function command(text, pattern, ms = 15000) {
  const r = nextChat(op, pattern, ms);
  op.chat(text);
  const line = await r;
  await sleep(400);
  return line;
}
async function startLocal(format) {
  server = startServer(['--port', String(PORT), '--seed', '42', '--ops', 'Tester', '--file', worldFile, '--size', '256',
    '--no-mobs', '--flat', ...(format ? ['--format'] : [])], { log: args.includes('--log') });
  await server.ready;
}
// the villager's data as the clients were told it, and the trades of the window
const data = new Map();   // entity id -> {type, profession, level}
let trades = null;
function watch(bot) {
  bot._client.on('entity_metadata', (p) => {
    for (const m of p.metadata) if (m.key === VILLAGER_DATA) data.set(p.entityId, m.value);
  });
  bot._client.on('trade_list', (p) => { trades = p; });
}
const professionOf = (v) => v && (v.villagerProfession !== undefined ? v.villagerProfession : v.profession);
const villager = () => Object.values(runner.entities).find((e) => e.name === 'villager');
const itemName = (id) => (runner.registry.items[id] || {}).name;
async function join() {
  runner = await connectBot(PORT, 'Runner', { host, checkTimeoutInterval: 600000 });
  runner.on('error', () => {});
  watch(runner);
  await waitFor(() => runner.entity, 20000, 'spawned');
}
async function openTrades() {
  trades = null;
  const v = villager();
  assert(v, 'no villager in sight');
  const opened = new Promise((resolve) => runner.once('windowOpen', resolve));
  runner.activateEntity(v);
  const win = await opened;
  await waitFor(() => trades && trades.windowId === win.id, 5000, 'the trades');
  return win;
}

(async () => {
  try {
    if (local) {
      worldFile = path.join(os.tmpdir(), `mc-villagers-${process.pid}.img`);
      await startLocal(true);
      host = 'localhost';
    }
    op = await connectBot(PORT, 'Tester', { host, checkTimeoutInterval: 600000 });
    op.on('error', () => {});
    op.physicsEnabled = false;
    watch(op);
    await command('/gamemode spectator', /game mode/i);
    await join();
    await command('/gamemode survival Runner', /game mode/i);
    await command(`/tp Tester ${X} ${Y + 5} ${Z - 6}`, /Teleported/);
    await sleep(2000);
    // a glass pen with a lectern, the runner outside it
    await command(`/fill ${X - 4} ${Y - 1} ${Z - 4} ${X + 4} ${Y - 1} ${Z + 8} stone`, /filled/i);
    await command(`/fill ${X - 4} ${Y} ${Z - 4} ${X + 4} ${Y + 3} ${Z + 8} air`, /filled/i);
    await command(`/fill ${X - 2} ${Y} ${Z + 1} ${X + 2} ${Y + 1} ${Z + 5} glass`, /filled/i);
    await command(`/fill ${X - 1} ${Y} ${Z + 2} ${X + 1} ${Y + 1} ${Z + 4} air`, /filled/i);
    await command(`/setblock ${X} ${Y} ${Z + 1} air`, /Changed|set/i);   // a window to trade through
    await command(`/setblock ${X + 1} ${Y} ${Z + 4} lectern`, /Changed|set/i);
    await command(`/summon villager ${X}.5 ${Y} ${Z + 3}.5`, /Summoned/);
    await command(`/tp Runner ${X}.5 ${Y} ${Z - 1}.5`, /Teleported/);
    await waitFor(() => villager(), 10000, 'the villager');

    // 1) a profession from the lectern
    const t0 = Date.now();
    await waitFor(() => professionOf(data.get(villager().id)) === LIBRARIAN, 30000, 'a librarian').catch(() => {});
    const d = data.get(villager().id);
    console.log(`  after ${((Date.now() - t0) / 1000).toFixed(1)} s: ${JSON.stringify(d)}`);
    assert.strictEqual(professionOf(d), LIBRARIAN, 'not a librarian');

    // 2) its trades
    await command('/give Runner paper 64', /Gave/);
    for (let i = 0; i < 3; i++) await command('/give Runner emerald 64', /Gave/);   // (what one trade does not use up)
    await command('/give Runner book 1', /Gave/);
    let win = await openTrades();
    const list = trades.trades.map((t) => `${t.inputItem1.itemCount} ${itemName(t.inputItem1.itemId)}` +
      `${t.inputItem2 ? ' + ' + t.inputItem2.itemCount + ' ' + itemName(t.inputItem2.itemId) : ''} -> ` +
      `${t.outputItem ? t.outputItem.itemCount + ' ' + itemName(t.outputItem.itemId) : '?'}`);
    console.log(`  window ${win.type}: ${list.join('; ')}, level ${trades.villagerLevel}`);
    assert.strictEqual(trades.trades.length, 2, 'not two trades');

    // 3) every trade once: chosen, the payment moved in, the result taken
    const xp0 = trades.experience;
    for (let i = 0; i < trades.trades.length; i++) {
      const t = trades.trades[i];
      const want = t.outputItem.itemId;
      runner._client.write('select_trade', { slot: i });
      await waitFor(() => win.slots[2] && win.slots[2].type === want, 5000, 'the result').catch(() => {});
      assert(win.slots[2] && win.slots[2].type === want, `trade ${i}: no result`);
      const before = win.slots.slice(win.inventoryStart).filter((s) => s && s.type === want).reduce((n, s) => n + s.count, 0);
      win.selectedTrade = { tradeDisabled: true };   // (mineflayer's own trading waits on slots otherwise)
      await runner.clickWindow(2, 0, 1);   // shift: into the inventory
      await waitFor(() => win.slots.slice(win.inventoryStart).filter((s) => s && s.type === want)
        .reduce((n, s) => n + s.count, 0) > before, 5000, 'the item').catch(() => {});
      const after = win.slots.slice(win.inventoryStart).filter((s) => s && s.type === want).reduce((n, s) => n + s.count, 0);
      await sleep(1000);   // (a shift click trades as often as it is paid for)
      const all = win.slots.slice(win.inventoryStart).filter((s) => s && s.type === want).reduce((n, s) => n + s.count, 0);
      console.log(`  trade ${i}: ${itemName(want)} ${before} -> ${all}, uses ${trades.trades[i].nbTradeUses}`);
      assert(after > before, `trade ${i}: nothing received`);
      if (i === 0 && itemName(t.inputItem1.itemId) === 'paper') assert.strictEqual(all - before, 2, '64 paper: not two trades');
    }
    await waitFor(() => trades.experience > xp0, 3000, 'experience').catch(() => {});
    console.log(`  the villager's experience ${xp0} -> ${trades.experience}; the runner's ${runner.experience.points}`);
    assert(trades.experience > xp0, 'the villager earned nothing');
    assert(runner.experience.points > 0, 'the runner earned nothing');
    const uses = trades.trades.map((t) => t.nbTradeUses);
    runner.closeWindow(win);
    await sleep(1000);

    // 4) after a restart: the same librarian, trades and uses (PC)
    if (local) {
      await command('/save-all', /Saved|saved/i);
      runner.quit();
      op.quit();
      await server.stop();
      data.clear();
      await startLocal(false);
      op = await connectBot(PORT, 'Tester', { host, checkTimeoutInterval: 600000 });
      op.on('error', () => {});
      await join();
      await waitFor(() => villager() && data.get(villager().id), 20000, 'the villager again');
      // 11 experience: an apprentice (the level up comes after the trading, saved or not)
      await waitFor(() => (data.get(villager().id) || {}).level === 2, 10000, 'apprentice').catch(() => {});
      win = await openTrades();
      const again = trades.trades.slice(0, 2).map((t) => t.nbTradeUses);
      console.log(`  after a restart: ${JSON.stringify(data.get(villager().id))}, uses ${again.join(', ')}`);
      assert.strictEqual(professionOf(data.get(villager().id)), LIBRARIAN, 'no librarian after a restart');
      assert.deepStrictEqual(again, uses, 'the uses were lost');
      assert.strictEqual(trades.villagerLevel, 2, 'no apprentice');
      assert.strictEqual(trades.trades.length, 4, 'not four trades as an apprentice');
      runner.closeWindow(win);
    }
    console.log(`VILLAGERS OK (${host})`);
  } finally {
    for (const b of [runner, op]) if (b) b.quit();
    if (server) await server.stop();
    if (worldFile) fs.rmSync(worldFile, { force: true });
  }
})().catch((err) => { console.error(err.stack || err); process.exitCode = 1; });
