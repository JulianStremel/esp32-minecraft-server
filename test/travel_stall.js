'use strict';
// How long the game loop stalls when a player changes dimension into chunks nobody has
// loaded yet (the arrival spot is searched on the game loop).
//   node travel_stall.js --host 192.168.1.160 [--x 9000 --z 9000] [--trips 3]
const { connectBot, nextChat, sleep, waitFor } = require('./lib');

const args = process.argv.slice(2);
const opt = (name, fallback) => {
  const i = args.indexOf('--' + name);
  return i < 0 ? fallback : args[i + 1];
};
const host = opt('host', 'localhost');
const PORT = Number(opt('port', '25565'));
const X = Number(opt('x', '9000')), Z = Number(opt('z', '9000')), TRIPS = Number(opt('trips', '3'));
const DIM = opt('dim', 'the_nether');
let op;

async function command(text, pattern, ms = 20000) {
  const r = nextChat(op, pattern, ms);
  op.chat(text);
  const line = await r;
  await sleep(650);
  return line;
}

async function slowestLoop() {
  const line = await command('/lag', /^Slowest loop/);
  return Number(/Slowest loop: (\d+) ms/.exec(line)[1]);
}

(async () => {
  op = await connectBot(PORT, 'Tester', { host, checkTimeoutInterval: 600000 });
  op.on('error', () => {});
  op.physicsEnabled = false;
  await command('/gamemode creative', /game mode/i);
  const results = [];
  for (let t = 0; t < TRIPS; t++) {
    const x = X + t * 400, z = Z - t * 400;   // fresh chunks on every trip
    await command(`/dimension overworld`, /Mov(ed|ing) /);
    await command(`/tp Tester ${x}.5 150 ${z}.5`, /Teleported/);
    await sleep(6000);
    await slowestLoop();
    await sleep(2500);
    const before = await slowestLoop();
    const t0 = Date.now();
    await command(`/dimension ${DIM}`, /Mov(ed|ing) /);
    const cmdMs = Date.now() - t0;
    await waitFor(() => op.game.dimension === DIM, 20000, DIM);
    await sleep(300);
    const after = await slowestLoop();
    results.push({ trip: t, quietLoopMs: before, travelLoopMs: after, replyMs: cmdMs });
    console.log(`  trip ${t}: slowest loop before ${before} ms, with the travel ${after} ms (reply after ${cmdMs} ms)`);
    await sleep(4000);
  }
  await command('/dimension overworld', /Mov(ed|ing) /);
  console.log(JSON.stringify(results));
  op.quit();
})().catch((err) => { console.error(err.message || err); process.exitCode = 1; if (op) op.quit(); });
