'use strict';
// CPU/core-idle profiling using an isolated FreeRTOS runtime-stats build.
// node test/emulator_profile.js [--board esp32s3-8] [--seconds 20] [--bots 6]
//   [--out build/profiles/s3] [--no-build] [--scenario all|empty|inline|two-workers]
// Each scenario boots a fresh world. Movement and measurement use the firmware's
// esp_timer time; host CPU accounting is separate. No browser or QEMU participates.
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const { execFileSync } = require('child_process');
const { startEmulator, ROOT } = require('./emulator');
const { connectBot, nextChat, sleep, waitFor } = require('./lib');
const args = process.argv.slice(2);
const opt = (name, fallback) => args.includes('--' + name) ? args[args.indexOf('--' + name) + 1] : fallback;
const board = opt('board', 'esp32s3-8');
const seconds = Number(opt('seconds', '20'));
const count = Number(opt('bots', '6'));
const scenario = opt('scenario', 'all');
if (!['all', 'empty', 'inline', 'two-workers'].includes(scenario)) throw new Error('Invalid --scenario');
const out = path.resolve(ROOT, opt('out', 'build/profiles/s3'));
if (!/^esp32s3-8$/.test(board) || !(seconds > 0 && seconds <= 30) ||
    !Number.isInteger(count) || !(count >= 1 && count <= 8)) {
  throw new Error('Use an S3 board, 1–30 guest seconds and 1–8 bots');
}
fs.mkdirSync(out, { recursive: true });
const hz = Number(execFileSync('getconf', ['CLK_TCK'], { encoding: 'utf8' }).trim());

function cpuSnapshot(pid) {
  const s = fs.readFileSync(`/proc/${pid}/stat`, 'utf8');
  const fields = s.slice(s.lastIndexOf(')') + 2).split(' ');
  return { wall_ns: process.hrtime.bigint(), user: Number(fields[11]), system: Number(fields[12]) };
}

