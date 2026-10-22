'use strict';
// The board's internal RAM (WiFi, lwIP, small allocations) along a few steps: /tps after each.
//   node heap_probe.js --host 192.168.1.160
const { connectBot, nextChat, sleep, waitFor } = require('./lib');

const args = process.argv.slice(2);
const host = args[args.indexOf('--host') + 1];
let op;

async function command(text, pattern, ms = 20000) {
  const r = nextChat(op, pattern, ms);
  op.chat(text);
  const line = await r;
  await sleep(500);
  return line;
}
async function heap(label) {
  const line = await command('/tps', /internal/);
  const m = /heap (\d+) KB, internal (\d+) KB \(min (\d+) KB\), (\d+) chunks/.exec(line);
  console.log(`${label.padEnd(36)} heap ${m[1]} KB, internal ${m[2]} KB (min ${m[3]}), ${m[4]} chunks`);
}

(async () => {
  op = await connectBot(25565, 'Tester', { host, checkTimeoutInterval: 600000 });
  op.on('error', () => {});
  op.physicsEnabled = false;
  await waitFor(() => op.entity, 30000, 'spawned');
  await heap('joined');
  await sleep(15000);
  await heap('15 s later');
  await command('/gamemode spectator', /game mode/i);
  await command('/dimension the_end', /Mov(ed|ing) Tester/);
  await sleep(10000);
  await heap('the End, 10 s');
  await command('/tp Tester 300 94 -20', /Teleported/);
  await sleep(10000);
  await heap('the End at 300, 10 s');
  await command('/fill 281 79 -19 319 94 19 glass', /filled/i);
  await heap('after a fill');
  await command('/dimension overworld', /Mov(ed|ing) Tester/);
  await sleep(15000);
  await heap('back in the overworld, 15 s');
  op.quit();
  await sleep(3000);
  process.exit(0);
})().catch((e) => { console.error(e.message || e); process.exit(1); });
