'use strict';
// Back in survival after creative, a bare hand must not break a block at the first hit:
// the server keeps the block, and the client is told it is no longer in creative.
//   SERVER_BIN=.../mcserver node gamemode_dig.js
//   node gamemode_dig.js --host <board ip> --serial COM5 --python <python with pyserial>   (as Tester)
const assert = require('assert');
const { Vec3 } = require('vec3');
const { execFileSync } = require('child_process');
const path = require('path');
const { startServer, connectBot, sleep, waitFor, nextChat } = require('./lib');

(async () => {
  const hi = process.argv.indexOf('--host'), host = hi > 0 ? process.argv[hi + 1] : null;
  const port = host ? 25565 : 25640 + Math.floor(Math.random() * 100);
  const name = host ? 'Tester' : 'Digger';
  const srv = host ? null : startServer(['--port', String(port), '--seed', '42', '--ops', 'Digger', '--no-mobs'], { log: process.argv.includes('--log') });
  let bot;
  try {
    if (srv) await srv.ready;
    bot = await connectBot(port, name, host ? { host, checkTimeoutInterval: 600000 } : {});
    const command = async (c, pattern = /./) => { const r = nextChat(bot, pattern, 15000).catch(() => null); bot.chat(c); await r; await sleep(300); };
    await waitFor(() => bot.blockAt(bot.entity.position.offset(0, -1, 0)), 20000, 'spawn terrain');
    const arg = (k) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : null; };
    const consoleLine = (line) => {   // the server console: stdin here, the serial port on the board
      if (srv) return srv.command(line);
      execFileSync(arg('--python') || 'python', [path.join(__dirname, '..', 'tools', 'console_send.py'), arg('--serial') || 'COM5', line, '0.5']);
    };
    let seq = 100;
    const dig = (pos, status) => bot._client.write('block_dig', { status, location: pos, face: 1, sequence: seq++ });
    let abilities = null;
    bot._client.on('abilities', (pk) => { abilities = pk.flags; });
    const f = bot.entity.position.floored();
    for (const [i, via] of ['command', 'F3+F4', 'console'].entries()) {
      const pos = new Vec3(f.x + 1 + i, f.y, f.z);
      await command(`/setblock ${pos.x} ${pos.y} ${pos.z} dirt`);
      if (via === 'console') consoleLine(`gamemode creative ${name}`);
      else await command('/gamemode creative');
      await waitFor(() => bot.game.gameMode === 'creative', 5000, 'creative');
      // a creative hit breaks at once
      const c = new Vec3(f.x + 1 + i, f.y + 1, f.z);
      await command(`/setblock ${c.x} ${c.y} ${c.z} dirt`);
      dig(c, 0);
      await waitFor(() => bot.blockAt(c).name === 'air', 5000, 'creative break');
      if (via === 'command') await command('/gamemode survival');
      else if (via === 'console') consoleLine(`gamemode survival ${name}`);
      else bot._client.write('change_gamemode', { mode: 'survival' });
      await waitFor(() => bot.game.gameMode === 'survival', 5000, `survival via ${via}`);
      await sleep(500);
      assert(abilities !== null && (abilities & 0x08) === 0, `abilities still allow instant building (${abilities})`);
      // a client that predicts the break locally (a ghost hole) gets the block back
      bot.world.setBlockStateId(pos, 0);
      dig(pos, 0);
      await sleep(1000);
      const b = bot.blockAt(pos);
      console.log(`via ${via}: dirt after a bare-hand hit in survival: ${b.name}, abilities ${abilities}`);
      assert.strictEqual(b.name, 'dirt', 'the block broke at the first hit (or the client kept its predicted hole)');
      dig(pos, 1);   // stop
    }
    console.log('GAMEMODE DIG OK');
  } catch (e) {
    console.error('FAILED:', e);
    process.exitCode = 1;
  } finally {
    if (bot) bot.quit();
    if (srv) await srv.stop();
  }
})();
