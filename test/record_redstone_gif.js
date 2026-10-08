'use strict';
// Records a GIF of a redstone circuit running on the server: a repeater chain that
// lights eight lamps one after another, and a dust line driving a sticky piston, built
// in the sky and switched on and off a few times.
//
//   node test/record_redstone_gif.js --host <board ip>      (Tester must be an operator)
//   SERVER_BIN=.../mcserver node test/record_redstone_gif.js --local
//   options: [--out docs/images/redstone.gif] [--seconds 16] [--fps 12] [--check]
//
// --check builds the circuit and verifies the lamps and the piston through the bot's
// world, without recording (no browser or ffmpeg needed).
// Recording: prismarine-viewer draws what the camera bot knows, Playwright's Chromium
// screencasts it, ffmpeg (on PATH, or FFMPEG=...) makes the GIF; see record_gif.js.
const { execFileSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { Vec3 } = require('vec3');
const { startServer, connectBot, sleep, waitFor } = require('./lib');

const Module = require('module');   // prismarine-viewer's unused headless renderer needs `canvas`
const realLoad = Module._load;
Module._load = function (request, ...rest) {
  if (request === 'canvas') return { createCanvas: () => { throw new Error('canvas is not available'); } };
  return realLoad.call(this, request, ...rest);
};

const args = process.argv.slice(2);
const opt = (name, def) => (args.indexOf('--' + name) >= 0 ? args[args.indexOf('--' + name) + 1] : def);
const flag = (name) => args.includes('--' + name);
const local = flag('local'), check = flag('check');
const host = local ? '127.0.0.1' : opt('host');
if (!host) { console.error('usage: node record_redstone_gif.js --host <board ip> | --local [--check]'); process.exit(2); }
const PORT = local ? 25600 + Math.floor(Math.random() * 300) : 25565;
const ROOT = path.join(__dirname, '..');
const OUT = path.resolve(ROOT, opt('out', 'docs/images/redstone.gif'));
const SECONDS = parseFloat(opt('seconds', '16'));
const FPS = parseFloat(opt('fps', '12'));
const WIDTH = parseInt(opt('width', '640')), HEIGHT = parseInt(opt('height', '360'));
const FFMPEG = process.env.FFMPEG || 'ffmpeg';
const CYCLE_MS = parseInt(opt('cycle', '4000'));   // on for half of it, off for the other half

// the circuit: rows along +x, starting at (X0, Y, Z0)
const X0 = parseInt(opt('x', '192')), Z0 = parseInt(opt('z', '-176')), Y = parseInt(opt('y', '150'));
const LAMPS = 8;
const RIPPLE_Z = Z0, PISTON_Z = Z0 + 3;
const SWITCH = [[X0, RIPPLE_Z], [X0, PISTON_Z]];   // redstone blocks placed / removed

function circuitCommands() {
  const c = [];
  const xEnd = X0 + 2 * LAMPS + 1;
  c.push(`/fill ${X0 - 2} ${Y - 1} ${Z0 - 2} ${xEnd + 2} ${Y + 2} ${Z0 + 5} air`);
  c.push(`/fill ${X0 - 1} ${Y - 1} ${Z0 - 1} ${xEnd} ${Y - 1} ${Z0 + 4} smooth_stone`);
  // ripple: repeater -> lamp -> repeater -> lamp ... (a repeater takes its input from the
  // side its `facing` names: west, so the signal runs east, 0.2 s a stage)
  for (let i = 0; i < LAMPS; i++) {
    c.push(`/setblock ${X0 + 1 + 2 * i} ${Y} ${RIPPLE_Z} repeater[facing=west,delay=2]`);
    c.push(`/setblock ${X0 + 2 + 2 * i} ${Y} ${RIPPLE_Z} redstone_lamp`);
  }
  // piston: a dust line into a sticky piston that pushes a gold block east
  c.push(`/fill ${X0 + 1} ${Y} ${PISTON_Z} ${X0 + 12} ${Y} ${PISTON_Z} redstone_wire`);
  c.push(`/setblock ${X0 + 13} ${Y} ${PISTON_Z} sticky_piston[facing=east]`);
  c.push(`/setblock ${X0 + 14} ${Y} ${PISTON_Z} gold_block`);
  return c;
}

const toNotchYaw = (yaw) => (180 / Math.PI) * (Math.PI - yaw);
const toNotchPitch = (pitch) => (180 / Math.PI) * -pitch;
function sendPose(bot, x, y, z, yaw, pitch) {
  bot.entity.position = new Vec3(x, y, z);
  bot.entity.yaw = yaw;
  bot.entity.pitch = pitch;
  bot._client.write('position_look', { x, y, z, yaw: toNotchYaw(yaw), pitch: toNotchPitch(pitch), onGround: true });
  bot.emit('move');
}

(async () => {
  let server = null, bot, browser, cycler;
  try {
    if (local) {
      server = startServer(['--port', String(PORT), '--seed', '42', '--view', '4', '--ops', 'Tester', '--no-mobs']);
      await server.ready;
    }
    bot = await connectBot(PORT, 'Tester', { host, checkTimeoutInterval: 600000 });
    bot.physicsEnabled = false;
    const command = async (text) => {
      if (server) server.command(text.slice(1));
      else bot.chat(text);
      await sleep(server ? 60 : 300);
    };
    await command('/gamemode spectator Tester');
    await command('/time set 6000');
    await command('/weather clear');
    // the camera: south of the circuit, above it, looking north-east over both rows
    const camX = X0 + LAMPS + 1, camY = Y + 4, camZ = Z0 + 9;
    await command(`/tp Tester ${camX} ${camY} ${camZ}`);
    await sleep(1500);
    const dx = (X0 + LAMPS + 1) - camX, dy = Y - (camY + 1.62), dz = (Z0 + 1.5) - camZ;
    sendPose(bot, camX + 0.5, camY, camZ + 0.5, Math.atan2(-dx, -dz), Math.atan2(dy, Math.sqrt(dx * dx + dz * dz)));
    await waitFor(() => bot.blockAt(new Vec3(X0 + 2 * LAMPS + 2, Y - 1, Z0 + 5)), 60000, 'the circuit\'s chunks');
    for (const c of circuitCommands()) await command(c);
    const at = (x, z) => bot.blockAt(new Vec3(x, Y, z));
    await waitFor(() => at(X0 + 14, PISTON_Z)?.name === 'gold_block' && at(X0 + 2 * LAMPS, RIPPLE_Z)?.name === 'redstone_lamp',
      30000, 'the circuit built');
    const power = async (on) => {
      for (const [x, z] of SWITCH) await command(`/setblock ${x} ${Y} ${z} ${on ? 'redstone_block' : 'air'}`);
    };

    if (check) {
      for (let cycle = 0; cycle < 2; cycle++) {
        await power(true);
        await waitFor(() => at(X0 + 2 * LAMPS, RIPPLE_Z)?.getProperties().lit === true, 10000, 'last lamp on');
        for (let i = 0; i < LAMPS; i++) {
          if (at(X0 + 2 + 2 * i, RIPPLE_Z)?.getProperties().lit !== true) throw new Error(`lamp ${i} is off`);
        }
        await waitFor(() => at(X0 + 15, PISTON_Z)?.name === 'gold_block', 10000, 'gold block pushed');
        await power(false);
        await waitFor(() => at(X0 + 2 * LAMPS, RIPPLE_Z)?.getProperties().lit === false, 10000, 'last lamp off');
        await waitFor(() => at(X0 + 14, PISTON_Z)?.name === 'gold_block', 10000, 'gold block pulled back');
      }
      console.log(`REDSTONE CIRCUIT OK (${host}): ${LAMPS} lamps in sequence, sticky piston out and back, twice`);
      return;
    }

    // ---- recording
    require('prismarine-viewer').mineflayer(bot, { port: PORT + 1000, firstPerson: true, viewDistance: 2 });
    const { chromium } = require('playwright');
    browser = await chromium.launch({ args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist'] });
    const context = await browser.newContext({ viewport: { width: WIDTH, height: HEIGHT } });
    const page = await context.newPage();
    await page.goto(`http://127.0.0.1:${PORT + 1000}`);
    await page.waitForSelector('canvas', { timeout: 60000 });
    await sleep(12000);   // let the browser mesh the chunks around the camera

    let on = false;
    await power(false);
    cycler = setInterval(() => { on = !on; power(on).catch(() => {}); }, CYCLE_MS / 2);
    const frameDir = fs.mkdtempSync(path.join(os.tmpdir(), 'mcredstone-'));
    const frames = [];
    const t0 = Date.now();
    const cdp = await context.newCDPSession(page);
    cdp.on('Page.screencastFrame', ({ data, metadata, sessionId }) => {
      cdp.send('Page.screencastFrameAck', { sessionId }).catch(() => {});
      const ts = Math.round(metadata.timestamp * 1000);
      if (ts < t0 || ts > t0 + SECONDS * 1000) return;
      const file = path.join(frameDir, `s${String(frames.length).padStart(5, '0')}.jpg`);
      fs.writeFileSync(file, Buffer.from(data, 'base64'));
      frames.push({ file, ts });
    });
    await cdp.send('Page.startScreencast', { format: 'jpeg', quality: 92, maxWidth: WIDTH, maxHeight: HEIGHT });
    await sleep(SECONDS * 1000 + 300);
    await cdp.send('Page.stopScreencast').catch(() => {});
    clearInterval(cycler);
    cycler = null;
    if (!frames.length) throw new Error('no frames captured');
    console.log(`${frames.length} frames (${(frames.length / SECONDS).toFixed(1)} fps rendered)`);
    const list = frames.map((f, i) => {
      const next = i + 1 < frames.length ? frames[i + 1].ts : t0 + SECONDS * 1000;
      return `file '${f.file.replace(/\\/g, '/')}'\nduration ${(Math.max(1, next - f.ts) / 1000).toFixed(3)}`;
    }).join('\n') + `\nfile '${frames[frames.length - 1].file.replace(/\\/g, '/')}'\n`;
    fs.writeFileSync(path.join(frameDir, 'list.txt'), list);
    const vf = `fps=${FPS},scale=${WIDTH}:-1:flags=lanczos,split[a][b];` +
      '[a]palettegen=max_colors=128:stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=5:diff_mode=rectangle';
    fs.mkdirSync(path.dirname(OUT), { recursive: true });
    execFileSync(FFMPEG, ['-y', '-loglevel', 'error', '-f', 'concat', '-safe', '0', '-i', path.join(frameDir, 'list.txt'),
      '-vf', vf, '-loop', '0', OUT], { stdio: 'inherit' });
    fs.rmSync(frameDir, { recursive: true, force: true });
    console.log(`wrote ${path.relative(ROOT, OUT)} (${(fs.statSync(OUT).size / 1048576).toFixed(1)} MB)`);
  } catch (e) {
    console.log('FAILED: ' + (e.stack || e.message));
    process.exitCode = 1;
  } finally {
    if (cycler) clearInterval(cycler);
    if (browser) await browser.close().catch(() => {});
    if (bot) bot.quit();
    if (server) { await sleep(300); await server.stop(); }
    setTimeout(() => process.exit(process.exitCode || 0), 500);
  }
})();
