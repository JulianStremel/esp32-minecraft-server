'use strict';
// Storage benchmark on a board: the same workload against any storage backend (NBD, SD).
//  1. write: a glass layer over 8 x 8 chunks far from spawn, then /save-all (timed);
//  2. evict: the player flies away (3000 blocks) so those chunks leave memory;
//  3. read: back again, timed until all 64 chunks are in the client with their glass
//     (each one loaded from storage, not generated).
// /storage is sampled around each step (bytes, chunks, latency).
//   node storage_bench.js --host 192.168.1.160 [--label sd] [--x 3000 --z 3000] [--version 1.21.8]
const { Vec3 } = require('vec3');
const { connectBot, nextChat, sleep, waitFor } = require('./lib');

const args = process.argv.slice(2);
const opt = (n, d) => { const i = args.indexOf('--' + n); return i < 0 ? d : args[i + 1]; };
const host = opt('host', '127.0.0.1'), label = opt('label', host);
const X0 = Math.floor(+opt('x', 3000) / 16) * 16, Z0 = Math.floor(+opt('z', 3000) / 16) * 16, Y = 150, N = 8;

(async () => {
  const extra = opt('version') ? { version: opt('version') } : {};
  // view distance 4: the workers are not busy generating a wide view, so the timings are the storage's
  const bot = await connectBot(25565, opt('name', 'Tester'), { host, checkTimeoutInterval: 600000, viewDistance: 4, ...extra });
  bot.physicsEnabled = false;
  const say = async (cmd, pattern, ms = 60000) => { const r = nextChat(bot, pattern, ms); bot.chat(cmd); return r; };
  const storage = async () => {
    const t = await say('/storage', /Storage:/);
    const n = (re) => { const m = re.exec(t); return m ? +m[1] : NaN; };
    return { read: n(/(\d+) KB read/), written: n(/(\d+) KB written/), saved: n(/saves (\d+)/), loaded: n(/loads (\d+)/),
      latency: n(/(\d+) ms latency/), errors: n(/(\d+) errors/), text: t,
      // behind a write cache (SD): what reached the card
      devReads: n(/device: (\d+) reads/), devReadKb: n(/reads \((\d+) KB\)/), devWrites: n(/(\d+) writes \(/), devWriteKb: n(/writes \((\d+) KB\)/) };
  };
  const goto = async (x, z) => { bot.chat(`/tp ${x} ${Y + 20} ${z}`); await sleep(300); };
  await say('/gamemode spectator', /./).catch(() => {});

  // 1) write
  await goto(X0 + 64, Z0 + 64);
  const inArea = (cx, cz) => bot.blockAt(new Vec3(X0 + cx * 16 + 8, Y - 10, Z0 + cz * 16 + 8));
  await waitFor(() => { for (let cx = 0; cx < N; cx++) for (let cz = 0; cz < N; cz++) if (!inArea(cx, cz)) return false; return true; },
    120000, 'the area');
  await sleep(5000);
  for (let i = 0; i < 2; i++) for (let k = 0; k < 2; k++)
    await say(`/fill ${X0 + i * 64} ${Y} ${Z0 + k * 64} ${X0 + i * 64 + 63} ${Y} ${Z0 + k * 64 + 63} glass`, /filled|Changed/i);
  await sleep(10000);   // generation around the area settles
  // a second layer right before the save: the chunks are dirty even if an autosave ran
  for (let i = 0; i < 2; i++) for (let k = 0; k < 2; k++)
    await say(`/fill ${X0 + i * 64} ${Y + 1} ${Z0 + k * 64} ${X0 + i * 64 + 63} ${Y + 1} ${Z0 + k * 64 + 63} glass`, /filled|Changed/i);
  const s0 = await storage();
  let t = Date.now();
  await say('/save-all', /Saved the game|Save failed/, 120000);
  const saveMs = Date.now() - t;
  const lag = await say('/lag', /Slowest loop/);   // the game loop during the save's last 2 s
  const s1 = await storage();

  // 2) evict: fly east along z = Z0 - 1500 until most of the world cache has turned over
  for (let d = 0; d <= 3000; d += 100) { await goto(X0 + 64 + d, Z0 - 1500); await sleep(1000); }
  await sleep(5000);
  const glassAt = (cx, cz) => { const b = bot.blockAt(new Vec3(X0 + cx * 16 + 8, Y, Z0 + cz * 16 + 8)); return b && b.name === 'glass'; };

  // 3) read back
  const s2 = await storage();
  t = Date.now();
  await goto(X0 + 64, Z0 + 64);
  let have = 0;
  await waitFor(() => {
    have = 0;
    for (let cx = 0; cx < N; cx++) for (let cz = 0; cz < N; cz++) have += glassAt(cx, cz) ? 1 : 0;
    return have === N * N;
  }, 180000, 'the saved chunks');
  const readMs = Date.now() - t;
  const s3 = await storage();
  bot.quit();

  const loaded = s3.loaded - s2.loaded;
  console.log(`[${label}] save: ${saveMs} ms for ${s1.saved - s0.saved} chunks, ${s1.written - s0.written} KB written, latency ${s1.latency} ms`);
  console.log(`[${label}] read back: ${readMs} ms until all ${N * N} marked chunks were in the client; ${loaded} chunks loaded ` +
    `from storage, ${s3.read - s2.read} KB read, latency ${s3.latency} ms, errors ${s3.errors}`);
  console.log(`[${label}] loop during the save: ${lag.split(' | ')[0]}`);
  if (!isNaN(s1.devWrites))
    console.log(`[${label}] device during the save: ${s1.devWrites - s0.devWrites} writes (${s1.devWriteKb - s0.devWriteKb} KB), ` +
      `${s1.devReads - s0.devReads} reads (${s1.devReadKb - s0.devReadKb} KB); during the read back: ` +
      `${s3.devReads - s2.devReads} reads (${s3.devReadKb - s2.devReadKb} KB)`);
  console.log(`[${label}] ${s3.text}`);
  process.exit(0);
})().catch((e) => { console.error(e); process.exit(1); });
