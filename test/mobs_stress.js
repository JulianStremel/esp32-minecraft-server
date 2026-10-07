'use strict';
// Hostile mobs and a multi-player load test.
const assert = require('assert');
const fs = require('fs');
const { Vec3 } = require('vec3');
const { startServer, connectBot, waitFor, nextChat, sleep } = require('./lib');

const results = [];
async function step(name, fn) {
  const t0 = Date.now();
  try {
    await fn();
    results.push([name, 'ok']);
    console.log(`  ok   ${name} (${Date.now() - t0} ms)`);
  } catch (e) {
    results.push([name, 'FAIL']);
    console.log(`  FAIL ${name}: ${e.message}`);
  }
}

function cpuSeconds(pid) {
  const f = fs.readFileSync(`/proc/${pid}/stat`, 'utf8').split(') ')[1].split(' ');
  return (parseInt(f[11]) + parseInt(f[12])) / 100;  // utime + stime (clock ticks)
}

(async () => {
  // ---------------------------------------------------------------- hostile mobs (flat world)
  {
    const port = 25604;
    const srv = startServer(['--port', String(port), '--flat', '--ops', 'Victim', '--view', '3', '--no-mobs'], { log: !!process.env.LOG });
    await srv.ready;
    const bot = await connectBot(port, 'Victim');
    try {
      await waitFor(() => bot.blockAt(bot.entity.position.offset(0, -1, 0)), 10000, 'ground');
      bot.chat('/time set midnight');
      bot.chat('/difficulty hard');

      await step('zombie chases and hurts a survival player', async () => {
        bot.chat('/tp Victim 0.5 4 0.5');
        await sleep(300);
        bot.chat('/summon zombie 6 4 0');
        await waitFor(() => Object.values(bot.entities).find((e) => e.name === 'zombie'), 4000, 'zombie');
        await waitFor(() => bot.health < 20, 10000, 'zombie damage');
        console.log('       health after zombie attack:', bot.health);
        bot.chat('/kill');  // reset
        await sleep(500);
      });

      await step('skeletons shoot arrows', async () => {
        await waitFor(() => bot.health === 20, 8000, 'respawned');
        // clear remaining hostiles by toggling peaceful
        bot.chat('/difficulty peaceful');
        await sleep(300);
        bot.chat('/difficulty hard');
        bot.chat('/tp Victim 0.5 4 0.5');
        await sleep(300);
        bot.chat('/summon skeleton 9 4 0');
        const sawArrow = waitFor(() => Object.values(bot.entities).find((e) => e.name === 'arrow'), 10000, 'arrow entity');
        await sawArrow;
        let shot = false;
        const onHealth = () => { if (bot.health < 20) shot = true; };
        bot.on('health', onHealth);
        await waitFor(() => shot, 10000, 'arrow damage');
        bot.removeListener('health', onHealth);
        console.log('       health after arrows:', bot.health);
      });

      await step('creepers explode, damaging players and terrain', async () => {
        bot.chat('/difficulty peaceful');
        await sleep(300);
        bot.chat('/heal');
        bot.chat('/difficulty normal');
        bot.chat('/tp Victim 20.5 4 20.5');
        await sleep(500);
        const before = bot.health;
        let hurt = false;
        const onHealth = () => { if (bot.health < before) hurt = true; };
        bot.on('health', onHealth);
        bot.once('death', () => { hurt = true; });
        bot.chat('/summon creeper 22 4 20');
        await waitFor(() => hurt, 12000, 'explosion damage (or death)');
        bot.removeListener('health', onHealth);
        await sleep(500);
        let holes = 0;
        for (let x = 17; x <= 25; x++) for (let z = 17; z <= 24; z++) {
          const b = bot.blockAt(new Vec3(x, 3, z));
          if (b && b.name === 'air') holes++;
        }
        console.log(`       health after explosion: ${bot.health}, blasted floor blocks: ${holes}`);
        assert(holes > 0, 'the explosion destroyed blocks');
      });

      await step('hostile mobs burn in daylight', async () => {
        bot.chat('/difficulty peaceful');
        await sleep(300);
        bot.chat('/difficulty normal');
        bot.chat('/gamemode creative');
        bot.chat('/time set noon');
        bot.chat('/summon zombie 40 4 40');
        bot.chat('/tp Victim 36.5 4 36.5');
        const z = await waitFor(() => Object.values(bot.entities).find((e) => e.name === 'zombie'), 4000, 'zombie');
        await waitFor(() => z.metadata && (z.metadata[0] & 1), 6000, 'zombie on fire');
      });
    } finally {
      bot.quit();
      await sleep(300);
      await srv.stop();
    }
  }

  // ---------------------------------------------------------------- load test (normal terrain)
  {
    const port = 25605;
    const N = parseInt(process.env.BOTS || '8');
    const srv = startServer(['--port', String(port), '--seed', '99', '--view', '4', '--max-players', String(N), '--ops', 'Bot0'], { log: !!process.env.LOG });
    await srv.ready;
    const bots = [];
    try {
      await step(`${N} players join and roam for 20 s`, async () => {
        for (let i = 0; i < N; i++) {
          bots.push(await connectBot(port, 'Bot' + i));
          await sleep(150);
        }
        const cpu0 = cpuSeconds(srv.proc.pid);
        const t0 = Date.now();
        // everyone walks in a different direction (sprint-jumping across chunk borders)
        bots.forEach((b, i) => {
          b.look((i / N) * Math.PI * 2, 0, true);
          b.setControlState('forward', true);
          b.setControlState('sprint', true);
          b.setControlState('jump', true);
        });
        await sleep(20000);
        bots.forEach((b) => b.clearControlStates());
        const cpu = cpuSeconds(srv.proc.pid) - cpu0;
        const wall = (Date.now() - t0) / 1000;
        const tps = nextChat(bots[0], /TPS/);
        bots[0].chat('/tps');
        const line = await tps;
        console.log('       ' + line.slice(0, 200));
        console.log(`       server CPU: ${(cpu / wall * 100).toFixed(1)}% of one core while ${N} players roam`);
        const dist = bots.map((b) => b.entity.position.distanceTo(new Vec3(0, b.entity.position.y, 0)));
        console.log('       distance travelled (blocks):', dist.map((d) => d.toFixed(0)).join(' '));
        for (const b of bots) assert(b.entity && b._client.state === 'play', 'still connected');
        const m = /TPS ([0-9.]+)/.exec(line);
        assert(m && parseFloat(m[1]) > 18, 'TPS stays near 20');
      });
      await step('everyone receives chat from everyone', async () => {
        const waits = bots.map((b) => nextChat(b, /roll call/));
        bots[N - 1].chat('roll call');
        await Promise.all(waits);
      });
    } finally {
      bots.forEach((b) => b.quit());
      await sleep(500);
      await srv.stop();
    }
  }
  const failed = results.filter((r) => r[1] !== 'ok');
  console.log(`\n${results.length - failed.length}/${results.length} checks passed`);
  process.exit(failed.length ? 1 : 0);
})().catch((e) => { console.error('FAILED:', e); process.exit(1); });
