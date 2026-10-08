'use strict';
// Client-visible redstone state changes. Tick-exact cases live in host unit tests.
// node test/redstone.js [--emulator esp32s3-8|esp32p4-8] (uses an existing firmware build)
const assert = require('assert');
const { Vec3 } = require('vec3');
const { startServer, connectBot, waitFor, nextChat, sleep } = require('./lib');
const { startEmulator } = require('./emulator');
const args = process.argv.slice(2);
const emu = args.indexOf('--emulator');

(async () => {
  const port = emu >= 0 ? 25583 : 25619;
  const server = emu >= 0
    ? startEmulator({ board: args[emu + 1], port, build: false })
    : startServer(['--port', String(port), '--flat', '--seed', '1', '--view', '3', '--no-mobs']);
  let bot;
  try {
    await server.ready;
    bot = await connectBot(port, emu >= 0 ? 'Tester' : 'CircuitTester');
    bot.physicsEnabled = false;
    const x0 = Math.floor(bot.entity.position.x / 16) * 16 + 8;
    const y = Math.floor(bot.entity.position.y) + 5, z = Math.floor(bot.entity.position.z) + 2;
    const command = async (text) => {
      if (emu < 0) server.command(text);
      else { bot.chat(text); await sleep(650); }
    };
    await waitFor(() => bot.blockAt(new Vec3(x0 + 15, y, z)), 30000, 'circuit chunks');
    const block = (x) => bot.blockAt(new Vec3(x0 + x, y, z));
    const props = (x) => block(x)?.getProperties();
    await command(`/fill ${x0 - 1} ${y} ${z} ${x0 + 15} ${y + 1} ${z} air`);
    await command(`/fill ${x0 - 1} ${y - 1} ${z} ${x0 + 15} ${y - 1} ${z} stone`);
    await command(`/fill ${x0} ${y} ${z} ${x0 + 14} ${y} ${z} redstone_wire`);
    await command(`/setblock ${x0 + 15} ${y} ${z} redstone_lamp`);
    await waitFor(() => block(15)?.name === 'redstone_lamp', 30000, 'lamp placement');
    assert.strictEqual(props(15).lit, false);
    for (let cycle = 0; cycle < 3; ++cycle) {
      await command(`/setblock ${x0 - 1} ${y} ${z} redstone_block`);
      await waitFor(() => props(15)?.lit === true, 30000, 'powered lamp packet');
      for (let x = 0; x < 15; ++x) assert.strictEqual(props(x).power, 15 - x, `wire at ${x}`);
      await command(`/setblock ${x0 - 1} ${y} ${z} air`);
      await waitFor(() => props(15)?.lit === false, 30000, 'unpowered lamp packet');
      for (let x = 0; x < 15; ++x) assert.strictEqual(props(x).power, 0, `unpowered wire at ${x}`);
    }
    const events = [];
    bot._client.on('block_action', packet => events.push(packet));
    const pz = z + 3;
    const pistonBlock = x => bot.blockAt(new Vec3(x0 + x, y, pz));
    await command(`/fill ${x0 - 1} ${y} ${pz} ${x0 + 3} ${y + 1} ${pz} air`);
    await command(`/setblock ${x0} ${y} ${pz} sticky_piston[facing=east]`);
    await command(`/setblock ${x0 + 1} ${y} ${pz} stone`);
    await command(`/setblock ${x0 - 1} ${y} ${pz} redstone_block`);
    await waitFor(() => pistonBlock(2)?.name === 'stone' && pistonBlock(1)?.name === 'piston_head', 30000, 'piston extension packets');
    assert(events.some(e => e.blockId === bot.registry.blocksByName.sticky_piston.id && e.byte1 === 0 && e.byte2 === 5), 'extension block event');
    await command(`/setblock ${x0 - 1} ${y} ${pz} air`);
    await waitFor(() => pistonBlock(1)?.name === 'stone' && pistonBlock(2)?.name === 'air' && pistonBlock(0)?.name === 'sticky_piston', 30000, 'sticky piston retraction packets');
    assert(events.some(e => e.blockId === bot.registry.blocksByName.sticky_piston.id && e.byte1 === 1 && e.byte2 === 5), 'retraction block event');
    await command(`/setblock ${x0} ${y} ${pz} note_block[note=12]`);
    await command(`/setblock ${x0 - 1} ${y} ${pz} redstone_block`);
    await waitFor(() => events.some(e => e.blockId === bot.registry.blocksByName.note_block.id && e.byte1 === 0 && e.byte2 === 0), 30000, 'note block event');
    // Exercise the actual windows and slot packets, including metadata, rather
    // than filling tile entities through commands or a test-only server API.
    await command(`/gamemode creative ${bot.username}`);
    await waitFor(() => bot.game.gameMode === 'creative', 30000, 'creative inventory setup');
    const Item = require('prismarine-item')(bot.registry);
    const az = z + 8;
    const at = x => bot.blockAt(new Vec3(x0 + x, y, az));
    const near = async x => {
      await command(`/tp ${bot.username} ${x0 + x + 0.5} ${y} ${az + 2.5}`);
      await waitFor(() => bot.entity.position.distanceTo(new Vec3(x0 + x + 0.5, y, az + 2.5)) < 1, 30000, 'container reach');
    };
    await command(`/fill ${x0} ${y} ${az} ${x0 + 12} ${y + 1} ${az} air`);
    await command(`/setblock ${x0} ${y} ${az} hopper[facing=east]`);
    await command(`/setblock ${x0 + 1} ${y} ${az} chest`);
    await near(0);
    await waitFor(() => at(0)?.name === 'hopper' && at(1)?.name === 'chest', 30000, 'hopper placement');
    const stoneId = bot.registry.itemsByName.stone.id;
    await bot.creative.setInventorySlot(36, new Item(stoneId, 8));
    const hopper = await bot.openContainer(at(0));
    assert.strictEqual(hopper.slots.length, 41, 'hopper has five container slots');
    await hopper.deposit(stoneId, null, 3); hopper.close();
    await sleep(emu < 0 ? 1600 : 3000);
    const hopperChest = await bot.openContainer(at(1));
    await waitFor(() => hopperChest.containerItems().filter(i => i.name === 'stone').reduce((n, i) => n + i.count, 0) === 3, 60000, 'all three hopper transfers');
    hopperChest.close();
    await command(`/setblock ${x0 + 4} ${y} ${az} dropper[facing=east]`);
    await command(`/setblock ${x0 + 5} ${y} ${az} chest`);
    await near(4);
    await waitFor(() => at(4)?.name === 'dropper' && at(5)?.name === 'chest', 30000, 'dropper placement');
    const dropper = await bot.openContainer(at(4));
    assert.strictEqual(dropper.slots.length, 45, 'dropper has nine container slots');
    await dropper.deposit(stoneId, null, 2); dropper.close();
    await command(`/setblock ${x0 + 4} ${y + 1} ${az} redstone_block`);
    await sleep(emu < 0 ? 700 : 1800);
    const dropperChest = await bot.openContainer(at(5));
    await waitFor(() => dropperChest.containerItems().filter(i => i.name === 'stone').reduce((n, i) => n + i.count, 0) === 1, 30000, 'single dropper transfer');
    dropperChest.close();
    await command(`/setblock ${x0 + 8} ${y} ${az} dispenser`);
    await near(8);
    await waitFor(() => at(8)?.name === 'dispenser', 30000, 'dispenser placement');
    const dispenser = await bot.openContainer(at(8));
    assert.strictEqual(dispenser.slots.length, 45, 'dispenser has nine container slots');
    dispenser.close();
    await command(`/setblock ${x0 + 11} ${y} ${az} lectern`);
    await near(11);
    await waitFor(() => at(11)?.name === 'lectern', 30000, 'lectern placement');
    const bookTag = { type: 'compound', name: '', value: {
      author: { type: 'string', value: 'CircuitTester' },
      title: { type: 'string', value: 'Redstone test' },
      resolved: { type: 'byte', value: 1 },
      pages: { type: 'list', value: { type: 'string', value: ['{"text":"one"}', '{"text":"two"}', '{"text":"three"}'] } }
    } };
    await bot.creative.setInventorySlot(36, new Item(bot.registry.itemsByName.written_book.id, 1, 0, bookTag));
    bot.setQuickBarSlot(0);
    const properties = [], windows = [];
    bot._client.on('craft_progress_bar', packet => properties.push(packet));
    bot._client.on('window_items', packet => windows.push(packet));
    await bot.activateBlock(at(11));
    await waitFor(() => at(11)?.getProperties().has_book, 30000, 'lectern book insertion');
    await bot.activateBlock(at(11));
    await waitFor(() => bot.currentWindow?.type === 'minecraft:lectern', 30000, 'lectern window');
    const lecternId = bot.currentWindow.id;
    await waitFor(() => windows.some(w => w.windowId === lecternId && w.items.length === 1), 30000, 'lectern single book slot');
    assert.deepStrictEqual(bot.currentWindow.slots[0].nbt.value.pages.value.value, bookTag.value.pages.value.value);
    bot._client.write('enchant_item', { windowId: lecternId, enchantment: 2 });
    await waitFor(() => properties.some(p => p.windowId === lecternId && p.property === 0 && p.value === 1), 30000, 'lectern page turn');
    bot._client.write('enchant_item', { windowId: lecternId, enchantment: 3 });
    await waitFor(() => !at(11)?.getProperties().has_book, 30000, 'lectern take book');
    await waitFor(() => !bot.currentWindow, 30000, 'empty lectern closes');
    if (emu < 0) {
      await command('/lag');
      await waitFor(() => /Redstone: \d+ updates, queue peak \d+, failures 0/.test(server.output()), 5000, 'redstone diagnostics');
      assert(!/Redstone stopped/.test(server.output()));
    } else {
      const diagnostics = nextChat(bot, /Redstone:/, 30000);
      await command('/lag');
      const line = await diagnostics;
      assert(/failures 0; scheduled ticks \d+\/\d+, refused 0/.test(line), line);
      console.log(line);
      const metrics = nextChat(bot, /TPS/, 30000);
      await command('/tps'); console.log(await metrics);
      assert(!server.fault(), 'firmware must not panic');
      console.log('serial log: ' + server.logPath);
    }
    console.log(`REDSTONE OK (${emu < 0 ? 'native' : args[emu + 1]}): cross-chunk wire, lamp transitions, piston/note events, hopper/dropper transfers, dispenser and lectern windows, book metadata and diagnostics`);
  } finally {
    if (emu >= 0) console.log('serial log: ' + server.logPath);
    if (bot) bot.quit();
    await server.stop();
  }
})().catch((err) => { console.error(err); process.exitCode = 1; });
