'use strict';
// The operator menu (/menu) with a real client: statistics, a setting changed, the
// player page, a seed typed in the chat, and (with --reset) a world reset through the
// confirmation page: the server restarts into a new world with that seed.
//   node op_menu.js --host 192.168.1.160 [--reset]      (--reset deletes that world!)
//   SERVER_BIN=~/mc-host-build/mcserver node op_menu.js --local --reset
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const net = require('net');
const { Vec3 } = require('vec3');
const { startServer, connectBot, nextChat, sleep, waitFor } = require('./lib');

const args = process.argv.slice(2);
const local = args.includes('--local'), doReset = args.includes('--reset');
const opt = (name, fallback) => {
  const i = args.indexOf('--' + name);
  return i < 0 ? fallback : args[i + 1];
};
let host = opt('host');
if (!host && !local) { console.error('usage: node op_menu.js --host <ip> [--reset] | --local [--reset]'); process.exit(2); }
const PORT = local ? 25600 + Math.floor(Math.random() * 300) : 25565;
let server, op, worldFile;

async function command(text, pattern, ms = 15000) {
  const r = nextChat(op, pattern, ms);
  op.chat(text);
  const line = await r;
  await sleep(650);
  return line;
}

const plain = (s) => {
  if (s === undefined || s === null) return '';
  // 1.21.8: names and lore are NBT text components
  if (typeof s === 'object' && s.type && s.value !== undefined) s = require('prismarine-nbt').simplify(s);
  if (typeof s !== 'string') s = JSON.stringify(s);
  try { const j = JSON.parse(s); return typeof j === 'string' ? j : (j.text || '') + (j.extra ? j.extra.map((x) => x.text || '').join('') : ''); } catch (e) { return s; }
};
const nameOf = (it) => (it ? plain(it.customName) : '');
const loreOf = (it) => (it && it.customLore ? it.customLore.map(plain).join(' / ') : '');

async function menu() {
  const w = op.currentWindow;
  if (w) return w;
  return new Promise((resolve, reject) => {
    const t = setTimeout(() => reject(new Error('no menu window')), 8000);
    op.once('windowOpen', (win) => { clearTimeout(t); resolve(win); });
  });
}

// clicks a menu button and waits for the window's contents to change
async function click(slot, button = 0) {
  const w = op.currentWindow;
  const before = JSON.stringify(w.slots.slice(0, 54).map((s) => s && [s.name, nameOf(s)]));
  op.clickWindow(slot, button, 0).catch(() => {});
  await waitFor(() => !op.currentWindow ||
    JSON.stringify(op.currentWindow.slots.slice(0, 54).map((s) => s && [s.name, nameOf(s)])) !== before, 5000, `menu after clicking ${slot}`).catch(() => {});
  await sleep(300);
}

async function join() {
  for (let i = 0; i < 60; i++) {
    try {
      op = await connectBot(PORT, 'Tester', { host, checkTimeoutInterval: 600000 });
      op.on('error', () => {});
      return;
    } catch (e) { await sleep(2000); }
  }
  throw new Error('could not join');
}

(async () => {
  try {
    if (local) {
      worldFile = path.join(os.tmpdir(), `mc-menu-${process.pid}.img`);
      server = startServer(['--port', String(PORT), '--seed', '42', '--ops', 'Tester', '--file', worldFile, '--size', '256',
        '--no-mobs', '--format'], { log: args.includes('--log') });
      await server.ready;
      host = 'localhost';
    }
    await join();
    await command('/gamemode creative', /game mode/i);
    await command('/difficulty easy', /difficulty/i);
    op.chat('/menu');
    let w = await menu();
    await sleep(500);
    const stats = w.slots[13];
    console.log(`  menu "${plain(w.title)}": ${nameOf(stats)}: ${loreOf(stats).slice(0, 120)}...`);
    assert(/statistics/i.test(nameOf(stats)) && /TPS/.test(loreOf(stats)), 'no statistics in the menu');
    // settings: the difficulty
    await click(29);
    w = op.currentWindow;
    const before = loreOf(w.slots[10]);
    await click(10);
    const after = loreOf(op.currentWindow.slots[10]);
    console.log(`  settings: difficulty ${before} -> ${after}`);
    assert(/easy/.test(before) && /normal/.test(after), 'the difficulty did not change');
    // players: our own page
    await click(45);
    await click(33);
    console.log(`  players page: ${nameOf(op.currentWindow.slots[10])} (${loreOf(op.currentWindow.slots[10])})`);
    assert.strictEqual(nameOf(op.currentWindow.slots[10]), 'Tester');
    await click(10);
    assert.strictEqual(nameOf(op.currentWindow.slots[4]), 'Tester', 'no player page');
    // the world page: a seed typed in the chat (text -> Java's hashCode, as vanilla)
    await click(45);
    await click(45);
    await click(31);
    op.clickWindow(10, 0, 0).catch(() => {});
    await waitFor(() => !op.currentWindow, 5000, 'the menu closing for the seed');
    op.chat('hello');
    w = await menu();
    await waitFor(() => /99162322/.test(loreOf(op.currentWindow.slots[10])), 5000, 'the new seed in the menu');
    console.log(`  seed typed: ${loreOf(op.currentWindow.slots[10])}`);
    if (!doReset) {
      op.closeWindow(op.currentWindow);
      console.log(`OP MENU OK (${host}, no reset)`);
      return;
    }
    // a block to see the world go
    op.closeWindow(op.currentWindow);
    const p = op.entity.position.floored();
    await command(`/setblock ${p.x} ${p.y + 3} ${p.z} gold_block`, /Changed/);
    await command('/save-all', /Saved/i, 60000);
    op.chat('/menu');
    await menu();
    await sleep(500);
    await click(31);
    await click(14);
    assert(/Delete/.test(nameOf(op.currentWindow.slots[4])), 'no confirmation page');
    const gone = new Promise((resolve) => op.once('end', resolve));
    op.clickWindow(20, 0, 0).catch(() => {});
    await gone;
    console.log('  confirmed: disconnected, the server restarts');
    await sleep(3000);
    await join();
    const seed = await command('/seed', /Seed/);
    const spawn = op.entity.position.floored();
    await command(`/tp Tester ${p.x} ${p.y + 6} ${p.z}`, /Teleported/);
    await waitFor(() => op.blockAt(new Vec3(p.x, p.y + 3, p.z)), 20000, 'the old spot loaded');
    const b = op.blockAt(new Vec3(p.x, p.y + 3, p.z));
    console.log(`  after the reset: ${seed}; where the gold block was: ${b ? b.name : 'not loaded'}; joined at ${spawn} (a new player there)`);
    assert(/99162322/.test(seed), 'not the new seed');
    assert(b && b.name !== 'gold_block', 'the old world is still there');
    console.log(`OP MENU OK (${host}, world reset)`);
  } finally {
    if (op) op.quit();
    if (server) await server.stop();
    if (worldFile) fs.rmSync(worldFile, { force: true });
  }
})().catch((err) => { console.error(err.message || err); process.exitCode = 1; });
