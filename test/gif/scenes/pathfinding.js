'use strict';
// A zombie finds its way around a wall: the player stands right behind it, the only
// gap is at the far end, so the zombie walks there (A* on the worker threads) and back.
const X0 = 192, Y = 150, Z0 = -190, W = 22, D = 13;
const WALL_Z = Z0 + 6, GAP_X = X0 + W - 2;
const TARGET = [X0 + 5.5, Y, Z0 + 10.5], ZOMBIE = [X0 + 5.5, Y, Z0 + 2.5];

module.exports = {
  name: 'pathfinding',
  order: 4,
  title: 'a zombie walking around a wall to reach the player',
  seconds: 18,
  time: 14000,   // night: zombies burn in daylight
  async build(ctx) {
    ctx.look([X0 + W / 2, Y + 14, Z0 + D + 9], [X0 + W / 2, Y, Z0 + D / 2]);
    await ctx.loaded(X0 - 1, Z0 - 1, X0 + W + 1, Z0 + D + 1);
    await ctx.platform(X0 - 1, Y, Z0 - 1, X0 + W, Z0 + D, 6, 'grass_block');
    // the wall (3 high: a zombie cannot jump it) with one gap near the east end, and a
    // rim so nobody walks off
    await ctx.cmd(`fill ${X0} ${Y} ${WALL_Z} ${X0 + W - 1} ${Y + 2} ${WALL_Z} cobblestone`);
    await ctx.cmd(`fill ${GAP_X} ${Y} ${WALL_Z} ${GAP_X} ${Y + 2} ${WALL_Z} air`);
    await ctx.cmd(`fill ${X0 - 1} ${Y} ${Z0 - 1} ${X0 + W} ${Y} ${Z0 - 1} oak_fence`);
    await ctx.cmd(`fill ${X0 - 1} ${Y} ${Z0 + D} ${X0 + W} ${Y} ${Z0 + D} oak_fence`);
    await ctx.cmd(`fill ${X0 - 1} ${Y} ${Z0} ${X0 - 1} ${Y} ${Z0 + D - 1} oak_fence`);
    await ctx.cmd(`fill ${X0 + W} ${Y} ${Z0} ${X0 + W} ${Y} ${Z0 + D - 1} oak_fence`);
    const steve = await ctx.addBot('Steve');
    steve.physicsEnabled = false;
    ctx.steve = steve;
    await ctx.cmd(`tp Steve ${TARGET.join(' ')}`);
    await ctx.waitFor(() => ctx.block(GAP_X - 1, Y + 2, WALL_Z)?.name === 'cobblestone', 30000, 'the wall');
    await ctx.sleep(1500);
  },
  async play(ctx) {
    await ctx.cmd(`summon zombie ${ZOMBIE.join(' ')}`);
    while (!ctx.stopped) {   // keep the player alive and in place
      await ctx.sleep(2500);
      await ctx.cmd('heal Steve');
    }
  },
  async check(ctx) {
    await ctx.cmd(`summon zombie ${ZOMBIE.join(' ')}`);
    const zombie = () => Object.values(ctx.camera.entities).find((e) => e.name === 'zombie');
    await ctx.waitFor(() => zombie(), 10000, 'the zombie');
    // through the gap (not over the 1.5-block fence rim at the west end), to the player
    await ctx.waitFor(() => zombie()?.position.x > GAP_X - 2, 30000, 'the zombie at the gap');
    await ctx.waitFor(() => {
      const z = zombie();
      return z && z.position.z > WALL_Z + 1 && z.position.distanceTo(new ctx.Vec3(...TARGET)) < 3;
    }, 30000, 'the zombie at the player');
  },
};
