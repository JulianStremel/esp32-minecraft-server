'use strict';
// Boots the firmware in esp-emulator through tools/emulator/run.sh (headless) with a fresh world
// image, collects its serial output and stops it again. Used by emulator_load.js and
// record_gif.js.
const { spawn } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { waitFor } = require('./lib');

const ROOT = path.resolve(__dirname, '..');

// opts: { port, nbdPort, board, build, log, world, trace, cpuProfile }
function startEmulator(opts) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'mc-emulator-'));
  const world = opts.world || path.join(dir, 'world.img');
  const runArgs = [path.join(ROOT, 'tools/emulator/run.sh'), '--headless', '--port', String(opts.port), '--nbd-port',
    String(opts.nbdPort || 10809), '--world', world];
  if (!opts.build) runArgs.push('--no-build');
  runArgs.push('--board', opts.board || 'esp32s3-8');
  if (opts.trace) runArgs.push('--trace', opts.trace);
  if (opts.cpuProfile) runArgs.push('--cpu-profile');
  const proc = spawn('bash', runArgs, { stdio: ['ignore', 'pipe', 'pipe'] });
  const logPath = path.join(dir, 'serial.log');
  const log = fs.createWriteStream(logPath);
  const stats = [];
  let failure = null;
  proc.on('error', (err) => { failure = err; });
  let buf = '';
  let listening = false;
  const onData = (d) => {
    log.write(d);
    buf += d.toString();
    let nl;
    while ((nl = buf.indexOf('\n')) >= 0) {
      const line = buf.slice(0, nl).replace(/\r$/, '');
      buf = buf.slice(nl + 1);
      if (/listening on port/.test(line)) listening = true;
      if (/^\[stat\]/.test(line)) stats.push(line);
      if (/Guru Meditation|assert failed|\[FATAL\]|^Backtrace:|^Firmware abort:|^CPU fault/.test(line)) {
        failure = new Error('firmware fault: ' + line);
        console.log('  device: ' + line);
      }
      if (opts.log) console.log('  | ' + line);
    }
  };
  proc.stdout.on('data', onData);
  proc.stderr.on('data', onData);
  return {
    proc, dir, logPath, stats,
    fault: () => failure,
    ready: waitFor(() => listening || failure || proc.exitCode !== null || proc.signalCode !== null, 900000, 'firmware boot').then(() => {
      if (failure) throw failure;
      if (!listening) throw new Error('esp-emulator exited before the server started (see ' + logPath + ')');
    }),
    stop: () => new Promise((res) => {
      if (proc.exitCode !== null || proc.signalCode !== null) return res();
      const timer = setTimeout(() => { proc.kill('SIGKILL'); res(); }, 10000);
      proc.once('exit', () => { clearTimeout(timer); res(); });
      proc.kill('SIGTERM');

    }),
  };
}

module.exports = { startEmulator, ROOT };
