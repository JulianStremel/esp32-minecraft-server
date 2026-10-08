'use strict';
// The Nether and the End: travel by command and by portal blocks, terrain, building,
// entity tracking across dimensions, and the player's dimension surviving a reconnect
// (and, locally, a server restart).
//   node hardware_dimensions.js --host 192.168.1.160 [--x 7000 --z 7000]
//   SERVER_BIN=/tmp/mc-host-build/mcserver node hardware_dimensions.js --local
// Tester (an operator) travels; Watcher stays in the overworld next to where Tester left.
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { Vec3 } = require('vec3');
const { startServer, connectBot, nextChat, sleep, waitFor } = require('./lib');

const args = process.argv.slice(2);
const opt = (name, fallback) => {
  const i = args.indexOf('--' + name);
  return i < 0 ? fallback : args[i + 1];
};
const local = args.includes('--local');
let host = opt('host');
if (!host && !local) { console.error('usage: node hardware_dimensions.js --host <board ip> | --local'); process.exit(2); }
const X = Number(opt('x', local ? '40' : '7000')), Z = Number(opt('z', local ? '40' : '7000'));
const PORT = local ? 25600 + Math.floor(Math.random() * 300) : 25565;

const NETHER_BLOCKS = new Set(['netherrack', 'soul_sand', 'gravel', 'nether_quartz_ore', 'nether_gold_ore', 'magma_block',
  'glowstone', 'bedrock', 'obsidian', 'lava']);
let server, op, watcher;
let worldFile;

async function command(text, pattern, ms = 20000) {
  const r = nextChat(op, pattern, ms);
  op.chat(text);
  const line = await r;
  await sleep(650);   // vanilla chat rate limit
  return line;
}

async function join(name) {
  const b = await connectBot(PORT, name, { host: host || 'localhost', checkTimeoutInterval: 600000 });
  b.on('error', () => {});
  return b;
}

// waits until the bot is in `dim` and the chunk under it has arrived
async function arrive(bot, dim, what) {
  await waitFor(() => bot.game.dimension === dim, 20000, `${what}: dimension ${dim} (is ${bot.game.dimension})`);
  await waitFor(() => {
    const p = bot.entity && bot.entity.position;
    return p && bot.blockAt(p.offset(0, -1, 0)) !== null;
  }, 20000, `${what}: the ground under the bot`);
  await sleep(1000);
  return bot.entity.position.clone();
}

function groundName(bot) {
  const b = bot.blockAt(bot.entity.position.offset(0, -1, 0));
  return b ? b.name : '?';
}

async function startLocal() {
  worldFile = path.join(os.tmpdir(), `mc-dims-${process.pid}.img`);
  server = startServer(['--port', String(PORT), '--seed', '42', '--ops', 'Tester', '--file', worldFile, '--size', '256',
    '--no-mobs', '--format']);
  await server.ready;
}

