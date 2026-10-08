'use strict';
// Light across chunk borders on a real board: a light source in one chunk must light
// the next one, and changing it must update the neighbour's light too.
//   node hardware_light.js --host 192.168.1.160 [--x 4096 --z 4100]
// Builds a stone platform in the sky across the chunk border at x = BX (a multiple of
// 16) and reads the block light the client receives (Tester must be an operator).
const assert = require('assert');
const { Vec3 } = require('vec3');
const { connectBot, nextChat, sleep, waitFor } = require('./lib');

const args = process.argv.slice(2);
const opt = (name, fallback) => {
  const i = args.indexOf('--' + name);
  return i < 0 ? fallback : args[i + 1];
};
const host = opt('host');
if (!host) { console.error('usage: node hardware_light.js --host <board ip>'); process.exit(2); }
const BX = Number(opt('x', '4096')) & ~15, Z0 = Number(opt('z', '4100')), Y = 160;
let bot;

async function command(text, pattern) {
  const r = nextChat(bot, pattern, 15000);
  bot.chat(text);
  const line = await r;
  await sleep(650);   // vanilla chat rate limit
  return line;
}

// mineflayer (prismarine-chunk 1.16) reads each 16-block row of a light array as one
// big-endian long, which permutes x within the row: local x t is found at
// 14 - 2 * (t >> 1) + (t & 1) (the mapping is its own inverse). The server sends the
// vanilla layout; undo the permutation to read what the real client sees.
const clientX = (x) => (x & ~15) + 14 - 2 * ((x & 15) >> 1) + (x & 1);
const blockLight = (x, y, z) => bot.world.getBlockLight(new Vec3(clientX(x), y, z));

async function expectLight(x, want, what) {
  await waitFor(() => blockLight(x, Y + 1, Z0) === want, 10000, `${what}: block light ${want} at x=${x}`)
    .catch(() => { throw new Error(`${what}: block light at x=${x} is ${blockLight(x, Y + 1, Z0)}, expected ${want}`); });
  console.log(`  ${what}: block light ${want} at x=${x} ok`);
}

(async () => {
  try {
    bot = await connectBot(25565, 'Tester', { host, checkTimeoutInterval: 600000 });
    bot.physicsEnabled = false;
    await command('/gamemode spectator', /game mode/i);
    await command(`/tp Tester ${BX} ${Y + 6} ${Z0}`, /Teleported/);
    await sleep(3000);
    await command(`/fill ${BX - 10} ${Y} ${Z0 - 6} ${BX + 10} ${Y + 4} ${Z0 + 6} air`, /filled|no blocks/i).catch(() => {});
    await command(`/fill ${BX - 10} ${Y} ${Z0 - 6} ${BX + 10} ${Y} ${Z0 + 6} stone`, /filled/);

    // a light source 2 blocks west of the border lights the first columns east of it
    await command(`/setblock ${BX - 2} ${Y + 1} ${Z0} glowstone`, /block/i);
    await expectLight(BX - 1, 14, 'glowstone, own chunk');
    await expectLight(BX, 13, 'glowstone, across the border');
    await expectLight(BX + 5, 8, 'glowstone, 7 blocks away');

    // changing it updates the neighbour's light as well. (mineflayer ignores the "empty
    // section" masks of light updates, so the test keeps some light in the section: a bot
    // cannot see light going back to 0, the real client can.)
    await command(`/setblock ${BX - 2} ${Y + 1} ${Z0} torch`, /block/i);
    await expectLight(BX, 12, 'replaced by a torch, across the border');
    await command(`/setblock ${BX - 2} ${Y + 1} ${Z0} air`, /block/i);
    await command(`/setblock ${BX - 4} ${Y + 1} ${Z0} torch`, /block/i);
    await expectLight(BX, 10, 'torch moved away, across the border');
    await expectLight(BX + 3, 7, 'torch moved away, 7 blocks away');
    await command(`/setblock ${BX - 4} ${Y + 1} ${Z0} air`, /block/i);
    console.log(`HARDWARE LIGHT OK (${host}): light crosses the chunk border at x=${BX} and follows edits`);
  } finally {
    if (bot) {
      bot.on('error', () => {});
      bot.quit();
    }
  }
})().catch((err) => { console.error(err.message || err); process.exitCode = 1; });
