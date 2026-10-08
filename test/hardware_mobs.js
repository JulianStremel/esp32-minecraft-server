'use strict';
// Mob spawning by light level on a real board: at noon in clear weather, hostile mobs
// must still spawn (in caves and other dark places) but never under the open sky.
//   node hardware_mobs.js --host 192.168.1.160 [--seconds 90] [--x 2000 --z 2000]
// Tester must be an operator. Mobs are only seen within the server's tracking range, so
// the check covers the spawns near the bot.
const assert = require('assert');
const { Vec3 } = require('vec3');
const { connectBot, nextChat, sleep } = require('./lib');

const args = process.argv.slice(2);
const opt = (name, fallback) => {
  const i = args.indexOf('--' + name);
  return i < 0 ? fallback : args[i + 1];
};
const host = opt('host');
if (!host) { console.error('usage: node hardware_mobs.js --host <board ip>'); process.exit(2); }
const SECONDS = Number(opt('seconds', '90'));
const X = Number(opt('x', '2000')), Z = Number(opt('z', '2000'));
const HOSTILE = new Set(['zombie', 'skeleton', 'creeper', 'spider']);
const PASSIVE = new Set(['pig', 'cow', 'sheep', 'chicken']);
let bot;

async function command(text, pattern) {
  const r = nextChat(bot, pattern, 15000);
  bot.chat(text);
  const line = await r;
  await sleep(650);   // vanilla chat rate limit
  return line;
}

// is there anything but air above (x, y, z) up to the top of the world?
function covered(pos) {
  const p = pos.floored();
  for (let y = p.y + 2; y < 256; y++) {
    const b = bot.blockAt(new Vec3(p.x, y, p.z));
    if (!b) return null;   // not loaded: unknown
    if (b.name !== 'air' && b.name !== 'cave_air') return true;
  }
  return false;
}

(async () => {
  try {
    bot = await connectBot(25565, 'Tester', { host, checkTimeoutInterval: 600000 });
    bot.physicsEnabled = false;
    await command('/gamemode creative', /game mode/i);   // spectators do not make mobs spawn
    await command('/difficulty normal', /difficulty/i);
    await command('/weather clear', /weather/i);
    // noon first: nothing near the new spot may have spawned at night
    await command('/time set 6000', /time/i);
    await command(`/tp Tester ${X} 120 ${Z}`, /Teleported/);
    await sleep(5000);   // terrain around the new spot
    const stats = async () => Number(/(\d+) mobs spawned/.exec(await command('/workers', /^Spawning:/))[1]);
    const before = await stats();
    const seen = { hostileDark: 0, hostileOpen: 0, hostileUnknown: 0, passive: 0 };
    const onSpawn = (e) => {
      const name = e.name || '';
      if (PASSIVE.has(name)) { seen.passive++; return; }
      if (!HOSTILE.has(name)) return;
      const c = covered(e.position);
      if (c === true) seen.hostileDark++;
      else if (c === false) {
        seen.hostileOpen++;
        console.log(`  hostile ${name} under the open sky at ${e.position.floored()}`);
      } else seen.hostileUnknown++;
    };
    bot.on('entitySpawn', onSpawn);
    const t0 = Date.now();
    while (Date.now() - t0 < SECONDS * 1000) {
      await sleep(10000);
      await command('/time set 6000', /time/i);   // stay at noon
      console.log(`  ${Math.round((Date.now() - t0) / 1000)}s: ${JSON.stringify(seen)}`);
    }
    bot.removeListener('entitySpawn', onSpawn);
    const spawned = (await stats()) - before;
    const spawning = await command('/workers', /^Spawning:/);
    console.log(`  ${spawning}`);
    console.log(`  server spawned ${spawned} mobs; near the bot: ${seen.hostileDark} hostile in the dark, ` +
      `${seen.hostileOpen} hostile under the open sky, ${seen.passive} passive`);
    assert(spawned > 0, 'nothing spawned');
    assert(seen.hostileDark > 0, 'no hostile mob spawned in the dark by day (caves)');
    assert.strictEqual(seen.hostileOpen, 0, 'hostile mobs spawned under the open sky at noon');
    console.log(`HARDWARE MOBS OK (${host}): hostile mobs spawn by day only where it is dark`);
  } finally {
    if (bot) {
      bot.on('error', () => {});
      bot.quit();
    }
  }
})().catch((err) => { console.error(err.message || err); process.exitCode = 1; });
