// Helpers shared by the end-to-end tests.
'use strict';
const { spawn } = require('child_process');
const net = require('net');
const path = require('path');
const mineflayer = require('mineflayer');

const ROOT = path.join(__dirname, '..');
// the Minecraft version the server speaks (protocol 772)
const VERSION = '1.21.8';
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

// A kick reason as text: mineflayer passes 1.21.8's NBT text components as objects.
function kickText(reason) {
  if (typeof reason === 'string') return reason;
  const flat = (c) => (typeof c === 'string' ? c : !c ? '' : (c.text || c.translate || '') + (c.extra || []).map(flat).join(''));
  return flat(reason && reason.value !== undefined && reason.type ? require('prismarine-nbt').simplify(reason) : reason) || JSON.stringify(reason);
}

function connectBot(port, username, opts = {}) {
  return new Promise((resolve, reject) => {
    const options = { host: '127.0.0.1', port, username, version: VERSION, auth: 'offline', ...opts };
    const connect = options.connect || ((client) => client.setSocket(net.connect(options.port, options.host)));
    let client;
    // Mineflayer initializes its plugins asynchronously. A loopback server can
    // send Join Game before those listeners exist, leaving bot.world undefined.
    // Use the supported transport hook to wait for actual plugin readiness.
    const bot = mineflayer.createBot({ ...options, connect: (c) => { client = c; } });
    let settled = false;
    const t = setTimeout(() => finish(new Error(username + ': spawn timeout')), 20000);
    const onSpawn = () => finish();
    const onKick = (r) => finish(new Error(username + ' kicked: ' + kickText(r)));
    const onEnd = (r) => finish(new Error(username + ' disconnected before spawn: ' + r));
    function finish(error) {
      if (settled) return;
      settled = true;
      clearTimeout(t);
      bot.removeListener('spawn', onSpawn);
      bot.removeListener('kicked', onKick);
      bot.removeListener('error', finish);
      bot.removeListener('end', onEnd);
      if (error) {
        bot.end();
        reject(error);
      } else {
        resolve(bot);
      }
    }
    bot.once('spawn', onSpawn);
    bot.once('kicked', onKick);
    bot.once('error', finish);
    bot.once('end', onEnd);
    bot.once('inject_allowed', () => {
      if (settled) return;
      try { connect(client); } catch (e) { finish(e); }
    });
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

// Block light as the server sent it. Light arrays travel as bytes (vanilla's DataLayer:
// two nibbles a byte, the low one first); mineflayer's chunk reads them as 64-bit words,
// which reverses the bytes within every 8, so its getBlockLight at local x answers for
// local x 14 - 2 * (x >> 1) + (x & 1) of the same row. This reads the right cell.
function blockLight(bot, pos) {
  const lx = Math.floor(pos.x) & 15;
  const mx = 14 - 2 * (lx >> 1) + (lx & 1);
  return bot.world.getBlockLight(pos.offset(mx - lx, 0, 0));
}

module.exports = { startServer, connectBot, sleep, waitFor, nextChat, kickText, blockLight, ROOT, VERSION };
