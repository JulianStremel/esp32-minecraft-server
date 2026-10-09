'use strict';
// Dying in the End or the Nether and respawning in the overworld: the player lands on
// loaded terrain, takes no damage afterwards, and can use a nether portal again.
//   SERVER_BIN=.../mcserver node respawn_dimensions.js     (or --host <board ip>: as Tester)
const assert = require('assert');
const { Vec3 } = require('vec3');
const { startServer, connectBot, sleep, waitFor, nextChat } = require('./lib');

(async () => {
  const hi = process.argv.indexOf('--host'), host = hi > 0 ? process.argv[hi + 1] : null;
  const port = host ? 25565 : 25640 + Math.floor(Math.random() * 100);
  const name = host ? 'Tester' : 'Diver';
  const srv = host ? null : startServer(['--port', String(port), '--seed', '42', '--ops', 'Diver', '--no-mobs', '--mem', '512'], { log: process.argv.includes('--log') });
  let bot;
  try {
    if (srv) await srv.ready;
    bot = await connectBot(port, name, host ? { host, checkTimeoutInterval: 600000 } : {});
    const command = async (c, pattern = /./) => { const r = nextChat(bot, pattern, 15000).catch(() => null); bot.chat(c); await r; await sleep(300); };
    await waitFor(() => bot.blockAt(bot.entity.position.offset(0, -1, 0)), 20000, 'spawn terrain');
    // a home bed far from the world spawn, as a player who slept somewhere
    await command(`/tp ${name} ${Math.floor(bot.entity.position.x) + 200} 120 ${Math.floor(bot.entity.position.z)}`);
    await waitFor(() => { const b = bot.blockAt(bot.entity.position.offset(0, -1, 0)); return b && b.name !== 'air'; }, 30000, 'land at the bed site');
    await sleep(1500);
    const home = bot.entity.position.floored();
    await command(`/setblock ${home.x + 1} ${home.y} ${home.z} red_bed[part=foot,facing=east]`);
    await command(`/setblock ${home.x + 2} ${home.y} ${home.z} red_bed[part=head,facing=east]`);
    await bot.activateBlock(bot.blockAt(new Vec3(home.x + 1, home.y, home.z)));
    await sleep(500);
    const spawn = new Vec3(home.x + 2, home.y + 1, home.z);
    for (const [dim, how] of [['the_end', 'bed'], ['the_end', 'void'], ['the_end', 'kill'], ['the_nether', 'kill']]) {
      await command(`/dimension ${dim}`);
      await waitFor(() => bot.game.dimension === dim, 20000, 'in ' + dim);
      await sleep(2000);
      const health = [];
      let died = false;
      bot.once('death', () => { died = true; });
      bot.on('health', () => health.push(bot.health));
      if (how === 'void') {   // off the island into the void, as a player falling off it
        await command('/gamemode survival');
        bot.physicsEnabled = true;
        await command(`/tp ${name} 300 -80 300`);
        await waitFor(() => died, 30000, 'killed by the void');
      } else if (how === 'bed') {   // sleeping outside the overworld: the bed explodes
        await command('/gamemode survival');
        bot.physicsEnabled = true;
        await sleep(1000);
        const f = bot.entity.position.floored();
        await command(`/setblock ${f.x + 1} ${f.y} ${f.z} red_bed[part=foot,facing=east]`);
        await command(`/setblock ${f.x + 2} ${f.y} ${f.z} red_bed[part=head,facing=east]`);
        await bot.activateBlock(bot.blockAt(new Vec3(f.x + 1, f.y, f.z)));
        await waitFor(() => died, 10000, 'killed by the bed');
      } else await command('/kill');
      await waitFor(() => died, 10000, 'dead');
      await sleep(500);
      bot.respawn();
      await waitFor(() => bot.game.dimension === 'overworld' && bot.health === 20, 20000, 'respawned in the overworld');
      health.length = 0;
      await waitFor(() => bot.blockAt(bot.entity.position.offset(0, -1, 0)), 20000, 'terrain under the respawned player');
      bot.physicsEnabled = true;
      await sleep(5000);
      const pos = bot.entity.position;
      const below = bot.blockAt(pos.offset(0, -1, 0));
      console.log(`after dying in ${dim} (${how}): at ${pos.x.toFixed(1)} ${pos.y.toFixed(1)} ${pos.z.toFixed(1)} on ${below && below.name}, ` +
        `health ${bot.health}, health changes since respawn: ${JSON.stringify(health)}`);
      if (!host) assert(Math.abs(pos.x - spawn.x) < 3 && Math.abs(pos.z - spawn.z) < 3, 'respawned at the home bed');
      assert.strictEqual(bot.health, 20, 'no damage after respawning');
      bot.removeAllListeners('health');
    }
    // a real portal: through it, die in the Nether, respawn, and through the same portal again
    const p = bot.entity.position.floored();
    const px = p.x + 3;
    await command(`/fill ${px} ${p.y - 1} ${p.z} ${px + 3} ${p.y + 3} ${p.z} obsidian`);
    await command(`/fill ${px + 1} ${p.y} ${p.z} ${px + 2} ${p.y + 2} ${p.z} nether_portal[axis=x]`);
    await command(`/fill ${px - 2} ${p.y} ${p.z - 2} ${px - 1} ${p.y + 3} ${p.z + 2} air`);
    await command('/gamemode creative');
    for (let round = 1; round <= 2; round++) {
      bot.entity.position = new Vec3(px + 1.5, p.y, p.z + 0.5);
      await sleep(1000);
      { const b = bot.blockAt(new Vec3(px + 1, p.y, p.z)), f = bot.blockAt(new Vec3(px, p.y, p.z));
        console.log(`round ${round}: bot at ${bot.entity.position}, portal block ${b && b.name}, frame ${f && f.name}, game mode ${bot.game.gameMode}`); }
      await waitFor(() => bot.game.dimension === 'the_nether', 20000, `through the portal (round ${round})`);
      console.log(`round ${round}: in the Nether`);
      await sleep(3000);
      let died = false;
      bot.once('death', () => { died = true; });
      await command('/kill');
      await waitFor(() => died, 10000, 'dead in the Nether');
      await sleep(500);
      bot.respawn();
      await waitFor(() => bot.game.dimension === 'overworld', 20000, 'respawned');
      await waitFor(() => bot.blockAt(bot.entity.position.offset(0, -1, 0)), 20000, 'terrain after respawn');
      await sleep(2000);
      await command('/gamemode creative');
      const b = bot.blockAt(new Vec3(px + 1, p.y, p.z));
      console.log(`  back at the portal site: ${b && b.name}`);
    }
    console.log('RESPAWN DIMENSIONS OK');
  } catch (e) {
    console.error('FAILED:', e);
    process.exitCode = 1;
  } finally {
    if (bot) bot.quit();
    if (srv) await srv.stop();
  }
})();
