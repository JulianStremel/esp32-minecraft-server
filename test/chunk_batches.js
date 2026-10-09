'use strict';
// Chunk batches (1.20.2+, vanilla's PlayerChunkSender): every chunk packet is inside a
// ChunkBatchStart .. ChunkBatchFinished pair whose size is right, and the server waits
// for the client's acknowledgement: a client that does not acknowledge gets its first
// quota (and what was being prepared), then nothing until it acknowledges.
//   SERVER_BIN=.../mcserver node chunk_batches.js     (or --host <board ip>: as Tester)
const assert = require('assert');
const mc = require('minecraft-protocol');
const { startServer, connectBot, sleep, waitFor, VERSION } = require('./lib');

(async () => {
  const hi = process.argv.indexOf('--host'), host = hi > 0 ? process.argv[hi + 1] : null;
  const port = host ? 25565 : 25640 + Math.floor(Math.random() * 100);
  const srv = host ? null : startServer(['--port', String(port), '--seed', '42', '--no-mobs', '--view', '6'], { log: process.argv.includes('--log') });
  let bot, raw;
  try {
    if (srv) await srv.ready;
    // 1. a client that acknowledges (mineflayer does): batches are well formed
    bot = await connectBot(port, host ? 'Tester' : 'Batcher', host ? { host } : {});
    let open = false, inBatch = 0, batches = 0, chunks = 0, outside = 0, wrong = 0;
    bot._client.on('chunk_batch_start', () => { assert(!open, 'a batch started inside a batch'); open = true; inBatch = 0; });
    bot._client.on('map_chunk', () => { chunks++; if (open) inBatch++; else outside++; });
    bot._client.on('chunk_batch_finished', (p) => { if (p.batchSize !== inBatch) wrong++; open = false; batches++; });
    await waitFor(() => chunks >= 60, 30000, '60 chunks');
    await sleep(500);
    console.log(`acknowledging client: ${chunks} chunks in ${batches} batches, ${outside} outside a batch, ${wrong} with a wrong size`);
    assert.strictEqual(outside, 0, 'chunks outside a batch');
    assert.strictEqual(wrong, 0, 'batch sizes do not match their chunks');
    bot.quit();
    bot = null;
    await sleep(500);
    // 2. a client that does not acknowledge: one batch, then nothing until it does
    raw = mc.createClient({ host: host || '127.0.0.1', port, username: host ? 'Tester' : 'Silent', version: VERSION, auth: 'offline' });
    let rawChunks = 0, rawBatches = 0;
    raw.on('map_chunk', () => rawChunks++);
    raw.on('chunk_batch_finished', () => rawBatches++);
    await waitFor(() => rawBatches >= 1, 30000, 'the first batch');
    await sleep(3000);   // what was being prepared when the server stopped sending arrives
    const held = { chunks: rawChunks, batches: rawBatches };
    await sleep(3000);
    console.log(`silent client: ${held.chunks} chunks in ${held.batches} batch(es), then ${rawChunks - held.chunks} in 3 s more without an acknowledgement`);
    assert.strictEqual(rawChunks, held.chunks, 'chunks keep coming without an acknowledgement');
    assert(held.chunks <= 9 + 5, 'more than the first quota and the sends in flight');
    raw.write('chunk_batch_received', { chunksPerTick: 64 });
    await waitFor(() => rawBatches > held.batches && rawChunks > held.chunks, 10000, 'chunks after the acknowledgement');
    console.log(`after acknowledging: ${rawChunks} chunks in ${rawBatches} batches`);
    console.log('CHUNK BATCHES OK');
  } catch (e) {
    console.error('FAILED:', e);
    process.exitCode = 1;
  } finally {
    if (bot) bot.quit();
    if (raw) raw.end();
    if (srv) await srv.stop();
  }
})();
