'use strict';
// Mobs chasing a player across chunk borders, and climbing single blocks on the way.
// The player (Runner) moves away in steps along +x over several chunk borders; a zombie
// and a creeper must follow, and walls of single blocks are in the way.
//   node path_border.js --host 192.168.1.160 [--x 8000 --z 8000]
//   SERVER_BIN=/tmp/mc-host-build/mcserver node path_border.js --local
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { startServer, connectBot, nextChat, sleep } = require('./lib');

const args = process.argv.slice(2);
const opt = (name, fallback) => {
  const i = args.indexOf('--' + name);
  return i < 0 ? fallback : args[i + 1];
};
const local = args.includes('--local');
let host = opt('host');
if (!host && !local) { console.error('usage: node path_border.js --host <ip> | --local'); process.exit(2); }
const PORT = local ? 25600 + Math.floor(Math.random() * 300) : 25565;
// start 4 blocks before a chunk border
const X = Math.floor(Number(opt('x', local ? '0' : '8000')) / 16) * 16 + 4, Z = Number(opt('z', local ? '8' : '8008'));
const Y = 180;   // a platform in the sky: no terrain in the way
let server, op, runner, worldFile;

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
      worldFile = path.join(os.tmpdir(), `mc-path-${process.pid}.img`);
      server = startServer(['--port', String(PORT), '--ops', 'Tester', '--file', worldFile, '--size', '256', '--no-mobs',
        '--format']);
      await server.ready;
      host = 'localhost';
    }
    op = await connectBot(PORT, 'Tester', { host, checkTimeoutInterval: 600000 });
    op.on('error', () => {});
    op.physicsEnabled = false;
    await command('/gamemode spectator', /game mode/i);
    await command('/difficulty normal', /difficulty/i);
    await command('/time set 18000', /time/i);
    await command(`/tp Tester ${X + 30} ${Y + 15} ${Z}`, /Teleported/);
    await sleep(3000);
    // a 3-wide lane 80 blocks long, walls along it, single-block steps across it every 12 blocks
    await command(`/fill ${X - 6} ${Y} ${Z - 2} ${X + 74} ${Y + 4} ${Z + 2} air`, /filled|Success/i).catch(() => {});
    await command(`/fill ${X - 6} ${Y} ${Z - 2} ${X + 74} ${Y} ${Z + 2} stone`, /filled/i);
    await command(`/fill ${X - 6} ${Y + 1} ${Z - 2} ${X + 74} ${Y + 3} ${Z - 2} glass`, /filled/i);
    await command(`/fill ${X - 6} ${Y + 1} ${Z + 2} ${X + 74} ${Y + 3} ${Z + 2} glass`, /filled/i);
    for (let x = X + 8; x < X + 70; x += 12)
      await command(`/fill ${x} ${Y + 1} ${Z - 1} ${x} ${Y + 1} ${Z + 1} stone`, /filled/i);

    runner = await connectBot(PORT, 'Runner', { host, checkTimeoutInterval: 600000 });
    runner.on('error', () => {});
    await command('/gamemode survival Runner', /game mode/i);
    await command(`/tp Runner ${X + 8}.5 ${Y + 2} ${Z}.5`, /Teleported/);
    await sleep(1500);
    await command(`/summon zombie ${X - 4}.5 ${Y + 1} ${Z}.5`, /Summoned/i);
    await command(`/summon creeper ${X - 4}.5 ${Y + 1} ${Z - 1}.5`, /Summoned/i);

    const mob = (name) => Object.values(runner.entities).find((e) => e.name === name && Math.abs(e.position.z - Z) < 4);
    const log = [];
    let rx = X + 8;
    const t0 = Date.now();
    // the runner keeps 9 blocks ahead of the slower of the two
    while (rx < X + 66 && Date.now() - t0 < 120000) {
      await sleep(1000);
      const zb = mob('zombie'), cr = mob('creeper');
      const zx = zb ? zb.position.x : NaN, cx = cr ? cr.position.x : NaN;
      log.push(`${((Date.now() - t0) / 1000).toFixed(0)}s runner ${rx} zombie ${zx.toFixed(1)}/${zb ? zb.position.y.toFixed(1) : '-'} ` +
        `creeper ${cx.toFixed(1)}/${cr ? cr.position.y.toFixed(1) : '-'}`);
      const slow = Math.min(zx, cx);
      if (slow > rx - 9) {
        rx = Math.min(rx + 6, X + 66);
        runner.chat(`/tp Runner ${rx}.5 ${Y + 2} ${Z}.5`);   // not an op: Tester does it
        op.chat(`/tp Runner ${rx}.5 ${Y + 2} ${Z}.5`);
      }
    }
    for (const l of log) console.log('  ' + l);
    const zb = mob('zombie'), cr = mob('creeper');
    const borders = (x) => Math.floor(x / 16) - Math.floor(X / 16);
    console.log(`  zombie crossed ${zb ? borders(zb.position.x) : 0} chunk borders, creeper ${cr ? borders(cr.position.x) : 0}`);
    assert(zb && zb.position.x > X + 50, `the zombie did not follow (at ${zb ? zb.position : 'gone'})`);
    assert(cr && cr.position.x > X + 50, `the creeper did not follow (at ${cr ? cr.position : 'gone'})`);
    console.log(`PATH BORDER OK (${host})`);
  } finally {
    for (const b of [runner, op]) if (b) b.quit();
    if (server) await server.stop();
    if (worldFile) fs.rmSync(worldFile, { force: true });
  }
})().catch((err) => { console.error(err.message || err); process.exitCode = 1; });
