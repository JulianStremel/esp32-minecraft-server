'use strict';
// Enchanting with a real client:
//  - an enchanting table among bookshelves: three offers, the last for 30 levels; the
//    enchanting takes levels and lapis, the pickaxe gets the enchantment shown
//  - the table's block entity comes with the chunk (the client draws its book from it)
//  - an efficiency pickaxe in hand: the client's mining efficiency attribute
//  - an anvil: two sharpness III swords make sharpness IV, named, for levels
//  - a grindstone takes the enchantments off and gives experience back
//   node enchanting.js --host 192.168.1.160
//   SERVER_BIN=~/mc-host-build/mcserver node enchanting.js --local
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
if (!host && !local) { console.error('usage: node enchanting.js --host <ip> | --local'); process.exit(2); }
const PORT = local ? 25600 + Math.floor(Math.random() * 300) : 25565;
const X = Number(opt('x', '80')), Z = Number(opt('z', '80')), Y = 120;
const MINING_EFFICIENCY = 20;
let server, op, runner, worldFile;

async function command(text, pattern, ms = 15000) {
  const r = nextChat(op, pattern, ms);
  op.chat(text);
  const line = await r;
  await sleep(400);
  return line;
}
// the item's enchantments from its components: {id: level}
function enchants(item, stored = false) {
  const c = item && (item.components || []).find((k) => k.type === (stored ? 'stored_enchantments' : 'enchantments'));
  const out = {};
  if (c && c.data) for (const e of c.data.enchantments || c.data) out[e.id] = e.level;
  return out;
}
const reg = () => runner.registry.enchantmentsArray || [];
const enchName = (id) => { const e = reg().find((x) => x.id === id); return e ? e.name : String(id); };
const invSlot = (win, name) => win.slots.findIndex((s, i) => i >= win.inventoryStart && s && s.name === name);
async function click(win, slot) {
  await runner.clickWindow(slot, 0, 0);
  await sleep(200);
}
async function holding(name) {
  const it = runner.inventory.items().find((i) => i.name === name);
  assert(it, 'no ' + name);
  await runner.equip(it, 'hand');
  await sleep(300);
}

