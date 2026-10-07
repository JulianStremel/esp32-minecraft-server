'use strict';
// Gameplay end-to-end tests with real 1.16.5 protocol clients (mineflayer).
const assert = require('assert');
const { Vec3 } = require('vec3');
const { startServer, connectBot, waitFor, nextChat, sleep } = require('./lib');

const results = [];
async function step(name, fn) {
  const t0 = Date.now();
  try {
    await fn();
    results.push([name, 'ok', Date.now() - t0]);
    console.log(`  ok   ${name} (${Date.now() - t0} ms)`);
  } catch (e) {
    results.push([name, 'FAIL', Date.now() - t0, e.message]);
    console.log(`  FAIL ${name}: ${e.message}`);
  }
}

const itemCount = (bot, name) => bot.inventory.items().filter((i) => i.name === name).reduce((a, i) => a + i.count, 0);

(async () => {
  const port = 25603;
  const srv = startServer(['--port', String(port), '--flat', '--seed', '5', '--ops', 'Alice,Bob', '--view', '3', '--peaceful'],
    { log: !!process.env.LOG });
  await srv.ready;
  let alice, bob;
  try {
    alice = await connectBot(port, 'Alice');
    bob = await connectBot(port, 'Bob');
    await waitFor(() => alice.blockAt(alice.entity.position.offset(0, -1, 0)), 10000, 'alice ground');
    await waitFor(() => bob.blockAt(bob.entity.position.offset(0, -1, 0)), 10000, 'bob ground');

    await step('players see each other in the tab list and as entities', async () => {
      await waitFor(() => alice.players.Bob && bob.players.Alice, 5000, 'tab list');
      await waitFor(() => alice.players.Bob.entity && bob.players.Alice.entity, 5000, 'player entities');
    });

    await step('movement is synchronised to other players', async () => {
      bob.chat('/tp Bob 5.5 4 9.5');
      await waitFor(() => { const e = alice.players.Bob.entity; return e && e.position.distanceTo(new Vec3(5.5, 4, 9.5)) < 0.6; }, 5000, 'bob position on alice');
      bob.chat('/tp Bob 2.5 4 2.5');
      await waitFor(() => { const e = alice.players.Bob.entity; return e && e.position.distanceTo(new Vec3(2.5, 4, 2.5)) < 0.6; }, 5000, 'bob moved');
    });

    await step('survival digging with server-side timing drops an item that is picked up', async () => {
      alice.chat('/gamemode survival');
      await sleep(300);
      alice.chat('/tp Alice 0.5 4 0.5');
      await sleep(500);
      const target = alice.blockAt(new Vec3(1, 3, 1));
      assert.strictEqual(target.name, 'grass_block');
      const before = itemCount(alice, 'dirt');
      await alice.dig(target);
      await waitFor(() => alice.blockAt(new Vec3(1, 3, 1)).name === 'air', 3000, 'block removed');
      // the drop fell into the hole (outside the vanilla pickup box): step into it
      alice.chat('/tp Alice 1.5 4 1.5');
      await waitFor(() => itemCount(alice, 'dirt') === before + 1, 5000, 'dirt picked up');
      alice.chat('/tp Alice 0.5 4 0.5');
      await sleep(300);
    });

    await step('placing a block in survival consumes it', async () => {
      const dirt = alice.inventory.items().find((i) => i.name === 'dirt');
      await alice.equip(dirt, 'hand');
      const ref = alice.blockAt(new Vec3(2, 3, -1));
      await alice.placeBlock(ref, new Vec3(0, 1, 0));
      await waitFor(() => alice.blockAt(new Vec3(2, 4, -1)).name === 'dirt', 3000, 'placed dirt');
      await waitFor(() => itemCount(alice, 'dirt') === 0, 3000, 'dirt consumed');
    });

    await step('2x2 crafting: logs -> planks -> crafting table', async () => {
      alice.chat('/give Alice oak_log 2');
      await waitFor(() => itemCount(alice, 'oak_log') === 2, 3000, 'logs');
      const planksRecipe = alice.recipesFor(alice.registry.itemsByName.oak_planks.id, null, 1, null)[0];
      assert(planksRecipe, 'planks recipe known');
      await alice.craft(planksRecipe, 2, null);
      await waitFor(() => itemCount(alice, 'oak_planks') === 8, 3000, '8 planks');
      const tableRecipe = alice.recipesFor(alice.registry.itemsByName.crafting_table.id, null, 1, null)[0];
      await alice.craft(tableRecipe, 1, null);
      await waitFor(() => itemCount(alice, 'crafting_table') === 1, 3000, 'crafting table');
      assert.strictEqual(itemCount(alice, 'oak_planks'), 4);
    });

    await step('3x3 crafting at a crafting table: sticks + pickaxe', async () => {
      const table = alice.inventory.items().find((i) => i.name === 'crafting_table');
      await alice.equip(table, 'hand');
      await alice.placeBlock(alice.blockAt(new Vec3(-1, 3, 1)), new Vec3(0, 1, 0));
      const tableBlock = await waitFor(() => { const b = alice.blockAt(new Vec3(-1, 4, 1)); return b.name === 'crafting_table' && b; }, 3000, 'table placed');
      const sticks = alice.recipesFor(alice.registry.itemsByName.stick.id, null, 1, tableBlock)[0];
      await alice.craft(sticks, 1, tableBlock);
      await waitFor(() => itemCount(alice, 'stick') === 4, 3000, 'sticks');
      alice.chat('/give Alice oak_planks 1');
      await waitFor(() => itemCount(alice, 'oak_planks') === 3, 3000, 'enough planks');
      const pick = alice.recipesFor(alice.registry.itemsByName.wooden_pickaxe.id, null, 1, tableBlock)[0];
      assert(pick, 'pickaxe recipe available');
      await alice.craft(pick, 1, tableBlock);
      await waitFor(() => itemCount(alice, 'wooden_pickaxe') === 1, 3000, 'wooden pickaxe');
    });

    await step('furnace smelts iron ore with coal', async () => {
      alice.chat('/give Alice furnace 1');
      alice.chat('/give Alice iron_ore 2');
      alice.chat('/give Alice coal 1');
      await waitFor(() => itemCount(alice, 'furnace') === 1 && itemCount(alice, 'coal') === 1, 3000, 'items');
      await alice.equip(alice.inventory.items().find((i) => i.name === 'furnace'), 'hand');
      await alice.placeBlock(alice.blockAt(new Vec3(0, 3, -2)), new Vec3(0, 1, 0));
      const fb = await waitFor(() => { const b = alice.blockAt(new Vec3(0, 4, -2)); return b.name === 'furnace' && b; }, 3000, 'furnace placed');
      const furnace = await alice.openFurnace(fb);
      await furnace.putFuel(alice.registry.itemsByName.coal.id, null, 1);
      await furnace.putInput(alice.registry.itemsByName.iron_ore.id, null, 2);
      await waitFor(() => furnace.outputItem() && furnace.outputItem().count >= 1, 15000, 'iron ingot');
      await furnace.takeOutput();
      furnace.close();
      await waitFor(() => itemCount(alice, 'iron_ingot') >= 1, 3000, 'ingot in inventory');
    });

    await step('fall damage in survival', async () => {
      alice.chat('/heal');
      await sleep(300);
      assert.strictEqual(alice.health, 20);
      alice.chat('/tp Alice 8.5 14 8.5');
      await waitFor(() => alice.health < 20, 6000, 'fall damage');
      console.log('       health after a 10 block fall:', alice.health);
      assert(alice.health >= 12 && alice.health <= 14, 'about 7 damage');
    });

    await step('death and respawn', async () => {
      const died = new Promise((r) => alice.once('death', r));
      alice.chat('/kill');
      await died;
      await waitFor(() => alice.health === 20, 8000, 'respawned with full health');
      await waitFor(() => alice.blockAt(alice.entity.position.offset(0, -1, 0)), 8000, 'chunks after respawn');
    });

    await step('items dropped by one player are picked up by another', async () => {
      bob.chat('/gamemode survival');
      bob.chat('/give Bob diamond 3');
      await waitFor(() => itemCount(bob, 'diamond') === 3, 3000, 'bob diamonds');
      alice.chat('/tp Alice 2.5 4 4.5');
      bob.chat('/tp Bob 2.5 4 2.5');
      await sleep(800);
      await bob.lookAt(new Vec3(2.5, 4.5, 6));
      await bob.toss(bob.registry.itemsByName.diamond.id, null, 3);
      await waitFor(() => itemCount(alice, 'diamond') === 3, 8000, 'alice picked them up');
    });

    await step('mobs: summon, attack, kill, loot', async () => {
      alice.chat('/difficulty normal');
      alice.chat('/give Alice diamond_sword 1');
      await waitFor(() => itemCount(alice, 'diamond_sword') === 1, 3000, 'sword');
      await alice.equip(alice.inventory.items().find((i) => i.name === 'diamond_sword'), 'hand');
      alice.chat('/summon cow ~2 ~ ~');
      const cow = await waitFor(() => Object.values(alice.entities).find((e) => e.name === 'cow'), 5000, 'cow entity');
      for (let i = 0; i < 8 && alice.entities[cow.id]; i++) {
        if (cow.position.distanceTo(alice.entity.position) > 3) {
          alice.chat(`/tp Alice ${cow.position.x.toFixed(1)} ${cow.position.y} ${(cow.position.z + 1.5).toFixed(1)}`);
          await sleep(400);
        }
        await alice.lookAt(cow.position.offset(0, 0.8, 0));
        alice.attack(cow);
        await sleep(700);
      }
      await waitFor(() => !alice.entities[cow.id], 5000, 'cow died');
      // walk over to the loot
      const drop = await waitFor(() => Object.values(alice.entities).find((e) => e.name === 'item' && e.position.distanceTo(alice.entity.position) < 12), 5000, 'loot entity');
      await sleep(800);
      alice.chat(`/tp Alice ${drop.position.x.toFixed(2)} ${Math.floor(drop.position.y)} ${drop.position.z.toFixed(2)}`);
      await waitFor(() => itemCount(alice, 'beef') >= 1, 5000, 'beef collected');
      alice.chat('/difficulty peaceful');
    });

    await step('water flows from a bucket', async () => {
      alice.chat('/setblock 6 4 6 water');
      await waitFor(() => { const b = alice.blockAt(new Vec3(7, 4, 6)); return b && b.name === 'water'; }, 5000, 'water spread');
      const far = await waitFor(() => { const b = alice.blockAt(new Vec3(9, 4, 6)); return b && b.name === 'water' && b; }, 5000, 'water spread 3 blocks');
      console.log('       flowing water level 3 blocks away:', far.getProperties().level);
      alice.chat('/setblock 6 4 6 air');
      await waitFor(() => { const b = alice.blockAt(new Vec3(8, 4, 6)); return b && b.name === 'air'; }, 8000, 'water drained');
    });

    await step('sand falls', async () => {
      alice.chat('/setblock -3 9 -3 sand');
      await waitFor(() => { const b = alice.blockAt(new Vec3(-3, 4, -3)); return b && b.name === 'sand'; }, 6000, 'sand landed');
      assert.strictEqual(alice.blockAt(new Vec3(-3, 9, -3)).name, 'air');
    });

    await step('doors are two blocks high and open on use', async () => {
      alice.chat('/gamemode creative');
      alice.chat('/give Alice oak_door 1');
      await waitFor(() => itemCount(alice, 'oak_door') === 1, 3000, 'door');
      await alice.equip(alice.inventory.items().find((i) => i.name === 'oak_door'), 'hand');
      alice.chat('/tp Alice 4.5 4 0.5');
      await sleep(500);
      await alice.placeBlock(alice.blockAt(new Vec3(4, 3, 3)), new Vec3(0, 1, 0));
      const lower = await waitFor(() => { const b = alice.blockAt(new Vec3(4, 4, 3)); return b.name === 'oak_door' && b; }, 3000, 'door lower');
      assert.strictEqual(alice.blockAt(new Vec3(4, 5, 3)).name, 'oak_door');
      assert.strictEqual(lower.getProperties().open, false);
      await alice.activateBlock(lower);
      await waitFor(() => alice.blockAt(new Vec3(4, 4, 3)).getProperties().open === true, 3000, 'door opened');
      assert.strictEqual(alice.blockAt(new Vec3(4, 5, 3)).getProperties().open, true);
    });

    await step('stairs and fences get correct shapes', async () => {
      alice.chat('/give Alice oak_fence 3');
      await waitFor(() => itemCount(alice, 'oak_fence') === 3, 3000, 'fences');
      await alice.equip(alice.inventory.items().find((i) => i.name === 'oak_fence'), 'hand');
      alice.chat('/tp Alice -5.5 4 1.5');
      await sleep(500);
      for (const x of [-6, -5, -4]) await alice.placeBlock(alice.blockAt(new Vec3(x, 3, 4)), new Vec3(0, 1, 0));
      await waitFor(() => alice.blockAt(new Vec3(-4, 4, 4)).name === 'oak_fence', 3000, 'fences placed');
      const mid = alice.blockAt(new Vec3(-5, 4, 4)).getProperties();
      assert.strictEqual(mid.east, true);
      assert.strictEqual(mid.west, true);
      assert.strictEqual(mid.north, false);
    });

    await step('tab completion for commands', async () => {
      const r = await alice.tabComplete('/gam');
      assert(r.some((m) => (m.match || m) === 'gamemode'), JSON.stringify(r));
      const r2 = await alice.tabComplete('/give Alice diamond_s');
      assert(r2.some((m) => (m.match || m).startsWith('diamond_s')), JSON.stringify(r2).slice(0, 200));
    });
  } finally {
    if (alice) alice.quit();
    if (bob) bob.quit();
    await sleep(300);
    await srv.stop();
  }
  const failed = results.filter((r) => r[1] !== 'ok');
  console.log(`\n${results.length - failed.length}/${results.length} gameplay checks passed`);
  if (failed.length) process.exit(1);
})().catch((e) => { console.error('FAILED:', e); process.exit(1); });
