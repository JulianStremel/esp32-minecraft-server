'use strict';
// What the dragon fight costs the game loop: ms per tick (/tps) for a spectator in the
// End beyond the fight's range (no dragon), then over the island with the fight on.
//   node end_load.js --host 192.168.1.160 [--seconds 20]
const { connectBot, nextChat, sleep, waitFor } = require('./lib');

const args = process.argv.slice(2);
const opt = (name, fallback) => {
  const i = args.indexOf('--' + name);
  return i < 0 ? fallback : args[i + 1];
};
const host = opt('host', 'localhost'), SECONDS = Number(opt('seconds', '20'));
let op;

async function command(text, pattern, ms = 15000) {
  const r = nextChat(op, pattern, ms);
  op.chat(text);
  const line = await r;
  await sleep(650);
  return line;
}

async function measure(seconds) {
  const v = [];
  const t0 = Date.now();
  while (Date.now() - t0 < seconds * 1000) {
    const m = /([\d.]+) ms\/tick/.exec(await command('/tps', /ms\/tick/));
    if (m) v.push(Number(m[1]));
    await sleep(2000);
  }
  return v.reduce((a, b) => a + b, 0) / v.length;
}

(async () => {
  op = await connectBot(25565, 'Tester', { host, checkTimeoutInterval: 600000 });
  op.on('error', () => {});
  op.physicsEnabled = false;
  await command('/gamemode spectator', /game mode/i);
  await command('/dragon reset', /reset/);
  if (op.game.dimension !== 'the_end') {
    await command('/dimension the_end', /Mov(ed|ing) Tester/);
    await waitFor(() => op.game.dimension === 'the_end', 30000, 'the End');
  }
  await command('/tp Tester 400 80 0', /Teleported/);   // beyond the fight's 192 blocks
  await sleep(10000);
  const base = await measure(SECONDS);
  await command('/tp Tester 0.5 100 30.5', /Teleported/);
  await waitFor(() => Object.values(op.entities).some((e) => e.name === 'ender_dragon'), 30000, 'the dragon');
  await sleep(5000);
  const fight = await measure(SECONDS);
  const status = await command('/dragon status', /Dragon fight/);
  console.log(`  the End: ${base.toFixed(2)} ms/tick without the fight, ${fight.toFixed(2)} with it (${status})`);
  console.log(JSON.stringify({ base, fight }));
  await command('/dragon reset', /reset/);
  await command('/dimension overworld', /Mov(ed|ing) Tester/);
  await sleep(2000);
  op.quit();
})().catch((err) => { console.error(err.message || err); process.exitCode = 1; if (op) op.quit(); });
