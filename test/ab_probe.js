'use strict';
// A/B load probe for firmware comparisons: one player flies east at 10 blocks/s for 70 s
// over fresh terrain (chunks stream the whole time) and /tps is sampled every 2 s.
// Prints the medians and maxima of ms/tick and the loop stall, the minimum free internal
// RAM, and the chunks the player received.
//   node ab_probe.js --host 192.168.1.160 [--seconds 70] [--x 200 --z -184] [--speed 10] [--version 1.16.5]
const { connectBot, sleep } = require('./lib');

const args = process.argv.slice(2);
const opt = (n, d) => { const i = args.indexOf('--' + n); return i < 0 ? d : args[i + 1]; };
const host = opt('host', '127.0.0.1'), seconds = +opt('seconds', 70), speed = +opt('speed', 10);
const x0 = +opt('x', 200), z0 = +opt('z', -184), y = 120;

(async () => {
  const extra = opt('version') ? { version: opt('version') } : {};
  const bot = await connectBot(25565, opt('name', 'Tester'), { host, checkTimeoutInterval: 600000, ...extra });
  bot.physicsEnabled = false;
  const samples = [];
  let chunks = 0;
  bot._client.on('map_chunk', () => chunks++);
  bot.on('message', (m) => {
    const t = m.toString();
    const a = /TPS ([\d.]+), ([\d.]+) ms\/tick \(max (\d+)\), max loop stall (\d+) ms.*internal (\d+) KB \(min (\d+) KB\)/.exec(t);
    if (a) samples.push({ tps: +a[1], mspt: +a[2], tickMax: +a[3], stall: +a[4], internal: +a[5], internalMin: +a[6] });
  });
  bot.chat('/gamemode spectator');
  await sleep(1000);
  const start = Date.now();
  let nextSample = 0;
  while (Date.now() - start < seconds * 1000) {
    const t = (Date.now() - start) / 1000;
    bot.chat(`/tp ${(x0 + speed * t).toFixed(1)} ${y} ${z0}`);
    if (t >= nextSample) { bot.chat('/tps'); nextSample += 2; }
    await sleep(1000);
  }
  await sleep(1500);
  bot.quit();
  const med = (k) => { const v = samples.map((s) => s[k]).sort((a, b) => a - b); return v[Math.floor(v.length / 2)]; };
  const max = (k) => Math.max(...samples.map((s) => s[k]));
  console.log(`samples ${samples.length}, chunks received ${chunks}`);
  console.log(`ms/tick median ${med('mspt')} max ${max('mspt')} | tick max ${max('tickMax')} ms | loop stall median ${med('stall')} max ${max('stall')} ms | TPS min ${Math.min(...samples.map((s) => s.tps))} | internal RAM min ${Math.min(...samples.map((s) => s.internalMin))} KB`);
  process.exit(0);
})().catch((e) => { console.error(e); process.exit(1); });
