'use strict';
// Only record_gif.js uses QEMU. Firmware validation stays in emulator.js.
const { spawn } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { waitFor } = require('./lib');

function startCaptureQemu(opts) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'mc-qemu-capture-'));
  const logPath = path.join(dir, 'serial.log');
  const log = fs.createWriteStream(logPath);
  const args = [path.resolve(__dirname, '../tools/capture/run_qemu.sh'),
    '--port', String(opts.port), '--world', path.join(dir, 'world.img')];
  if (!opts.build) args.push('--no-build');
  const proc = spawn('bash', args, { stdio: ['ignore', 'pipe', 'pipe'] });
  let listening = false;
  let failure = null;
  let buf = '';
  proc.on('error', (err) => { failure = err; });
  proc.on('close', () => log.end());
  const onData = (data) => {
    log.write(data);
    buf += data.toString();
    let nl;
    while ((nl = buf.indexOf('\n')) >= 0) {
      const line = buf.slice(0, nl).replace(/\r$/, '');
      buf = buf.slice(nl + 1);
      if (/listening on port/.test(line)) listening = true;
      if (/Guru Meditation|assert failed|\[FATAL\]|^Backtrace:|^Firmware abort:/.test(line)) {
        failure = new Error('capture firmware fault: ' + line);
      }
      if (opts.log) console.log('  | ' + line);
    }
  };
  proc.stdout.on('data', onData);
  proc.stderr.on('data', onData);
  return {
    logPath,
    fault: () => failure || (proc.exitCode !== null || proc.signalCode !== null
      ? new Error('QEMU capture exited; see ' + logPath) : null),
    ready: waitFor(() => listening || failure || proc.exitCode !== null || proc.signalCode !== null,
      900000, 'QEMU capture boot').then(() => {
      if (failure) throw failure;
      if (!listening) throw new Error('QEMU capture failed; see ' + logPath);
    }),
    stop: () => new Promise((resolve) => {
      if (proc.exitCode !== null || proc.signalCode !== null) return resolve();
      proc.once('exit', resolve);
      proc.kill('SIGTERM');
    }),
  };
}

module.exports = { startCaptureQemu };
