'use strict';
// Mob path finding on a real board: a zombie must walk around a wall (through its one
// gap) to reach a player, on a platform in the sky it must not walk off.
//   node hardware_path.js --host 192.168.1.160 [--x 6000 --z 6000] [--seconds 40]
// Tester (an operator) builds the course and watches as a spectator; Runner is the
// zombie's target (survival).
const assert = require('assert');
const { Vec3 } = require('vec3');
const { connectBot, nextChat, sleep, waitFor } = require('./lib');

const args = process.argv.slice(2);
const opt = (name, fallback) => {
  const i = args.indexOf('--' + name);
  return i < 0 ? fallback : args[i + 1];
};
const host = opt('host');
if (!host) { console.error('usage: node hardware_path.js --host <board ip>'); process.exit(2); }
const X = Number(opt('x', '6000')), Z = Number(opt('z', '6000')), Y = 170;
const SECONDS = Number(opt('seconds', '40'));
let op, runner;

async function command(text, pattern) {
  const r = nextChat(op, pattern, 15000);
  op.chat(text);
  const line = await r;
  await sleep(650);   // vanilla chat rate limit
  return line;
}

(async () => {
  try {
    op = await connectBot(25565, 'Tester', { host, checkTimeoutInterval: 600000 });
    op.physicsEnabled = false;
    await command('/gamemode spectator', /game mode/i);
    await command('/difficulty normal', /difficulty/i);
    await command('/time set 18000', /time/i);   // night: the zombie does not burn
    await command(`/tp Tester ${X} ${Y + 12} ${Z}`, /Teleported/);
    await sleep(3000);
    // a 25 x 25 platform, a wall across it at x = X (3 high) with a gap at z = Z + 8
    await command(`/fill ${X - 12} ${Y + 1} ${Z - 12} ${X + 12} ${Y + 4} ${Z + 12} air`, /filled|no blocks/i).catch(() => {});
    await command(`/fill ${X - 12} ${Y} ${Z - 12} ${X + 12} ${Y} ${Z + 12} stone`, /filled/);
    await command(`/fill ${X} ${Y + 1} ${Z - 12} ${X} ${Y + 3} ${Z + 12} stone`, /filled/);
    await command(`/fill ${X} ${Y + 1} ${Z + 8} ${X} ${Y + 2} ${Z + 8} air`, /filled/);

    runner = await connectBot(25565, 'Runner', { host, checkTimeoutInterval: 600000 });
    await command('/gamemode survival Runner', /game mode/i);
    await command(`/tp Runner ${X + 6}.5 ${Y + 1} ${Z - 4}.5`, /Teleported/);
    await sleep(2000);
    const before = /(\d+) reached/.exec(await command('/workers', /^Paths:/))[1];
    await command(`/summon zombie ${X - 6}.5 ${Y + 1} ${Z - 4}.5`, /Summoned/i);

    const zombie = () => Object.values(runner.entities).find((e) => e.name === 'zombie' &&
      Math.abs(e.position.x - X) < 14 && Math.abs(e.position.z - Z) < 14);
    let best = 1e9;
    const t0 = Date.now();
    await waitFor(() => {
      const zb = zombie();
      if (zb) best = Math.min(best, zb.position.distanceTo(runner.entity.position));
      return best < 2.5;
    }, SECONDS * 1000, 'zombie reaching the player').catch(() => {});
    const z = zombie();
    const paths = await command('/workers', /^Paths:/);
    console.log(`  ${paths}`);
    console.log(`  closest approach ${best.toFixed(1)} blocks after ${((Date.now() - t0) / 1000).toFixed(1)} s; ` +
      `zombie at ${z ? z.position.floored() : 'gone'}`);
    assert(z, 'the zombie is gone (walked off the platform?)');
    assert(z.position.y > Y - 0.5, 'the zombie fell off the platform');
    assert(best < 2.5, `the zombie did not reach the player (closest ${best.toFixed(1)} blocks)`);
    assert(Number(/(\d+) reached/.exec(paths)[1]) > Number(before), 'no path reached the target');
    console.log(`HARDWARE PATH OK (${host}): the zombie found the gap in the wall`);
  } finally {
    for (const b of [runner, op]) {
      if (!b) continue;
      b.on('error', () => {});
      b.quit();
    }
  }
})().catch((err) => { console.error(err.message || err); process.exitCode = 1; });
