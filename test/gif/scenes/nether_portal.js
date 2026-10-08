'use strict';
// A player lights an obsidian frame with flint and steel, the portal fills it, and the
// player walks in and leaves for the Nether.
const X = 200, Y = 151, Z = -186;   // inside of the frame: x X..X+1, y Y..Y+2, at z Z

async function light(ctx) {
  const alex = ctx.alex;
  await alex.activateBlock(alex.blockAt(new ctx.Vec3(X, Y - 1, Z)), new ctx.Vec3(0, 1, 0));
}
const portalBlocks = (ctx) => {
  let n = 0;
  for (let x = X; x <= X + 1; x++) for (let y = Y; y <= Y + 2; y++) n += ctx.block(x, y, Z)?.name === 'nether_portal';
  return n;
};

module.exports = {
  name: 'nether_portal',
  order: 6,
  title: 'lighting a nether portal and walking through it',
  seconds: 12,
  peaceful: true,
  async build(ctx) {
    ctx.look([X + 6.5, Y + 3, Z - 7.5], [X + 1, Y + 1, Z]);
    await ctx.loaded(X - 4, Z - 6, X + 5, Z + 4);
    await ctx.cmd(`fill ${X - 4} ${Y - 2} ${Z - 6} ${X + 5} ${Y + 5} ${Z + 4} air`);
    await ctx.cmd(`fill ${X - 4} ${Y - 2} ${Z - 6} ${X + 5} ${Y - 2} ${Z + 4} stone_bricks`);
    await ctx.cmd(`fill ${X - 1} ${Y - 1} ${Z} ${X + 2} ${Y + 3} ${Z} obsidian`);
    await ctx.cmd(`fill ${X} ${Y} ${Z} ${X + 1} ${Y + 2} ${Z} air`);
    const alex = await ctx.addBot('Alex', { mode: 'creative' });
    alex.physicsEnabled = false;
    ctx.alex = alex;
    await ctx.cmd('give Alex flint_and_steel 1');
    await ctx.waitFor(() => alex.inventory.items().some((i) => i.name === 'flint_and_steel'), 10000, 'flint and steel');
    await alex.equip(alex.inventory.items().find((i) => i.name === 'flint_and_steel'), 'hand');
    await ctx.cmd(`tp Alex ${X + 1} ${Y - 1} ${Z - 3}.5`);
    await ctx.waitFor(() => alex.blockAt(new ctx.Vec3(X, Y - 1, Z))?.name === 'obsidian', 30000, 'the frame');
    await ctx.sleep(1500);
  },
  async play(ctx) {
    const alex = ctx.alex;
    const at = [X + 1, Y - 1, Z - 3.5];
    ctx.look(at, [X + 1, Y, Z], alex);   // looking at the frame
    await ctx.sleep(1500);
    await light(ctx);
    await ctx.sleep(2500);
    // walk into the portal
    for (let i = 1; i <= 15 && !ctx.stopped; i++) {
      ctx.look([at[0], at[1], at[2] + (3.5 * i) / 15], [X + 1, Y, Z + 3], alex);
      await ctx.sleep(120);
    }
  },
  async check(ctx) {
    ctx.look([X + 1, Y - 1, Z - 3.5], [X + 1, Y, Z], ctx.alex);
    await ctx.sleep(500);
    await light(ctx);
    await ctx.waitFor(() => portalBlocks(ctx) === 6, 10000, 'the portal (6 blocks)');
    for (let i = 1; i <= 15; i++) {
      ctx.look([X + 1, Y - 1, Z - 3.5 + (3.5 * i) / 15], [X + 1, Y, Z + 3], ctx.alex);
      await ctx.sleep(120);
    }
    await ctx.waitFor(() => ctx.alex.game.dimension === 'the_nether', 20000, 'Alex in the Nether');
  },
};
