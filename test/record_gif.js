'use strict';
// Records a short GIF of players exploring the server: a group teleports into
// unexplored land (the terrain appears around them as the server generates it) and
// walks into fresh terrain while a camera player follows them.
//
// mineflayer itself renders nothing; prismarine-viewer draws what the camera bot knows
// (blocks + entities, three.js) in a web page, Playwright's Chromium (headless, software
// WebGL) screenshots it, and ffmpeg turns the frames into a GIF.
//
//   node test/record_gif.js [--server qemu|host] [--seconds 18] [--fps 10]
//                           [--out docs/images/exploring.gif] [--no-build] [--keep-frames]
//                           [--capture screencast|screenshots] [--hold 5] [--lead 300] [--debug]
//
// --server qemu (default): the real firmware on the emulated ESP32-S3 (tools/qemu/run.sh
//   --mttcg), so the speed at which terrain appears is the emulated device's. Chromium's
//   software rendering competes with QEMU for the host's cores, so the device looks
//   slower than in the load test. --server host: the PC build (needs make -C host server).
// The clip starts --lead ms after the viewer has drawn the first terrain at the start
// spot; the group stands still for --hold s, then walks east.
const { execFileSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { Vec3 } = require('vec3');
const { startServer, connectBot, sleep, nextChat } = require('./lib');
const { startQemu, ROOT } = require('./qemu');

// prismarine-viewer's node side only needs its WorldView (the renderer that runs in the
// browser is bundled separately), but it also loads a headless renderer that requires
// the native `canvas` package. That renderer is never used here, so stub the module
// instead of compiling it.
const Module = require('module');
const realLoad = Module._load;
Module._load = function (request, ...rest) {
  if (request === 'canvas') return { createCanvas: () => { throw new Error('canvas is not available'); } };
  return realLoad.call(this, request, ...rest);
};

const args = process.argv.slice(2);
const opt = (name, def) => {
  const i = args.indexOf('--' + name);
  return i >= 0 ? args[i + 1] : def;
};
const flag = (name) => args.includes('--' + name);
const SERVER = opt('server', 'qemu');
const SECONDS = parseFloat(opt('seconds', '18'));
const FPS = parseFloat(opt('fps', '10'));
const OUT = path.resolve(ROOT, opt('out', 'docs/images/exploring.gif'));
const PORT = parseInt(opt('port', '25640'));
const VIEWER_PORT = PORT + 1;
const WIDTH = parseInt(opt('width', '600'));
const HEIGHT = parseInt(opt('height', '338'));
const GIF_WIDTH = parseInt(opt('gif-width', '420'));
const HOLD_S = parseFloat(opt('hold', '5'));          // watch the terrain appear before walking
const LEAD_MS = parseFloat(opt('lead', '300'));       // start the clip this long after the first terrain is drawn
const FIRST_MESHES = 12;                              // "first terrain": this many section meshes
const SPEED = parseFloat(opt('speed', '3.5'));        // blocks per second (walking)
// unexplored savanna with some hills for seed 42 (the QEMU build's seed), walked eastwards
const START = [parseFloat(opt('x', '160')), parseFloat(opt('z', '-136'))];
const CAPTURE = opt('capture', 'screencast');
const DEBUG = flag('debug');
const VIEW = parseInt(opt('view', '4'));   // the QEMU build's view distance
const EXPLORERS = ['Alice', 'Bob', 'Carol', 'Dave'];
const CAMERA = 'Tester';                               // an operator in include/config_qemu.h

// mineflayer angles (radians, yaw 0 = north) <-> protocol angles (degrees)
const toNotchYaw = (yaw) => (180 / Math.PI) * (Math.PI - yaw);
const toNotchPitch = (pitch) => (180 / Math.PI) * -pitch;

// surface height from the blocks this bot has received (null if not loaded yet)
function groundY(bot, x, z) {
  for (let y = 160; y > 1; y--) {
    const b = bot.blockAt(new Vec3(Math.floor(x), y, Math.floor(z)));
    if (!b) return null;
    if (b.name !== 'air' && b.name !== 'cave_air' && b.boundingBox === 'block') return y + 1;
    if (b.name === 'water') return y + 1;
  }
  return null;
}

function sendPose(bot, x, y, z, yaw, pitch) {
  bot.entity.position = new Vec3(x, y, z);
  bot.entity.yaw = yaw;
  bot.entity.pitch = pitch;
  bot._client.write('position_look', { x, y, z, yaw: toNotchYaw(yaw), pitch: toNotchPitch(pitch), onGround: true });
  bot.emit('move');
}

(async () => {
  console.log(`recording ${SECONDS} s at ${FPS} fps from the ${SERVER} server -> ${path.relative(ROOT, OUT)}`);
  let server;
  if (SERVER === 'qemu') {
    server = startQemu({ port: PORT, mttcg: true, build: !flag('no-build') });
  } else {
    server = startServer(['--port', String(PORT), '--seed', '42', '--view', '6', '--ops', CAMERA]);
  }
  const frameDir = fs.mkdtempSync(path.join(os.tmpdir(), 'mcgif-'));
  const bots = [];
  let browser;
  let timer;
  try {
    await server.ready;
    const camera = await connectBot(PORT, CAMERA, { checkTimeoutInterval: 120000 });
    bots.push(camera);
    for (const name of EXPLORERS) bots.push(await connectBot(PORT, name, { checkTimeoutInterval: 120000 }));
    for (const b of bots) b.physicsEnabled = false;   // poses come from this script
    const explorers = bots.slice(1);
    for (const b of bots) camera.chat(`/gamemode creative ${b.username}`);
    camera.chat('/time set 1000');
    camera.chat('/weather clear');
    await sleep(500);

    // the viewer shows what the camera bot knows. mineflayer forwards each chunkColumnLoad
    // event twice, and the viewer would send (and mesh) every chunk twice: drop the second
    // call while the first is still in flight.
    const { WorldView } = require('prismarine-viewer/viewer');
    const loadChunk = WorldView.prototype.loadChunk;
    WorldView.prototype.loadChunk = async function (pos) {
      const key = `${pos.x},${pos.z}`;
      this.loading = this.loading || new Set();
      if (this.loading.has(key)) return;
      this.loading.add(key);
      try { await loadChunk.call(this, pos); } finally { this.loading.delete(key); }
    };
    require('prismarine-viewer').mineflayer(camera, { port: VIEWER_PORT, firstPerson: true, viewDistance: VIEW });
    const { chromium } = require('playwright');
    browser = await chromium.launch({
      args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist'],
    });
    // screencast: Chromium pushes every frame the compositor produces, with its timestamp
    // (cheap); screenshots: one PNG per frame (each a full readback with software WebGL)
    const context = await browser.newContext({ viewport: { width: WIDTH, height: HEIGHT } });
    // count the section meshes the viewer's web workers deliver: the browser needs a few
    // seconds to mesh new chunks (more while QEMU competes for the CPU)
    await context.addInitScript(() => {
      window.meshes = 0;
      const RealWorker = window.Worker;
      window.Worker = function (src) {
        const w = new RealWorker(src);
        w.addEventListener('message', ({ data }) => { if (data && data.type === 'geometry') window.meshes++; });
        return w;
      };
    });
    const page = await context.newPage();
    await page.goto(`http://127.0.0.1:${VIEWER_PORT}`);
    await page.waitForSelector('canvas', { timeout: 60000 });
    await sleep(1500);

    // jump into unexplored land: the screen starts (nearly) empty and fills in. The clip
    // starts once the viewer has drawn the first terrain there, so it does not open with
    // seconds of empty sky while the device generates the first chunks and the browser
    // meshes them.
    const tp = nextChat(camera, /Teleported|Teleport/i, 5000).catch(() => {});
    const tTp = Date.now();
    let chunksSeen = 0;
    camera._client.on('map_chunk', () => { chunksSeen++; });
    const meshesBefore = await page.evaluate(() => window.meshes);
    for (const b of bots) camera.chat(`/tp ${b.username} ${START[0]} 120 ${START[1]}`);
    await tp;
    // the camera's position as the viewer knows it (mineflayer emits no move for a teleport
    // while physics is off): from here on the viewer loads the chunks around the start spot
    sendPose(camera, START[0] - 10, 125, START[1], -Math.PI / 2, -0.6);   // looking east, down
    while (Date.now() - tTp < 60000 && (await page.evaluate(() => window.meshes)) < meshesBefore + FIRST_MESHES) {
      await sleep(100);
    }
    if (DEBUG) console.log(`first terrain drawn ${Date.now() - tTp} ms after the teleport (${chunksSeen} chunks received)`);
    await sleep(LEAD_MS);

    const t0 = Date.now();
    // start on the ground (it is drawn by now), so the camera does not swoop down first
    const g0 = groundY(camera, START[0], START[1]);
    let lastY = explorers.map(() => (g0 !== null ? g0 : 110));
    let camY = (g0 !== null ? g0 : 114) + 11;
    const formation = [[2.5, -4], [-1, -1.5], [1, 2.5], [-4, 4.5]];   // forward, sideways
    const tick = () => {
      const t = (Date.now() - t0) / 1000;
      const walk = Math.max(0, t - HOLD_S);
      const heading = 0.35 * Math.sin(walk / 7);                       // gentle curve, mostly east
      const dir = [Math.cos(heading), Math.sin(heading)];
      const side = [-dir[1], dir[0]];
      const along = SPEED * walk;
      // the group's centre follows the integral of the heading (close enough: small curve)
      const cx = START[0] + along * Math.cos(heading * 0.5);
      const cz = START[1] + along * Math.sin(heading * 0.5);
      let ySum = 0;
      explorers.forEach((b, i) => {
        const [f, s] = formation[i];
        const wiggle = Math.sin(walk * 1.3 + i * 1.7) * 0.8;
        const x = cx + dir[0] * f + side[0] * (s + wiggle);
        const z = cz + dir[1] * f + side[1] * (s + wiggle);
        const g = groundY(b, x, z);
        if (g !== null) lastY[i] = lastY[i] + (g - lastY[i]) * 0.5;   // ease onto the terrain
        ySum += lastY[i];
        const yaw = Math.atan2(-dir[0], -dir[1]) + Math.sin(walk * 0.9 + i) * 0.3;
        sendPose(b, x, lastY[i], z, yaw, 0);
      });
      // camera: behind and above the group, looking a little ahead of it
      const groupY = ySum / explorers.length;
      camY = camY + (groupY + 11 - camY) * 0.1;
      const camX = cx - dir[0] * 10, camZ = cz - dir[1] * 10;
      const lookX = cx + dir[0] * 4, lookZ = cz + dir[1] * 4, lookY = groupY;
      const dx = lookX - camX, dy = lookY - (camY + 1.62), dz = lookZ - camZ;
      sendPose(camera, camX, camY, camZ, Math.atan2(-dx, -dz), Math.atan2(dy, Math.sqrt(dx * dx + dz * dz)));
    };
    timer = setInterval(tick, 50);
    if (DEBUG) {
      const dbg = setInterval(() => {
        if (!timer) return clearInterval(dbg);
        const t = ((Date.now() - t0) / 1000).toFixed(1);
        const parts = explorers.map((b) => {
          const seen = camera.players[b.username] && camera.players[b.username].entity;
          const sp = seen ? seen.position : null;
          const me = b.entity.position;
          return `${b.username} sent ${me.x.toFixed(0)},${me.y.toFixed(0)},${me.z.toFixed(0)} seen ` +
            (sp ? `${sp.x.toFixed(0)},${sp.y.toFixed(0)},${sp.z.toFixed(0)}` : '-');
        });
        const c = camera.entity.position;
        console.log(`t=${t} cam ${c.x.toFixed(0)},${c.y.toFixed(0)},${c.z.toFixed(0)} chunks ${chunksSeen} | ${parts.join(' | ')}`);
      }, 2000);
    }

    const vf = `fps=${FPS},scale=${GIF_WIDTH}:-1:flags=lanczos,split[a][b];` +
      '[a]palettegen=max_colors=96:stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=5:diff_mode=rectangle';
    fs.mkdirSync(path.dirname(OUT), { recursive: true });
    // frames -> ffmpeg concat list with their real durations
    const encode = (frames) => {
      const list = frames.map((f, i) => {
        const next = i + 1 < frames.length ? frames[i + 1].ts : t0 + SECONDS * 1000;
        return `file '${f.file}'\nduration ${(Math.max(1, next - f.ts) / 1000).toFixed(3)}`;
      }).join('\n') + `\nfile '${frames[frames.length - 1].file}'\n`;
      fs.writeFileSync(path.join(frameDir, 'list.txt'), list);
      execFileSync('ffmpeg', ['-y', '-loglevel', 'error', '-f', 'concat', '-safe', '0', '-i',
        path.join(frameDir, 'list.txt'), '-vf', vf, '-loop', '0', OUT], { stdio: 'inherit' });
    };
    if (CAPTURE === 'screencast') {
      const frames = [];
      let pending = null;   // the last frame before t0 starts the clip
      const cdp = await context.newCDPSession(page);
      cdp.on('Page.screencastFrame', ({ data, metadata, sessionId }) => {
        cdp.send('Page.screencastFrameAck', { sessionId }).catch(() => {});
        const ts = Math.round(metadata.timestamp * 1000);
        const file = path.join(frameDir, `s${String(frames.length + (pending ? 1 : 0)).padStart(5, '0')}.jpg`);
        if (ts < t0) {
          pending = { file, ts: t0, data };
          return;
        }
        if (ts > t0 + SECONDS * 1000) return;
        fs.writeFileSync(file, Buffer.from(data, 'base64'));
        frames.push({ file, ts });
      });
      await cdp.send('Page.startScreencast', { format: 'jpeg', quality: 92, maxWidth: WIDTH, maxHeight: HEIGHT });
      await sleep(SECONDS * 1000 - (Date.now() - t0) + 300);
      await cdp.send('Page.stopScreencast').catch(() => {});
      clearInterval(timer);
      timer = null;
      if (pending) {
        fs.writeFileSync(pending.file, Buffer.from(pending.data, 'base64'));
        frames.unshift({ file: pending.file, ts: pending.ts });
      }
      if (!frames.length) throw new Error('no frames captured');
      console.log(`${frames.length} frames (${(frames.length / SECONDS).toFixed(1)} fps rendered)`);
      encode(frames);
    } else {
      // frames with their real timestamps -> constant fps
      const frames = [];
      const interval = 1000 / FPS;
      while (Date.now() - t0 < SECONDS * 1000) {
        const ts = Date.now();
        const file = path.join(frameDir, `f${String(frames.length).padStart(5, '0')}.png`);
        await page.screenshot({ path: file });
        frames.push({ file, ts });
        const wait = interval - (Date.now() - ts);
        if (wait > 0) await sleep(wait);
      }
      clearInterval(timer);
      timer = null;
      console.log(`${frames.length} frames (${(frames.length / SECONDS).toFixed(1)} fps captured)`);
      encode(frames);
    }
    console.log(`wrote ${path.relative(ROOT, OUT)} (${(fs.statSync(OUT).size / 1048576).toFixed(1)} MB)`);
    if (flag('keep-frames')) console.log('frames kept in ' + frameDir);
    else fs.rmSync(frameDir, { recursive: true, force: true });
  } catch (e) {
    console.log('FAILED: ' + (e.stack || e.message));
    process.exitCode = 1;
  } finally {
    if (timer) clearInterval(timer);
    if (browser) await browser.close().catch(() => {});
    bots.forEach((b) => b.quit());
    await sleep(500);
    await server.stop();
    process.exit(process.exitCode || 0);
  }
})();
