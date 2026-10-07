'use strict';
// Real ESP-IDF firmware: protocol/login, PSRAM-backed chunks, instrumentation,
// NBD writes and a cold restart. The same check covers both chip families.
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { Vec3 } = require('vec3');
const { startEmulator } = require('./emulator');
const { connectBot, nextChat, waitFor, sleep } = require('./lib');
const args = process.argv.slice(2);
const opt = (name, fallback) => {
  const i = args.indexOf('--' + name);
  return i < 0 ? fallback : args[i + 1];
};
const board = opt('board', 'esp32s3-8');
const port = Number(opt('port', '25581'));
const nbdPort = Number(opt('nbd-port', '10809'));
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'mc-idf-smoke-'));
const world = path.join(dir, 'world.img');
const trace = path.join(dir, 'trace.json');
let device, bot;

async function command(text, pattern) {
  const reply = nextChat(bot, pattern, 60000);
  bot.chat(text);
  const line = await reply;
  console.log(line);
  await sleep(650); // respect the existing vanilla chat rate limit
  return line;
}

async function boot(build, recordTrace) {
  device = startEmulator({ board, port, nbdPort, world, build,
    trace: recordTrace ? trace : undefined, log: args.includes('--log') });
  console.log('serial log: ' + device.logPath);
  await device.ready;
  bot = await connectBot(port, 'Tester', { checkTimeoutInterval: 600000 });
  bot.physicsEnabled = false;
  await waitFor(() => bot.blockAt(bot.entity.position.offset(0, -1, 0)), 60000, 'spawn terrain');
  assert(!device.fault(), 'firmware boot must not panic');
}

(async () => {
  let block;
  try {
    await boot(!args.includes('--no-build'), true);
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
    block = bot.entity.position.floored().offset(2, 3, 0);
    await command(`/setblock ${block.x} ${block.y} ${block.z} gold_block`, /block/i);
    await waitFor(() => bot.blockAt(block)?.name === 'gold_block', 30000, 'placed block');
    await command('/save-all', /Saved the game/);
    await waitFor(() => device.stats.length, 120000, 'serial status instrumentation');
    const serial = device.stats[device.stats.length - 1];
    assert(/\[stat\] TPS [0-9.]+/.test(serial) && /min free heap \d+ KB/.test(serial),
      'serial status metrics missing');
    const measured = await command('/lag', /Slowest loop/);
    assert(Number(/waits (\d+)/.exec(measured)?.[1]) > 0, 'select waits were not recorded');
    assert(Number(/(\d+) wake\(\)/.exec(measured)?.[1]) > 0, 'eventfd wakeups were not recorded');
    assert(!device.fault(), 'firmware fault during gameplay');
    bot.quit();
    await sleep(1000);
    await device.stop();
    const data = fs.readFileSync(trace, 'utf8');
    // esp-emu permits an unterminated trace array on forced exit; retain those events.
    const events = JSON.parse(data.trim().endsWith(']') ? data : data.replace(/,\s*$/, '') + ']');
    assert(events.length > 0, 'no trace events');
    assert(events.some((e) => e.ph === 'X' && e.name === 'minecraft' && e.dur > 0),
      'server execution slices missing from trace');
    console.log('task trace: ' + trace);
    await boot(false, false);
    await waitFor(() => bot.blockAt(new Vec3(block.x, block.y, block.z))?.name === 'gold_block',
      60000, 'saved block after cold restart');
    assert(!device.fault(), 'firmware fault after restart');
    console.log(`ESP-IDF EMULATOR SMOKE OK (${board}): login, terrain, diagnostics, trace, NBD persistence`);
  } finally {
    if (bot) bot.quit();
    if (device) await device.stop();
  }
})().catch((err) => { console.error(err); process.exitCode = 1; });
