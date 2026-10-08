'use strict';
// An item dropped into water must bob below the surface at vanilla's pace, which the
// client predicts (ItemEntity physics); a faster server bob shows as corrections.
// Records the item's height as the server sends it (client physics off).
//   node item_float.js --host 192.168.1.160 [--x 9100 --z 9100]
//   SERVER_BIN=/tmp/mc-host-build/mcserver node item_float.js --local
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
if (!host && !local) { console.error('usage: node item_float.js --host <ip> | --local'); process.exit(2); }
const PORT = local ? 25600 + Math.floor(Math.random() * 300) : 25565;
const X = Number(opt('x', local ? '40' : '9100')), Z = Number(opt('z', local ? '40' : '9100')), Y = 190;
let server, op, worldFile;

async function command(text, pattern, ms = 15000) {
  const r = nextChat(op, pattern, ms);
  op.chat(text);
  const line = await r;
  await sleep(650);
  return line;
}

(async () => {
  try {
    if (local) {
      worldFile = path.join(os.tmpdir(), `mc-item-${process.pid}.img`);
      server = startServer(['--port', String(PORT), '--ops', 'Tester', '--file', worldFile, '--size', '256', '--no-mobs',
        '--format']);
      await server.ready;
      host = 'localhost';
    }
    op = await connectBot(PORT, 'Tester', { host, checkTimeoutInterval: 600000 });
    op.on('error', () => {});
    op.physicsEnabled = false;
    await command('/gamemode creative', /game mode/i);
    await command(`/tp Tester ${X}.5 ${Y + 1} ${Z - 4}.5`, /Teleported/);
    await sleep(3000);
    // a still pool, 4 deep, surface top at Y + 1
    await command(`/fill ${X - 4} ${Y - 4} ${Z - 6} ${X + 4} ${Y + 6} ${Z + 4} air`, /filled|Success/i).catch(() => {});
    await command(`/fill ${X - 4} ${Y - 4} ${Z - 6} ${X + 4} ${Y} ${Z + 4} stone`, /filled/i);
    await command(`/fill ${X - 2} ${Y - 3} ${Z - 2} ${X + 2} ${Y} ${Z + 2} water`, /filled/i);
    await sleep(2000);
    await command(`/summon item ${X}.5 ${Y + 3} ${Z}.5`, /Summoned|Unknown/i).catch(() => {});
    let item = () => Object.values(op.entities).find((e) => e.name === 'item' && Math.abs(e.position.x - X - 0.5) < 3);
    if (!item()) {   // no /summon item: throw one
      await command(`/give Tester stone 1`, /Gave|Given/i);
      await sleep(500);
      await op.lookAt(op.entity.position.offset(0, -1.6, 4), true);
      await op.tossStack(op.inventory.items()[0]);
    }
    await waitFor(() => item(), 10000, 'the item');
    await sleep(Number(opt('settle', '15')) * 1000);   // sink and float up
    const ys = [];
    const t0 = Date.now();
    while (Date.now() - t0 < 20000) {
      const it = item();
      if (it) ys.push(it.position.y);
      await sleep(50);
    }
    assert(ys.length > 50, 'lost the item');
    let turns = 0, dir = 0;
    for (let i = 1; i < ys.length; i++) {
      const d = Math.sign(ys[i] - ys[i - 1]);
      if (d && dir && d !== dir) turns++;
      if (d) dir = d;
    }
    const lo = Math.min(...ys), hi = Math.max(...ys);
    console.log(`  item height over 20 s: ${lo.toFixed(3)} .. ${hi.toFixed(3)} (range ${(hi - lo).toFixed(3)}), ` +
      `${turns} turns; water surface at ${Y + 1 - 1 / 9}`);
    // vanilla (the decompiled 1.16.5 ItemEntity#tick, simulated): 190.39 .. 190.79, a
    // range of 0.40 and 10 turns in 20 s; the old server bobbed 3.5 times as fast (35
    // turns) and rose above the surface
    assert(hi < Y + 1 - 1 / 9, `the item rises out of the water (up to ${hi.toFixed(2)})`);
    assert(turns >= 6 && turns <= 14, `the item bobs at another pace than vanilla (${turns} turns, vanilla 10)`);
    assert(hi - lo > 0.2 && hi - lo < 0.7, `the item bobs ${(hi - lo).toFixed(2)} blocks (vanilla 0.40)`);
    console.log(`ITEM FLOAT OK (${host})`);
  } finally {
    if (op) op.quit();
    if (server) await server.stop();
    if (worldFile) fs.rmSync(worldFile, { force: true });
  }
})().catch((err) => { console.error(err.message || err); process.exitCode = 1; });
