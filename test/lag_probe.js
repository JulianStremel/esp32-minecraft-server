'use strict';
// Joins a server and prints /lag every 2 s: where the slowest loop pass of each window went.
//   node lag_probe.js --host 192.168.1.160 [--seconds 30] [--name Tester]
const { connectBot, sleep } = require('./lib');

const args = process.argv.slice(2);
const opt = (n, d) => { const i = args.indexOf('--' + n); return i < 0 ? d : args[i + 1]; };
const host = opt('host', '127.0.0.1'), seconds = +opt('seconds', 30), name = opt('name', 'Tester');

(async () => {
  const t0 = Date.now();
  const bot = await connectBot(25565, name, { host, checkTimeoutInterval: 600000 });
  console.log(`joined after ${Date.now() - t0} ms`);
  bot.on('message', (m) => {
    const t = m.toString();
    if (/Slowest loop/.test(t)) console.log(`${((Date.now() - t0) / 1000).toFixed(1)} s  ${t}`);
  });
  for (let s = 0; s < seconds; s += 2) {
    bot.chat('/lag');
    await sleep(2000);
  }
  bot.quit();
  await sleep(500);
  process.exit(0);
})().catch((e) => { console.error(e); process.exit(1); });
