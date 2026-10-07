'use strict';
// Boots the firmware in QEMU through tools/qemu/run.sh (headless) with a fresh world
// image, collects its serial output and stops it again. Used by qemu_load.js and
// record_gif.js.
const { spawn } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { waitFor } = require('./lib');

const ROOT = path.resolve(__dirname, '..');

// opts: { port, nbdPort, icount (shift) | mttcg (bool), build (bool), env (build dir name), log (bool) }
function startQemu(opts) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'mcqemu-'));
  const world = path.join(dir, 'world.img');
  const runArgs = [path.join(ROOT, 'tools/qemu/run.sh'), '--headless', '--port', String(opts.port), '--nbd-port',
    String(opts.nbdPort || opts.port + 1000), '--world', world];
  if (opts.icount) runArgs.push('--icount', String(opts.icount));
  else if (opts.mttcg !== false) runArgs.push('--mttcg');
  if (!opts.build) runArgs.push('--no-build');
  if (opts.env) runArgs.push('--env', opts.env);
  const proc = spawn('bash', runArgs, { stdio: ['ignore', 'pipe', 'pipe'] });
  const logPath = path.join(dir, 'serial.log');
  const log = fs.createWriteStream(logPath);
  const stats = [];
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
      if (/Guru Meditation|assert failed|\[FATAL\]|Backtrace:/.test(line)) console.log('  device: ' + line);
      if (opts.log) console.log('  | ' + line);
    }
  };
  proc.stdout.on('data', onData);
  proc.stderr.on('data', onData);
  return {
    proc, dir, logPath, stats,
    ready: waitFor(() => listening || proc.exitCode !== null, 900000, 'firmware boot').then(() => {
      if (!listening) throw new Error('QEMU exited before the server started (see ' + logPath + ')');
    }),
    stop: () => new Promise((res) => {
      if (proc.exitCode !== null) return res();
      proc.once('exit', () => res());
      proc.kill('SIGTERM');
      setTimeout(() => { try { proc.kill('SIGKILL'); } catch (e) { /* gone */ } res(); }, 10000);
    }),
  };
}

module.exports = { startQemu, ROOT };
