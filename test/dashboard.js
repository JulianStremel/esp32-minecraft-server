'use strict';
// The HTTP status dashboard (firmware built with -D MC_DASHBOARD=ON; the PC build has it
// with --dashboard PORT): the page, the JSON with a player in it, errors, more
// connections than it serves at once, and what polling it costs the game loop.
//   node dashboard.js --host 192.168.1.160 [--http-port 80] [--seconds 20]
//   SERVER_BIN=~/mc-host-build/mcserver node dashboard.js --local
const assert = require('assert');
const http = require('http');
const zlib = require('zlib');
const { startServer, connectBot, sleep, waitFor } = require('./lib');

const args = process.argv.slice(2);
const local = args.includes('--local');
const opt = (name, fallback) => {
  const i = args.indexOf('--' + name);
  return i < 0 ? fallback : args[i + 1];
};
let host = opt('host');
if (!host && !local) { console.error('usage: node dashboard.js --host <ip> [--http-port 80] | --local'); process.exit(2); }
const PORT = local ? 25600 + Math.floor(Math.random() * 300) : 25565;
const HTTP_PORT = local ? PORT + 1000 : Number(opt('http-port', 80));
const SECONDS = Number(opt('seconds', 20));
let server, bot;

// a raw GET (no automatic decompression): status, headers, body bytes and the time it took
function get(path, headers = {}, method = 'GET') {
  return new Promise((resolve, reject) => {
    const t0 = process.hrtime.bigint();
    const req = http.request({ host, port: HTTP_PORT, path, method, headers, agent: false }, (res) => {
      const parts = [];
      res.on('data', (d) => parts.push(d));
      res.on('end', () => resolve({ status: res.statusCode, headers: res.headers, body: Buffer.concat(parts),
        ms: Number(process.hrtime.bigint() - t0) / 1e6 }));
    });
    req.setTimeout(10000, () => req.destroy(new Error('timeout')));
    req.on('error', reject);
    req.end();
  });
}
const status = async () => JSON.parse((await get('/api/status')).body.toString());
const median = (a) => [...a].sort((x, y) => x - y)[Math.floor(a.length / 2)];

// polls /api/status every `everyMs` for `seconds`; the server's tick figures meanwhile
async function pollFor(seconds, everyMs) {
  const end = Date.now() + seconds * 1000;
  const lat = [], mspt = [], stall = [], tps = [];
  while (Date.now() < end) {
    const t = Date.now();
    const r = await get('/api/status');
    assert.strictEqual(r.status, 200);
    const s = JSON.parse(r.body.toString());
    lat.push(r.ms); mspt.push(s.perf.mspt); stall.push(s.perf.stallMax); tps.push(s.perf.tps);
    const wait = everyMs - (Date.now() - t);
    if (wait > 0) await sleep(wait);
  }
  const last = await status();
  return { requests: lat.length, latMedian: median(lat), latMax: Math.max(...lat), tpsMin: Math.min(...tps),
    msptMedian: median(mspt), stallMax: Math.max(...stall), buildMaxUs: last.dashboard.maxUs,
    buildAvgUs: last.dashboard.avgUs };
}

async function main() {
  if (local) {
    host = '127.0.0.1';
    server = startServer(['--port', String(PORT), '--seed', '42', '--dashboard', String(HTTP_PORT), '--mem', '64']);
    await server.ready;
  }

  // the page: gzipped HTML with an ETag, then a 304 for the same ETag
  const page = await get('/', { 'Accept-Encoding': 'gzip' });
  assert.strictEqual(page.status, 200);
  assert.strictEqual(page.headers['content-encoding'], 'gzip');
  const html = zlib.gunzipSync(page.body).toString();
  assert.ok(html.startsWith('<!doctype html>') && html.includes('/api/status'), 'the page');
  const etag = page.headers.etag;
  assert.ok(etag, 'an ETag');
  const again = await get('/', { 'If-None-Match': etag });
  assert.strictEqual(again.status, 304);
  console.log(`page: ${page.body.length} bytes gzipped (${html.length} unpacked) in ${page.ms.toFixed(1)} ms, 304 when unchanged`);

  const ico = await get('/favicon.png');
  assert.strictEqual(ico.status, 200);
  assert.ok(ico.body.subarray(0, 4).equals(Buffer.from([0x89, 0x50, 0x4e, 0x47])), 'a PNG');
  assert.strictEqual((await get('/nope')).status, 404);
  assert.strictEqual((await get('/api/status', {}, 'POST')).status, 405);

  // the JSON, then with a player in it
  let s = await status();
  for (const k of ['server', 'perf', 'memory', 'world', 'players', 'entities', 'jobs', 'dashboard']) assert.ok(k in s, k);
  const before = s.players.online;
  bot = await connectBot(PORT, 'DashBot', { host, checkTimeoutInterval: 600000 });
  await waitFor(async () => (await status()).players.list.some((p) => p.name === 'DashBot'), 10000, 'DashBot listed');
  s = await status();
  const me = s.players.list.find((p) => p.name === 'DashBot');
  assert.strictEqual(me.dim, 'overworld');
  assert.ok(Math.abs(me.x - bot.entity.position.x) < 3 && Math.abs(me.z - bot.entity.position.z) < 3, 'position');
  assert.strictEqual(s.players.online, before + 1);
  console.log(`status: ${JSON.stringify(s).length} bytes JSON, TPS ${s.perf.tps.toFixed(1)}, ${s.memory.heap} KB free, ` +
    `${s.players.online} online, ${s.memory.chunks} chunks`);

  // more connections than it serves at once: the extra ones are closed, the server lives on
  const burst = await Promise.allSettled(Array.from({ length: 6 }, () => get('/api/status')));
  const ok = burst.filter((r) => r.status === 'fulfilled' && r.value.status === 200).length;
  assert.ok(ok >= 2, `at least 2 of 6 parallel requests answered (${ok})`);
  s = await status();
  console.log(`6 parallel requests: ${ok} answered, ${s.dashboard.refused} refused so far`);

  // what polling costs: as the page does (every 2 s), then 10 per second
  const slow = await pollFor(SECONDS, 2000);
  const fast = await pollFor(SECONDS, 100);
  console.log('polling        requests  latency med/max ms  TPS min  ms/tick med  stall max ms  build avg/max us');
  for (const [name, r] of [['every 2 s', slow], ['10 per s', fast]])
    console.log(`${name.padEnd(14)} ${String(r.requests).padStart(8)}  ${r.latMedian.toFixed(1).padStart(7)} / ${r.latMax.toFixed(1).padEnd(8)}` +
      `  ${r.tpsMin.toFixed(1).padStart(7)}  ${r.msptMedian.toFixed(2).padStart(11)}  ${String(r.stallMax).padStart(12)}  ${String(r.buildAvgUs).padStart(9)} / ${r.buildMaxUs}`);
  assert.ok(fast.tpsMin >= 19, 'TPS stays up while polled 10 times a second');
  console.log('dashboard OK');
}

main().then(() => 0, (e) => { console.error(e.stack || e); if (server) console.error(server.output().slice(-3000)); return 1; })
  .then(async (code) => {
    if (bot) bot.end();
    if (server) await server.stop();
    process.exit(code);
  });
