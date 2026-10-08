'use strict';
// A creeper walks up to a player, its fuse burns, and it blows a crater into the island.
const X0 = 192, Y = 150, Z0 = -190, W = 16, D = 12;
const TARGET = [X0 + 11.5, Y, Z0 + 6.5], CREEPER = [X0 + 2.5, Y, Z0 + 6.5];

module.exports = {
  name: 'creeper',
  order: 5,
  title: 'a creeper exploding next to a player',
  seconds: 12,
  time: 14000,
  async build(ctx) {
    ctx.look([X0 + W / 2, Y + 9, Z0 + D + 9], [X0 + W / 2 + 1, Y, Z0 + D / 2]);
    await ctx.loaded(X0 - 1, Z0 - 1, X0 + W + 1, Z0 + D + 1);
    await ctx.cmd(`fill ${X0} ${Y - 4} ${Z0} ${X0 + W} ${Y + 5} ${Z0 + D} air`);
    await ctx.cmd(`fill ${X0} ${Y - 4} ${Z0} ${X0 + W} ${Y - 2} ${Z0 + D} dirt`);
    await ctx.cmd(`fill ${X0} ${Y - 1} ${Z0} ${X0 + W} ${Y - 1} ${Z0 + D} grass_block`);
    const steve = await ctx.addBot('Steve');
    steve.physicsEnabled = false;
    await ctx.cmd(`tp Steve ${TARGET.join(' ')}`);
    await ctx.waitFor(() => ctx.block(X0 + W, Y - 1, Z0 + D)?.name === 'grass_block', 30000, 'the island');
    await ctx.sleep(1500);
  },
  async play(ctx) {
    await ctx.cmd(`summon creeper ${CREEPER.join(' ')}`);
  },
  async check(ctx) {
    await ctx.cmd(`summon creeper ${CREEPER.join(' ')}`);
    const creeper = () => Object.values(ctx.camera.entities).find((e) => e.name === 'creeper');
    await ctx.waitFor(() => creeper(), 10000, 'the creeper');
    await ctx.waitFor(() => !creeper(), 30000, 'the explosion');
    const holes = [];
    for (let x = X0 + 6; x <= X0 + W; x++) if (ctx.block(x, Y - 1, Z0 + 6)?.name === 'air') holes.push(x);
    if (!holes.length) throw new Error('no crater');
  },
};
