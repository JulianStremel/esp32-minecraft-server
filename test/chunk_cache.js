'use strict';
// The chunk cache (storage/packet_cache.h) with a real client: a view of chunks, then far
// away, then back: the chunks come from the cache (its counters in /storage) and are the
// same blocks; after a restart of the server they still come from it.
// The time until a view is there is measured each time (generated against cached).
//   SERVER_BIN=~/mc-host-build/mcserver node chunk_cache.js
//   node chunk_cache.js --host 192.168.1.160 [--at x,z]   (a board with a chunk cache; no restart)
const assert = require('assert');
const os = require('os');
const path = require('path');
const fs = require('fs');
const { startServer, connectBot, nextChat, sleep, waitFor } = require('./lib');

const args = process.argv.slice(2);
const opt = (name, fallback) => { const i = args.indexOf('--' + name); return i >= 0 ? args[i + 1] : fallback; };
const remote = opt('host', null);
const PORT = remote ? 25565 : 25600 + (process.pid % 300);
const [AX, AZ] = (opt('at', '500,500')).split(',').map(Number);
const world = path.join(os.tmpdir(), `mc-cache-world-${process.pid}.img`);
const cache = path.join(os.tmpdir(), `mc-cache-${process.pid}.img`);
let server, bot;
const results = [];

async function command(text, pattern, ms = 30000) {
  const r = nextChat(bot, pattern, ms);
  bot.chat(text);
  const line = await r;
  await sleep(300);
  return line;
}

function start() {
  return startServer(['--port', String(PORT), '--seed', '42', '--ops', 'Tester', '--generator', '20', '--file', world,
    '--size', '256', '--format', '--view', '6', '--radius', '4096', '--cache-file', cache, '--cache-size', '64'], { log: process.argv.includes('--log') });
}

// blocks of a column grid around a point, as a signature of what the client got
function signature(cx, cz) {
  const out = [];
  for (let dx = -40; dx <= 40; dx += 8)
    for (let dz = -40; dz <= 40; dz += 8)
      for (let y = 50; y <= 110; y += 6) {
        const b = bot.blockAt(bot.entity.position.clone().set(cx + dx, y, cz + dz));
        out.push(b ? b.name : '?');
      }
  return out.join(',');
}

// the time until the chunks 5 around (x, z) are all there
async function viewAt(x, z) {
  const t0 = Date.now();
  await command(`/tp Tester ${x} 160 ${z}`, /Teleported/);
  const all = () => {
    for (let dx = -80; dx <= 80; dx += 16)
      for (let dz = -80; dz <= 80; dz += 16)
        if (!bot.blockAt(bot.entity.position.clone().set(x + dx, 0, z + dz))) return false;
    return true;
  };
  await waitFor(all, 300000, 'the view there');
  const ms = Date.now() - t0;
  await sleep(3000);
  return ms;
}

(async () => {
  try {
    if (!remote) {
      server = start();
      await server.ready;
    }
    bot = await connectBot(PORT, 'Tester', { host: remote || 'localhost', checkTimeoutInterval: 600000 });
    bot.on('error', () => {});
    await waitFor(() => bot.entity, 30000, 'spawned');
    await command('/gamemode spectator', /mode/i);
    let ms = await viewAt(AX, AZ);
    const first = signature(AX, AZ);
    let st = await command('/storage', /chunk cache/);
    const hits0 = Number((/(\d+) hits/.exec(st) || [0, 0])[1]);
    results.push(`first visit: ${ms} ms for the view; ${st.trim()}`);
    await viewAt(AX + 3000, AZ);   // far away: the first view's chunks are evicted
    ms = await viewAt(AX, AZ);
    const second = signature(AX, AZ);
    st = await command('/storage', /chunk cache/);
    const hits = Number((/(\d+) hits/.exec(st) || [0, 0])[1]) - hits0;
    results.push(`back again: ${ms} ms for the view; ${st.trim()}`);
    assert(hits > 50, 'chunks from the cache');
    assert.strictEqual(second, first, 'the same blocks as generated');
    if (remote) {
      console.log('PASS chunk_cache\n  ' + results.join('\n  '));
      return;
    }
    // a restart: the cache still knows the chunks
    bot.quit();
    await server.stop();
    server = start();
    await server.ready;
    bot = await connectBot(PORT, 'Tester', { host: 'localhost', checkTimeoutInterval: 600000 });
    bot.on('error', () => {});
    await waitFor(() => bot.entity, 30000, 'spawned again');
    ms = await viewAt(AX, AZ);
    const third = signature(AX, AZ);
    st = await command('/storage', /chunk cache/);
    results.push(`after a restart: ${ms} ms for the view; ${st.trim()}`);
    assert(Number((/(\d+) hits/.exec(st) || [0, 0])[1]) > 50, 'chunks from the cache after a restart');
    assert.strictEqual(third, first, 'the same blocks after a restart');
    console.log('PASS chunk_cache\n  ' + results.join('\n  '));
  } catch (e) {
    console.log('FAIL chunk_cache: ' + (e && e.stack || e) + '\n  ' + results.join('\n  '));
    process.exitCode = 1;
  } finally {
    try { bot && bot.quit(); } catch (e) {}
    if (server) await server.stop();
    if (!remote) for (const f of [world, cache]) try { fs.unlinkSync(f); } catch (e) {}
    setTimeout(() => process.exit(), 500);
  }
})();
