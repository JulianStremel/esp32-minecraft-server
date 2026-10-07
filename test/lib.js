// Helpers shared by the end-to-end tests.
'use strict';
const { spawn } = require('child_process');
const path = require('path');
const mineflayer = require('mineflayer');

const ROOT = path.join(__dirname, '..');
const SERVER_BIN = process.env.SERVER_BIN || path.join(ROOT, 'host', 'build', 'mcserver');

function startServer(args, { log = false } = {}) {
  const proc = spawn(SERVER_BIN, args, { stdio: ['pipe', 'pipe', 'pipe'] });
  let output = '';
  const onData = (d) => {
    output += d.toString();
    if (log) process.stderr.write('[server] ' + d.toString());
  };
  proc.stdout.on('data', onData);
  proc.stderr.on('data', onData);
  const ready = new Promise((resolve, reject) => {
    const t = setTimeout(() => reject(new Error('server did not start:\n' + output)), 15000);
    const check = () => {
      if (output.includes('listening on port')) { clearTimeout(t); resolve(); } else setTimeout(check, 50);
    };
    check();
    proc.on('exit', (code) => reject(new Error('server exited with ' + code + '\n' + output)));
  });
  return {
    proc,
    ready,
    output: () => output,
    command: (line) => proc.stdin.write(line + '\n'),
    stop: () => new Promise((resolve) => {
      if (proc.exitCode !== null || proc.signalCode !== null) return resolve(proc.exitCode);
      proc.on('exit', (code) => resolve(code));
      proc.kill('SIGTERM');
    }),
  };
}

function connectBot(port, username, opts = {}) {
  return new Promise((resolve, reject) => {
    const bot = mineflayer.createBot({ host: '127.0.0.1', port, username, version: '1.16.5', auth: 'offline', ...opts });
    const t = setTimeout(() => reject(new Error(username + ': spawn timeout')), 20000);
    bot.once('spawn', () => { clearTimeout(t); resolve(bot); });
    bot.once('kicked', (r) => { clearTimeout(t); reject(new Error(username + ' kicked: ' + r)); });
    bot.once('error', (e) => { clearTimeout(t); reject(e); });
  });
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function waitFor(fn, ms = 10000, what = 'condition') {
  const end = Date.now() + ms;
  while (Date.now() < end) {
    const v = await fn();
    if (v) return v;
    await sleep(50);
  }
  throw new Error('timeout waiting for ' + what);
}

function nextChat(bot, pattern, ms = 5000) {
  return new Promise((resolve, reject) => {
    const t = setTimeout(() => { bot.removeListener('message', on); reject(new Error('no chat matching ' + pattern)); }, ms);
    function on(msg) {
      const s = msg.toString();
      if (pattern.test(s)) { clearTimeout(t); bot.removeListener('message', on); resolve(s); }
    }
    bot.on('message', on);
  });
}

module.exports = { startServer, connectBot, sleep, waitFor, nextChat, ROOT };