async function run(name, workers, build) {
  const trace = path.join(out, name + '.trace.json');
  if (fs.existsSync(trace)) throw new Error('Refusing to overwrite ' + trace + '; choose a fresh --out');
  const device = startEmulator({ board, port: 25584, build, trace, cpuProfile: true });
  const cpuSamples = [];
  let sampleError = null;
  let serialBuffer = '';
  device.proc.stdout.on('data', (data) => {
    serialBuffer += data.toString();
    let nl;
    while ((nl = serialBuffer.indexOf('\n')) >= 0) {
      const line = serialBuffer.slice(0, nl).trim();
      serialBuffer = serialBuffer.slice(nl + 1);
      if (line.startsWith('[cpu] ')) {
        try { cpuSamples.push(JSON.parse(line.slice(6))); } catch (err) { sampleError = err; }
      }
    }
  });
  const guestTime = () => cpuSamples.at(-1)?.us || 0;
  const bots = [];
  let chunks = 0;
  let ended = false;
  const check = () => {
    if (sampleError) throw sampleError;
    if (device.fault()) throw device.fault();
    if (device.proc.exitCode !== null || device.proc.signalCode !== null) throw new Error('emulator exited');
    if (ended) throw new Error('a profiling client disconnected');
  };
  const advance = async (us) => {
    const limit = Date.now() + 600000;
    while (guestTime() < us) {
      check();
      if (Date.now() > limit) throw new Error('guest clock progress timeout');
      await sleep(200);
    }
  };
  const command = async (text, match) => {
    const reply = nextChat(bots[0], match, 60000);
    bots[0].chat(text);
    return reply;
  };
  try {
    console.log(`Starting ${name}: ${board}, ${workers === null ? 0 : count} clients`);
    await device.ready;
    await waitFor(() => guestTime() > 0, 60000, 'FreeRTOS runtime counters');
    const children = execFileSync('ps', ['--ppid', String(device.proc.pid), '-o', 'pid=,comm='], { encoding: 'utf8' });
    const pid = children.split('\n').map((line) => line.trim().split(/\s+/))
      .find(([, name]) => name === 'esp-emu')?.[0];
    if (!pid) throw new Error('cannot find esp-emu child for host CPU accounting');
    if (workers !== null) {
      for (let i = 0; i < count; i++) {
        const bot = await connectBot(25584, 'Bot' + i, { checkTimeoutInterval: 600000 });
        bot.physicsEnabled = false;
        bot.on('end', () => { ended = true; });
        bot._client.on('map_chunk', () => chunks++);
        bots.push(bot);
      }
      console.log(await command(`/workers ${workers}`, /^Jobs:/));
      for (const bot of bots) bots[0].chat(`/gamemode creative ${bot.username}`);
    }
    // Exclude boot/join/pool-switch and allow the command queue to settle.
    await advance(guestTime() + 3000000);
    const start = guestTime();
    const first = cpuSamples.at(-1);
    const hostStart = cpuSnapshot(pid);
    const stop = start + seconds * 1000000;
    let nextMove = start;
    let move = 0;
    chunks = 0;
    const limit = Date.now() + 1200000;
    while (guestTime() < stop) {
      check();
      if (Date.now() > limit) throw new Error('profile exceeded host deadline');
      const now = guestTime();
      if (bots.length && now >= nextMove) {
        for (let i = 0; i < bots.length; i++) {
          const a = i * Math.PI * 2 / bots.length;
          const radius = 32 + 16 * move;
          bots[0].chat(`/tp ${bots[i].username} ${(120 + Math.cos(a) * radius).toFixed(1)} 140 ${(-120 + Math.sin(a) * radius).toFixed(1)}`);
        }
        move++;
        nextMove = now + 2000000;
      }
      await sleep(200);
    }
    const last = cpuSamples.at(-1);
    const hostEnd = cpuSnapshot(pid);
    const hostSeconds = Number(hostEnd.wall_ns - hostStart.wall_ns) / 1e9;
    const cpuSeconds = (hostEnd.user + hostEnd.system - hostStart.user - hostStart.system) / hz;
    const result = { name, board, workers, clients: bots.length, start_us: start, end_us: last.us,
      guest_seconds: (last.us - start) / 1e6, requested_seconds: seconds, first, last, host_seconds: hostSeconds, host_cpu_seconds: cpuSeconds,
      host_cpu_percent_one_core: 100 * cpuSeconds / hostSeconds,
      guest_seconds_per_host_second: (last.us - start) / 1e6 / hostSeconds, moves: move, chunks,
      trace: path.basename(trace), serial: name + '.serial.log' };
    if (bots.length) {
      result.tps_after = await command('/tps', /^TPS/);
      result.lag_after = await command('/lag', /^Slowest loop/);
      if (!chunks) throw new Error('no terrain delivered during the profile');
    }
    check();
    fs.writeFileSync(path.join(out, name + '.window.json'), JSON.stringify(result, null, 2) + '\n');
    console.log(`${name}: ${result.guest_seconds.toFixed(3)} guest seconds / ${hostSeconds.toFixed(1)} host seconds, ${result.chunks} chunks`);
    return result;
  } finally {
    bots.forEach((b) => b.quit());
    await device.stop();
    fs.copyFileSync(device.logPath, path.join(out, name + '.serial.log'));
    fs.writeFileSync(path.join(out, name + '.cpu.json'), JSON.stringify(cpuSamples, null, 2) + '\n');
  }
}

(async () => {
  const results = [];
  const scenarios = [['empty', null, 'empty'], [`${count}_clients_inline`, 0, 'inline'], [`${count}_clients_two_workers`, 2, 'two-workers']];
  for (const [name, workers, key] of scenarios) {
    if (scenario !== 'all' && scenario !== key) continue;
    results.push(await run(name, workers, results.length === 0 && !args.includes('--no-build')));
  }
  const elf = path.join(ROOT, 'build', board + '-emulator-cpu-profile', 'mcserver.elf');
  const metadata = { captured_at: new Date().toISOString(), board,
    elf_sha256: crypto.createHash('sha256').update(fs.readFileSync(elf)).digest('hex'),
    emulator: execFileSync(process.env.ESP_EMU || 'esp-emu', ['--version'], { encoding: 'utf8' }).trim(),
    results };
  fs.writeFileSync(path.join(out, 'run.json'), JSON.stringify(metadata, null, 2) + '\n');
  execFileSync('python3', [path.join(ROOT, 'tools/emulator/summarize_profile.py'), out], { stdio: 'inherit' });
})().catch((err) => { console.error(err); process.exitCode = 1; });