(async () => {
  try {
    if (local) {
      await startLocal();
      host = 'localhost';
    }
    op = await join('Tester');
    op.physicsEnabled = false;
    await command('/gamemode creative', /game mode/i);
    if (op.game.dimension !== 'overworld') {   // left elsewhere by an earlier run
      await command('/dimension overworld', /Mov(ed|ing) Tester/);
      await arrive(op, 'overworld', 'start');
    }
    await command(`/tp Tester ${X}.5 120 ${Z}.5`, /Teleported/);
    await sleep(3000);
    watcher = await join('Watcher');
    await command(`/tp Watcher ${X + 3}.5 120 ${Z}.5`, /Teleported/);
    await waitFor(() => Object.values(watcher.entities).some((e) => e.username === 'Tester'), 15000, 'Watcher seeing Tester');

    // 1) by command into the Nether
    const t0 = Date.now();
    const line = await command('/dimension the_nether', /Mov(ed|ing) Tester|not available/);
    assert(/Mov(ed|ing) Tester/.test(line), line);
    let pos = await arrive(op, 'the_nether', '/dimension the_nether');
    console.log(`  /dimension the_nether: ${line}; arrived at ${pos.floored()} on ${groundName(op)} after ${Date.now() - t0} ms`);
    assert(Math.abs(pos.x - X / 8) < 12 && Math.abs(pos.z - Z / 8) < 12, `not at the overworld position / 8: ${pos}`);
    assert(NETHER_BLOCKS.has(groundName(op)), `not Nether ground: ${groundName(op)}`);
    // the terrain around: mostly Nether blocks, no grass or stone
    const seen = {};
    for (let dx = -8; dx <= 8; dx += 2)
      for (let dz = -8; dz <= 8; dz += 2)
        for (let dy = -10; dy <= 10; dy += 2) {
          const b = op.blockAt(pos.offset(dx, dy, dz));
          if (b && b.name !== 'air' && b.name !== 'cave_air') seen[b.name] = (seen[b.name] || 0) + 1;
        }
    console.log('  blocks around:', JSON.stringify(seen));
    assert(!seen.grass_block && !seen.stone && !seen.dirt, 'overworld blocks in the Nether');
    assert((seen.netherrack || 0) > 0, 'no netherrack around');
    // Watcher no longer sees Tester
    await waitFor(() => !Object.values(watcher.entities).some((e) => e.username === 'Tester'), 10000,
      'Tester disappearing for Watcher');
    // build in the Nether
    const gold = pos.floored().offset(2, 0, 0);
    await command(`/setblock ${gold.x} ${gold.y} ${gold.z} gold_block`, /Changed|placed|set/i);
    await waitFor(() => op.blockAt(gold) && op.blockAt(gold).name === 'gold_block', 5000, 'the gold block in the Nether');
    console.log(`  gold block placed at ${gold} in the Nether`);
    await command('/save-all', /Saved/i, 60000);

    // 2) the dimension survives a reconnect (and, locally, a restart)
    op.quit();
    await sleep(1500);
    if (local) {
      await server.stop();
      server = startServer(['--port', String(PORT), '--ops', 'Tester', '--file', worldFile, '--no-mobs']);
      await server.ready;
      watcher = null;
    }
    op = await join('Tester');
    op.physicsEnabled = false;
    pos = await arrive(op, 'the_nether', 'rejoin');
    await waitFor(() => op.blockAt(gold) && op.blockAt(gold).name === 'gold_block', 15000, 'the gold block after rejoining');
    console.log(`  rejoined${local ? ' after a server restart' : ''}: still in the Nether at ${pos.floored()}, the gold block is there`);

    // 3) back by command
    await command('/dimension overworld', /Mov(ed|ing) Tester/);
    pos = await arrive(op, 'overworld', '/dimension overworld');
    console.log(`  /dimension overworld: at ${pos.floored()} on ${groundName(op)}`);
    assert(Math.abs(pos.x - Math.floor(gold.x) * 8) < 24, `not at the Nether position x 8: ${pos}`);
    assert(!NETHER_BLOCKS.has(groundName(op)) || groundName(op) === 'gravel' || groundName(op) === 'bedrock',
      `Nether ground in the overworld: ${groundName(op)}`);

    // 4) a nether portal placed in creative: travel after a tick (creative)
    await sleep(15500);   // the arrival cooldown (300 ticks)
    let f = op.entity.position.floored();
    await command(`/fill ${f.x} ${f.y} ${f.z} ${f.x} ${f.y + 1} ${f.z} nether_portal`, /filled|Changed/i);
    pos = await arrive(op, 'the_nether', 'nether portal');
    console.log(`  walked into a nether portal: Nether at ${pos.floored()}`);

    // 5) an end portal: to the End's obsidian platform. The arrival is inside the linked
    // portal, which keeps the cooldown up: take it away first (its frame breaks with it)
    f = op.entity.position.floored();
    await command(`/setblock ${f.x} ${f.y} ${f.z} air`, /Changed|placed|set/i);
    await sleep(15500);
    await command(`/setblock ${f.x} ${f.y} ${f.z} end_portal`, /Changed|placed|set/i);
    pos = await arrive(op, 'the_end', 'end portal');
    console.log(`  end portal: the End at ${pos.floored()} on ${groundName(op)}`);
    assert(Math.abs(pos.x - 100.5) < 1 && Math.abs(pos.z - 0.5) < 1 && Math.abs(pos.y - 49) < 1, `not on the platform: ${pos}`);
    assert.strictEqual(groundName(op), 'obsidian');
    // end stone columns along z = 0 between x 0 and 60 (chunks 6 away stream in after the arrival)
    const islandColumns = () => {
      let n = 0;
      for (let x = 0; x <= 60; x += 4)
        for (let y = 40; y < 75; y++) {
          const b = op.blockAt(new Vec3(x, y, 0));
          if (b && b.name === 'end_stone') { n++; break; }
        }
      return n;
    };
    await waitFor(() => islandColumns() > 12, 30000, 'the End island').catch(() => {});
    const island = islandColumns();
    console.log(`  end stone columns between x 0 and 60: ${island}/16`);
    assert(island > 12, 'no End island');

    // 6) an end portal in the End: home
    await sleep(15500);
    f = op.entity.position.floored();
    await command(`/setblock ${f.x} ${f.y} ${f.z} end_portal`, /Changed|placed|set/i);
    pos = await arrive(op, 'overworld', 'end portal home');
    console.log(`  end portal in the End: overworld at ${pos.floored()}`);
    console.log(`HARDWARE DIMENSIONS OK (${host})`);
  } finally {
    for (const b of [watcher, op]) if (b) b.quit();
    if (server) await server.stop();
    if (worldFile) fs.rmSync(worldFile, { force: true });
  }
})().catch((err) => { console.error(err.message || err); process.exitCode = 1; });