(async () => {
  try {
    if (local) {
      worldFile = path.join(os.tmpdir(), `mc-enchanting-${process.pid}.img`);
      server = startServer(['--port', String(PORT), '--seed', '42', '--ops', 'Tester', '--file', worldFile, '--size', '256',
        '--format', '--flat'], { log: args.includes('--log') });
      await server.ready;
      host = 'localhost';
    }
    op = await connectBot(PORT, 'Tester', { host, checkTimeoutInterval: 600000 });
    op.on('error', () => {});
    op.physicsEnabled = false;
    await command('/gamemode spectator', /game mode/i);
    runner = await connectBot(PORT, 'Runner', { host, checkTimeoutInterval: 600000 });
    runner.on('error', () => {});
    await command('/gamemode survival Runner', /game mode/i);
    await command(`/tp Tester ${X} ${Y + 5} ${Z - 6}`, /Teleported/);
    await sleep(2000);
    // a table at (X, Y, Z + 2) in a ring of bookshelves two high, the runner inside
    await command(`/fill ${X - 4} ${Y - 1} ${Z - 4} ${X + 4} ${Y - 1} ${Z + 6} stone`, /filled/i);
    await command(`/fill ${X - 4} ${Y} ${Z - 4} ${X + 4} ${Y + 3} ${Z + 6} air`, /filled/i);
    await command(`/fill ${X - 2} ${Y} ${Z} ${X + 2} ${Y + 1} ${Z + 4} bookshelf`, /filled/i);
    await command(`/fill ${X - 1} ${Y} ${Z + 1} ${X + 1} ${Y + 1} ${Z + 3} air`, /filled/i);
    await command(`/setblock ${X} ${Y} ${Z + 2} enchanting_table`, /Changed|set/i);
    await command(`/tp Runner ${X + 1}.5 ${Y} ${Z + 1}.5`, /Teleported/);
    await waitFor(() => runner.entity && Math.abs(runner.entity.position.y - Y) < 0.1, 10000, 'in the ring');
    await command('/clear Runner', /Cleared|Removed|clear/i);
    await command('/xp add Runner 40 levels', /Gave/);
    await command('/give Runner diamond_pickaxe 1', /Gave/);
    await command('/give Runner lapis_lazuli 3', /Gave/);
    await sleep(800);

    // 1) the enchanting table
    const tableBlock = runner.blockAt(new Vec3(X, Y, Z + 2));
    assert(tableBlock && tableBlock.name === 'enchanting_table', 'no table');
    const table = await runner.openEnchantmentTable(tableBlock);
    await table.putTargetItem(table.items().find((i) => i.name === 'diamond_pickaxe'));
    await table.putLapis(table.items().find((i) => i.name === 'lapis_lazuli'));
    await waitFor(() => table.enchantments[2].level > 0 && table.enchantments[2].expected.enchant >= 0, 10000, 'offers')
      .catch(() => {});
    const offers = table.enchantments.map((e) => `${e.level} (${enchName(e.expected.enchant)} ${e.expected.level})`);
    console.log(`  offers: ${offers.join(', ')}`);
    assert.strictEqual(table.enchantments[2].level, 30, 'the last offer is not 30 levels');
    const shown = table.enchantments[2].expected.enchant;
    const before = runner.experience.level;
    const item = await table.enchant(2);
    const got = enchants(item);
    await sleep(500);
    console.log(`  enchanted for 30: ${Object.entries(got).map(([id, l]) => `${enchName(Number(id))} ${l}`).join(', ')}; ` +
      `levels ${before} -> ${runner.experience.level}, lapis ${table.slots[1] ? table.slots[1].count : 0}`);
    assert(got[shown] > 0, 'the enchantment shown was not given');
    assert.strictEqual(runner.experience.level, before - 3, 'not 3 levels');
    assert(!table.slots[1], 'the lapis is still there');
    await table.takeTargetItem();
    table.close();
    await sleep(500);

    // 2) the table's block entity in the chunk packet: away and back
    const seen = [];
    runner._client.on('map_chunk', (p) => {
      if (p.x === X >> 4 && p.z === (Z + 2) >> 4) for (const b of p.blockEntities || []) seen.push(b);
    });
    await command('/gamemode creative Runner', /game mode/i);   // (it falls out there: no fall damage)
    await command(`/tp Runner ${X + 1}.5 ${Y} ${Z + 1 + 600}.5`, /Teleported/);
    await sleep(3000);
    await command(`/tp Runner ${X + 1}.5 ${Y} ${Z + 1}.5`, /Teleported/);
    await command('/gamemode survival Runner', /game mode/i);
    await waitFor(() => seen.length > 0, 15000, 'the chunk again').catch(() => {});
    const tables = seen.filter((b) => b.y === Y && b.x === (X & 15) && b.z === ((Z + 2) & 15));
    console.log(`  the chunk again: ${seen.length} block entities, the table's type ${tables.length ? tables[0].type : 'missing'}`);
    assert(tables.length, 'no block entity for the table');

    // 3) efficiency in hand: the mining efficiency attribute (level squared + 1)
    let efficiency = null;
    runner._client.on('entity_update_attributes', (p) => {
      if (p.entityId !== runner.entity.id) return;
      // (minecraft-data names 1.21.8's attribute 20, mining_efficiency, after an older registry)
      for (const a of p.properties) if (a.key === MINING_EFFICIENCY || a.key === 'generic.movement_speed') efficiency = a;
    });
    await command('/give Runner iron_pickaxe 1', /Gave/);
    await holding('iron_pickaxe');
    await command('/enchant Runner efficiency 3', /Applied/);
    await waitFor(() => efficiency && efficiency.modifiers.length, 5000, 'attribute').catch(() => {});
    const effMod = efficiency && efficiency.modifiers[0];
    console.log(`  efficiency III in hand: mining efficiency +${effMod ? effMod.amount : 'none'}`);
    assert(effMod && effMod.amount === 10, 'no mining efficiency modifier');

    // 4) the anvil: two sharpness III swords, named
    await command(`/fill ${X - 2} ${Y} ${Z} ${X + 2} ${Y + 1} ${Z + 4} air`, /filled/i);
    await command(`/setblock ${X} ${Y} ${Z + 3} anvil`, /Changed|set/i);
    await command(`/setblock ${X + 2} ${Y} ${Z + 3} grindstone`, /Changed|set/i);
    await command('/clear Runner', /Cleared|Removed|clear/i);
    await command('/give Runner diamond_sword 2', /Gave/);   // (one in each of the first two hotbar slots)
    for (let i = 0; i < 2; i++) {
      runner.setQuickBarSlot(i);
      await sleep(300);
      await command('/enchant Runner sharpness 3', /Applied/);
    }
    const levelsBefore = runner.experience.level;
    let cost = -1;
    const anvil = await runner.openBlock(runner.blockAt(new Vec3(X, Y, Z + 3)));
    runner._client.on('craft_progress_bar', (p) => { if (p.windowId === anvil.id && p.property === 0) cost = p.value; });
    await click(anvil, invSlot(anvil, 'diamond_sword'));
    await click(anvil, 0);
    await click(anvil, invSlot(anvil, 'diamond_sword'));
    await click(anvil, 1);
    runner._client.write('name_item', { name: 'Edge' });
    // sharpness IV for 4 (the book's... the sword's anvil cost 1 a level), the name 1 more
    await waitFor(() => anvil.slots[2] && cost === 5, 5000, 'the anvil result').catch(() => {});
    const result = anvil.slots[2];
    console.log(`  anvil: ${result ? result.name : 'nothing'}, sharpness ${result && enchants(result)[runner.registry.enchantmentsByName.sharpness.id]}, ` +
      `cost ${cost}`);
    assert(result && enchants(result)[runner.registry.enchantmentsByName.sharpness.id] === 4, 'not sharpness IV');
    const paid = cost;
    await click(anvil, 2);
    await click(anvil, anvil.inventoryStart + 9);
    await waitFor(() => runner.experience.level === levelsBefore - paid, 5000, 'levels').catch(() => {});
    const named = anvil.slots[anvil.inventoryStart + 9];
    const customName = named && (named.components || []).find((k) => k.type === 'custom_name');
    console.log(`  taken: levels ${levelsBefore} -> ${runner.experience.level}, name ${customName ? JSON.stringify(customName.data) : 'none'}`);
    assert.strictEqual(runner.experience.level, levelsBefore - paid, 'the levels were not taken');
    assert(!anvil.slots[0] && !anvil.slots[1], 'the swords are still in the anvil');
    assert(customName, 'no custom name');
    anvil.close();
    await sleep(500);

    // 5) the grindstone: the enchantments off, experience back
    const xpBefore = runner.experience.points;
    const grind = await runner.openBlock(runner.blockAt(new Vec3(X + 2, Y, Z + 3)));
    await click(grind, invSlot(grind, 'diamond_sword'));
    await click(grind, 0);
    await waitFor(() => grind.slots[2], 5000, 'the grindstone result').catch(() => {});
    const plain = grind.slots[2];
    assert(plain && !Object.keys(enchants(plain)).length, 'the enchantments are still on');
    await click(grind, 2);
    await click(grind, grind.inventoryStart + 9);
    await waitFor(() => runner.experience.points > xpBefore, 5000, 'experience').catch(() => {});
    console.log(`  grindstone: ${plain.name} without enchantments, experience ${xpBefore} -> ${runner.experience.points}`);
    assert(runner.experience.points > xpBefore, 'no experience back');
    grind.close();
    console.log(`ENCHANTING OK (${host})`);
  } finally {
    for (const b of [runner, op]) if (b) b.quit();
    if (server) await server.stop();
    if (worldFile) fs.rmSync(worldFile, { force: true });
  }
})().catch((err) => { console.error(err.message || err); process.exitCode = 1; });
