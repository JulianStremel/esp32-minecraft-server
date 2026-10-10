'use strict';
// Vanilla's structures (generator version 10) with a real client:
//  - /locate structure finds a desert pyramid; at its place the client sees the pyramid
//    (the blue terracotta in the middle of the floor, its towers)
//  - its chests below hold loot, and a chest opens with it
//   node structures.js --host 192.168.1.160     (a world made with generator 10)
//   SERVER_BIN=~/mc-host-build/mcserver node structures.js --local
const assert = require('assert');
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
if (!host && !local) { console.error('usage: node structures.js --host <ip> | --local'); process.exit(2); }
const PORT = local ? 25600 + Math.floor(Math.random() * 300) : 25565;
let server, op;
const results = [];

async function command(text, pattern, ms = 30000) {
  const r = nextChat(op, pattern, ms);
  op.chat(text);
  const line = await r;
  await sleep(500);
  return line;
}

(async () => {
  try {
    if (local) {
      const file = path.join(os.tmpdir(), `mc-structures-${process.pid}.img`);
      server = startServer(['--port', String(PORT), '--seed', '42', '--ops', 'Tester', '--generator', '10', '--file', file,
        '--size', '256', '--format', '--view', '6', '--radius', '4096'], { log: args.includes('--log') });
      await server.ready;
      host = 'localhost';
    }
    op = await connectBot(PORT, 'Tester', { host, checkTimeoutInterval: 600000 });
    op.on('error', () => {});
    await waitFor(() => op.entity, 30000, 'spawned');
    await command('/gamemode creative', /mode/i);

    const line = await command('/locate structure minecraft:desert_pyramid', /nearest|Could not/);
    const m = /\[(-?\d+), ~, (-?\d+)\]/.exec(line);
    assert(m, 'locate: ' + line);
    const x = Number(m[1]), z = Number(m[2]);
    results.push(`locate: ${line.trim()}`);
    await command(`/tp Tester ${x + 10} 120 ${z + 10}`, /Teleported|tp/i);
    await waitFor(() => op.blockAt(op.entity.position.offset(0, -60, 0)), 30000, 'chunks there');
    await sleep(3000);
    const blue = op.findBlocks({ matching: (b) => b && b.name === 'blue_terracotta', maxDistance: 64, count: 4 });
    const chests = op.findBlocks({ matching: (b) => b && b.name === 'chest', maxDistance: 96, count: 8 });
    const tnt = op.findBlocks({ matching: (b) => b && b.name === 'tnt', maxDistance: 96, count: 16 });
    results.push(`the pyramid: ${blue.length} blue terracotta, ${chests.length} chests, ${tnt.length} tnt`);
    assert.strictEqual(blue.length, 1, 'one blue terracotta');
    assert.strictEqual(chests.length, 4, 'four chests');
    assert.strictEqual(tnt.length, 9, 'nine tnt');

    // open a chest: stand beside it (the treasure room is open above it)
    const c = chests[0];
    await command(`/tp Tester ${c.x + 0.5} ${c.y + 1} ${c.z + 0.5}`, /Teleported|tp/i);
    await sleep(1500);
    const win = await op.openContainer(op.blockAt(c));
    const items = win.containerItems();
    results.push(`a chest: ${items.map((i) => `${i.count} ${i.name}`).join(', ')}`);
    assert(items.length > 0, 'the chest holds loot');
    win.close();
    console.log('PASS structures\n  ' + results.join('\n  '));
  } catch (e) {
    console.log('FAIL structures: ' + (e && e.stack || e) + '\n  ' + results.join('\n  '));
    process.exitCode = 1;
  } finally {
    try { op && op.quit(); } catch (e) {}
    if (server) server.stop();
    setTimeout(() => process.exit(), 500);
  }
})();
