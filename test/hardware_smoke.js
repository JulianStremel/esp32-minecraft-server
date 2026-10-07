'use strict';
// The firmware on a real board: WiFi login, PSRAM-backed chunks, diagnostics, the
// performance banner, the serial console, NBD writes and a hardware reset. Flash the board first (tools/idf/build.sh ... flash)
// with include/config.h listing Tester as an operator, and keep its NBD server running.
//   node hardware_smoke.js --host 192.168.1.160 --serial COM5 [--python <idf python>] [--log]
// Without --serial the reset/persistence part is skipped.
const assert = require('assert');
const { spawn } = require('child_process');
const path = require('path');
const { Vec3 } = require('vec3');
const { connectBot, nextChat, waitFor, sleep, ROOT } = require('./lib');
const args = process.argv.slice(2);
const opt = (name, fallback) => {
  const i = args.indexOf('--' + name);
  return i < 0 ? fallback : args[i + 1];
};
const host = opt('host');
const port = Number(opt('port', '25565'));
const serialPort = opt('serial');
const python = opt('python', process.env.IDF_PYTHON || 'python');
if (!host) { console.error('usage: node hardware_smoke.js --host <board ip> [--serial <port>]'); process.exit(2); }
let monitor, bot;

// Serial console of the board; resetting restarts the firmware.
function startMonitor(reset) {
  const a = [path.join(ROOT, 'tools/idf/serial_monitor.py'), serialPort];
  if (reset) a.push('--reset');
  const proc = spawn(python, a, { stdio: ['pipe', 'pipe', 'pipe'] });
  const m = { proc, listening: false, failure: null, stats: [], lines: [] };
  let buf = '';
  const onData = (d) => {
    buf += d.toString();
    let nl;
    while ((nl = buf.indexOf('\n')) >= 0) {
      const line = buf.slice(0, nl).replace(/\r$/, '');
      buf = buf.slice(nl + 1);
      if (/listening on port/.test(line)) m.listening = true;
      if (/\[stat\]/.test(line)) m.stats.push(line);
      m.lines.push(line);
      if (/Guru Meditation|assert failed|\[FATAL\]|^Backtrace:|abort\(\)/.test(line)) {
        m.failure = new Error('firmware fault: ' + line);
        console.log('  device: ' + line);
      }
      if (args.includes('--log')) console.log('  | ' + line);
    }
  };
  proc.stdout.on('data', onData);
  proc.stderr.on('data', onData);
  proc.on('error', (err) => { m.failure = err; });
  m.send = (line) => proc.stdin.write(line + '\n');   // a server console command
  m.stop = () => new Promise((res) => {
    if (proc.exitCode !== null || proc.signalCode !== null) return res();
    proc.once('exit', () => res());
    proc.kill();
  });
  return m;
}

async function command(text, pattern) {
  const reply = nextChat(bot, pattern, 60000);
  bot.chat(text);
  const line = await reply;
  console.log(line);
  await sleep(650); // respect the vanilla chat rate limit
  return line;
}

async function join() {
  bot = await connectBot(port, 'Tester', { host, checkTimeoutInterval: 600000 });
  bot.physicsEnabled = false;
  await waitFor(() => bot.blockAt(bot.entity.position.offset(0, -1, 0)), 60000, 'spawn terrain');
}

(async () => {
  let block;
  try {
    if (serialPort) monitor = startMonitor(false);
    await join();
    console.log('spawned at', bot.entity.position.toString());
    await command('/gamemode creative', /game mode|gamemode/i);
    const tps = await command('/tps', /TPS/);
    assert(/wakeups\/s/.test(tps) && /overruns/.test(tps), 'tick/wakeup instrumentation missing');
    const lag = await command('/lag', /Slowest loop/);
    assert(/waits \d+/.test(lag) && /wake\(\)/.test(lag), 'select/eventfd instrumentation missing');
    await command('/workers 0', /Jobs:/);
    const jobs = await command('/workers 2', /Jobs:/);
    assert(/generated|sent/.test(jobs), 'worker instrumentation missing');
    const storage = await command('/storage', /Storage:/);
    assert(!/no storage/.test(storage), 'NBD storage not connected');
    // performance banner: a boss bar, on from the chat, refreshed every second, off from
    // the serial console
    let barUpdates = 0;
    bot.on('bossBarUpdated', () => barUpdates++);
    await command('/perfbar on', /Performance banner enabled/);
    await waitFor(() => bot.bossBars.some((b) => /^TPS \d+\.\d .*online$/.test(String(b.title))), 5000, 'banner');
    console.log('banner:', String(bot.bossBars[0].title));
    await waitFor(() => barUpdates >= 6, 5000, 'banner refresh');
    if (monitor) {
      monitor.send('perfbar off');
      await waitFor(() => monitor.lines.some((l) => /Performance banner disabled/.test(l)), 5000, 'serial console reply');
    } else {
      await command('/perfbar off', /Performance banner disabled/);
    }
    await waitFor(() => bot.bossBars.length === 0, 5000, 'banner removed');
    // the chunks around the spawn arrive over WiFi
    await sleep(3000);
    const cols = Object.keys(bot.world.async.columns || {}).length;
    console.log('columns loaded:', cols);
    assert(cols >= 25, 'too few chunk columns loaded: ' + cols);
    block = bot.entity.position.floored().offset(2, 3, 0);
    await command(`/setblock ${block.x} ${block.y} ${block.z} gold_block`, /block/i);
    await waitFor(() => bot.blockAt(block)?.name === 'gold_block', 30000, 'placed block');
    await command('/save-all', /Saved the game/);
    const measured = await command('/lag', /Slowest loop/);
    assert(Number(/waits (\d+)/.exec(measured)?.[1]) > 0, 'select waits were not recorded');
    assert(!monitor?.failure, 'firmware fault during gameplay');
    bot.quit();
    bot = null;
    if (!serialPort) {
      console.log(`HARDWARE SMOKE OK (${host}): login, terrain, diagnostics, NBD save (reset skipped, no --serial)`);
      return;
    }
    await sleep(1000);
    await monitor.stop();
    monitor = startMonitor(true);
    await waitFor(() => monitor.listening || monitor.failure, 120000, 'firmware restart');
    if (monitor.failure) throw monitor.failure;
    await join();
    await waitFor(() => bot.blockAt(new Vec3(block.x, block.y, block.z))?.name === 'gold_block',
      60000, 'saved block after reset');
    assert(!monitor.failure, 'firmware fault after reset');
    console.log(`HARDWARE SMOKE OK (${host}): login, terrain, diagnostics, NBD persistence across a reset`);
  } finally {
    if (bot) bot.quit();
    if (monitor) await monitor.stop();
  }
})().catch((err) => { console.error(err); process.exitCode = 1; });
