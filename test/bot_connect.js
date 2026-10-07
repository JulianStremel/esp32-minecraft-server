'use strict';
// Regression: a fast loopback server must not outrun Mineflayer's plugin setup.
const assert = require('assert');
const mineflayer = require('mineflayer');
const { startServer, connectBot, waitFor, sleep } = require('./lib');

const originalCreateBot = mineflayer.createBot;
let earlyPackets = 0;
const created = [];
mineflayer.createBot = (options) => {
  const bot = originalCreateBot(options);
  created.push(bot);
  const emit = bot.emit;
  let ready = false;
  // Make the existing asynchronous plugin initialization race deterministic.
  bot.emit = function (event, ...args) {
    if (event === 'inject_allowed') {
      setTimeout(() => { ready = true; emit.call(this, event, ...args); }, 100);
      return true;
    }
    return emit.call(this, event, ...args);
  };
  bot._client.on('packet', () => { if (!ready) earlyPackets++; });
  return bot;
};

(async () => {
  const port = 25606;
  const srv = startServer(['--port', String(port), '--flat', '--no-mobs', '--view', '2']);
  let bot;
  try {
    await srv.ready;
    bot = await connectBot(port, 'ReadyBot');
    assert.strictEqual(earlyPackets, 0, 'server packets arrived before plugins were ready');
    assert(bot.world, 'Join Game must initialize the client world');
    await waitFor(() => bot.blockAt(bot.entity.position.offset(0, -1, 0)), 10000, 'spawn terrain');
    assert.strictEqual(bot._client.state, 'play');
    await assert.rejects(connectBot(port, 'No'), /kicked:.*Invalid username/);
    await waitFor(() => created[created.length - 1]._client.socket.destroyed, 5000, 'rejected client closed');
    assert.strictEqual(earlyPackets, 0, 'rejected clients also wait for plugin initialization');
    console.log('BOT CONNECT OK: delayed plugin initialization, login, terrain and rejection cleanup');
  } finally {
    mineflayer.createBot = originalCreateBot;
    created.forEach((b) => b.end());
    await sleep(300);
    await srv.stop();
  }
})().catch((e) => { console.error('FAILED:', e); process.exitCode = 1; });
