'use strict';
// What mobs cost the game loop: ms per tick (from /tps) with no mobs, then with a set of
// mobs summoned around a spectator in a closed arena (they stay put: nobody to chase).
//   node mob_load.js --host 192.168.1.160 [--dim the_nether] [--mobs ghast:6,magma_cube:12,zombified_piglin:12]
//   SERVER_BIN=~/mc-host-build/mcserver node mob_load.js --local
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
const PORT = local ? 25600 + Math.floor(Math.random() * 300) : 25565;
const DIM = opt('dim', 'the_nether');
const MOBS = opt('mobs', 'ghast:6,magma_cube:12,zombified_piglin:12').split(',').map((s) => s.split(':'));
const X = Number(opt('x', local ? '300' : '2400')), Z = Number(opt('z', local ? '300' : '2400')), Y = 80, SECONDS = Number(opt('seconds', '30'));
let server, op, worldFile;

async function command(text, pattern, ms = 15000) {
  const r = nextChat(op, pattern, ms);
  op.chat(text);
  const line = await r;
  await sleep(650);
  return line;
}

// average of /tps ms/tick samples over `seconds`
async function measure(seconds) {
  const v = [];
  const t0 = Date.now();
  while (Date.now() - t0 < seconds * 1000) {
    const line = await command('/tps', /ms\/tick/);
    const m = /([\d.]+) ms\/tick/.exec(line);
    if (m) v.push(Number(m[1]));
    await sleep(2000);
  }
  return v.reduce((a, b) => a + b, 0) / v.length;
}

(async () => {
  try {
    if (local) {
      worldFile = path.join(os.tmpdir(), `mc-load-${process.pid}.img`);
      server = startServer(['--port', String(PORT), '--seed', '42', '--ops', 'Tester', '--file', worldFile, '--size', '256',
        '--no-mobs', '--format']);
      await server.ready;
      host = 'localhost';
    }
    op = await connectBot(PORT, 'Tester', { host, checkTimeoutInterval: 600000 });
    op.on('error', () => {});
    op.physicsEnabled = false;
    await command('/gamemode spectator', /game mode/i);
    await command('/difficulty normal', /difficulty/i);
    if (op.game.dimension !== DIM) {
      await command(`/dimension ${DIM}`, /Mov(ed|ing) Tester/);
      await waitFor(() => op.game.dimension === DIM, 30000, DIM);
    }
    await command(`/tp Tester ${X} ${Y + 8} ${Z}`, /Teleported/);
    await sleep(5000);
    await command(`/fill ${X - 19} ${Y - 1} ${Z - 19} ${X + 19} ${Y + 14} ${Z + 19} glass`, /filled/i);
    await command(`/fill ${X - 18} ${Y} ${Z - 18} ${X + 18} ${Y + 13} ${Z + 18} air`, /filled/i);
    await command(`/fill ${X - 18} ${Y - 1} ${Z - 18} ${X + 18} ${Y - 1} ${Z + 18} netherrack`, /filled/i);
    await command('/kill @e[type=!player]', /Killed/);
    await sleep(5000);
    const base = await measure(SECONDS);
    let n = 0;
    for (const [type, count] of MOBS)
      for (let i = 0; i < Number(count); i++) {
        const a = (n++ * 137.5) * Math.PI / 180, r = 5 + (n % 4) * 3;
        await command(`/summon ${type} ${(X + Math.cos(a) * r).toFixed(1)} ${type === 'ghast' ? Y + 5 : Y} ${(Z + Math.sin(a) * r).toFixed(1)}`, /Summoned|Unable/i);
      }
    await sleep(3000);
    const loaded = await measure(SECONDS);
    const counts = {};
    for (const e of Object.values(op.entities)) if (e.name && e.name !== 'player') counts[e.name] = (counts[e.name] || 0) + 1;
    console.log(`  ${DIM}: ${base.toFixed(2)} ms/tick without mobs, ${loaded.toFixed(2)} with ${JSON.stringify(counts)}`);
    console.log(JSON.stringify({ dim: DIM, base, loaded, mobs: counts }));
    await command('/kill @e[type=!player]', /Killed/);
  } finally {
    if (op) op.quit();
    if (server) await server.stop();
    if (worldFile) fs.rmSync(worldFile, { force: true });
  }
})().catch((err) => { console.error(err.message || err); process.exitCode = 1; });
