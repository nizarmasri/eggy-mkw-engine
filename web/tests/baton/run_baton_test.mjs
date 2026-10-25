// Headless Chromium run of web/tests/baton/baton_test.cpp: the baton build's machinery (no JSPI,
// guest threads on workers, aurora split with the browser main thread) without the game.
//
//   node web/tests/baton/run_baton_test.mjs BUILD_DIR [LOGFILE]
//
// BUILD_DIR holds baton_test.html/.js/.wasm (web/tests/baton/run_baton_test.sh builds them).
// The page is served cross-origin isolated (COOP/COEP, as Eggy serves the game) and JSPI is
// removed from it before anything runs. Checks, in one page load:
//   * the module runs without WebAssembly.Suspending / promising, in a cross-origin isolated page;
//   * main() and the guest contexts run on workers, aurora's SDL window steps on the main thread,
//     the WebGPU instance without TimedWaitAny (its waits poll), aurora waits for a 0x0 canvas;
//   * "BATON-TEST PASS": thread_locals shared across contexts, exceptions and recursion inside
//     them, a proxied promise (and an `async` function) that blocks the worker, the product's
//     own disc/storage/eggy imports answering from the main thread, frames presented from a
//     guest context;
//   * the canvas shows the colour the guest cleared it to (GXSetCopyClear 200,40,160);
//   * no WebGPU validation error, page error, abort or trap.
// Prints the measured hand-off and main-thread costs. Exit code 0 on success.
import fs from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import zlib from 'node:zlib';
import { pathToFileURL } from 'node:url';

const buildDir = process.argv[2];
const logFile = process.argv[3] || path.join(os.tmpdir(), 'baton-test.log');
if (!buildDir || !fs.existsSync(path.join(buildDir, 'baton_test.html'))) {
  console.error('usage: run_baton_test.mjs BUILD_DIR [LOGFILE]   (BUILD_DIR/baton_test.html must exist)');
  process.exit(2);
}
const pageDir = path.resolve(buildDir);
fs.writeFileSync(logFile, '');
const t0 = Date.now();
const log = (line) => {
  const text = `[${((Date.now() - t0) / 1000).toFixed(2).padStart(7)}] ${line}`;
  fs.appendFileSync(logFile, text + '\n');
  console.log(text);
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// Cross-origin isolated, as Eggy serves the game: SharedArrayBuffer needs it.
const types = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.data': 'application/octet-stream' };
const server = http.createServer((req, res) => {
  const file = path.join(pageDir, decodeURIComponent(new URL(req.url, 'http://x').pathname));
  if (!file.startsWith(pageDir) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) {
    res.writeHead(404).end();
    return;
  }
  res.writeHead(200, {
    'Content-Type': types[path.extname(file)] || 'application/octet-stream',
    'Cache-Control': 'no-store',
    'Cross-Origin-Opener-Policy': 'same-origin',
    'Cross-Origin-Embedder-Policy': 'require-corp',
    'Cross-Origin-Resource-Policy': 'same-origin',
  });
  fs.createReadStream(file).pipe(res);
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const url = `http://127.0.0.1:${server.address().port}/baton_test.html`;

// Minimal PNG reader (8-bit RGB/RGBA, not interlaced: what Chromium screenshots are).
function readPng(buffer) {
  let pos = 8;
  let width = 0, height = 0, channels = 0;
  const idat = [];
  while (pos < buffer.length) {
    const length = buffer.readUInt32BE(pos);
    const type = buffer.toString('latin1', pos + 4, pos + 8);
    const data = buffer.subarray(pos + 8, pos + 8 + length);
    if (type === 'IHDR') {
      width = data.readUInt32BE(0);
      height = data.readUInt32BE(4);
      if (data[8] !== 8 || data[12] !== 0) throw new Error('unsupported PNG (bit depth / interlace)');
      channels = { 2: 3, 6: 4 }[data[9]];
      if (!channels) throw new Error(`unsupported PNG colour type ${data[9]}`);
    } else if (type === 'IDAT') {
      idat.push(data);
    }
    pos += 12 + length;
  }
  const raw = zlib.inflateSync(Buffer.concat(idat));
  const stride = width * channels;
  const pixels = Buffer.alloc(stride * height);
  for (let y = 0; y < height; y++) {
    const filter = raw[y * (stride + 1)];
    const line = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));
    for (let x = 0; x < stride; x++) {
      const a = x >= channels ? pixels[y * stride + x - channels] : 0;
      const b = y > 0 ? pixels[(y - 1) * stride + x] : 0;
      const c = x >= channels && y > 0 ? pixels[(y - 1) * stride + x - channels] : 0;
      let v = line[x];
      if (filter === 1) v += a;
      else if (filter === 2) v += b;
      else if (filter === 3) v += (a + b) >> 1;
      else if (filter === 4) {
        const p = a + b - c;
        const pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
        v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
      }
      pixels[y * stride + x] = v & 0xff;
    }
  }
  return { width, height, at: (x, y) => Array.from(pixels.subarray((y * width + x) * channels, (y * width + x) * channels + 3)) };
}

const pw = await import(process.env.PLAYWRIGHT_MODULE
  ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href
  : 'playwright');
const { chromium } = pw.default ?? pw;

const problems = [];
const lines = [];
const need = (re, what) => { if (!lines.some((l) => re.test(l))) problems.push(`missing: ${what}`); };
let verdict = 1;
try {
  const browser = await chromium.launch({
    executablePath: process.env.CHROMIUM_PATH || undefined,
    args: ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-vulkan=swiftshader',
      '--use-webgpu-adapter=swiftshader', '--use-angle=swiftshader', '--ignore-gpu-blocklist', '--no-sandbox'],
  });
  const page = await (await browser.newContext({ viewport: { width: 1024, height: 768 } })).newPage();
  const BAD = [/WebGPU error/, /RuntimeError/, /Aborted\(/, /unreachable/, /FATAL/, /BATON-TEST FAIL/,
    /memory access out of bounds/, /Uncaught/, /could not run a call on the browser main thread/];
  page.on('console', (msg) => {
    for (const line of msg.text().split('\n')) {
      lines.push(line);
      log(`console.${msg.type()}: ${line}`);
      if (BAD.some((re) => re.test(line))) problems.push(line);
    }
  });
  page.on('pageerror', (e) => { log(`pageerror: ${e.name}: ${e.message}`); problems.push(`${e.name}: ${e.message}`); });
  page.on('crash', () => problems.push('the page crashed'));
  // No JSPI on this page, as on Safari 26: the module must not need it.
  await page.addInitScript(() => {
    for (const name of ['Suspending', 'promising']) {
      try { delete WebAssembly[name]; } catch (_) { /* not configurable */ }
    }
    console.log(`[ios26] test page: JSPI removed (WebAssembly.Suspending is ${typeof WebAssembly.Suspending})`);
  });
  // As web/tests/aurora_depth_range_test.mjs: an older test Chromium lacks the WGSL language
  // feature uniform_buffer_standard_layout that aurora's GX shaders use; rewrite that member.
  await page.addInitScript(() => {
    if (!self.GPUDevice || (navigator.gpu && navigator.gpu.wgslLanguageFeatures &&
        navigator.gpu.wgslLanguageFeatures.has('uniform_buffer_standard_layout'))) return;
    const create = GPUDevice.prototype.createShaderModule;
    GPUDevice.prototype.createShaderModule = function (descriptor) {
      let code = descriptor && descriptor.code;
      if (typeof code === 'string' && code.includes('array_start: array<u32, 12>')) {
        code = code.replace('array_start: array<u32, 12>', 'array_start: array<vec4u, 3>')
          .replace(/ubuf\.array_start\[(\d+)\]/g, (_, n) => `ubuf.array_start[${Math.floor(n / 4)}][${n % 4}]`);
        descriptor = { ...descriptor, code };
      }
      return create.call(this, descriptor);
    };
  });
  // The canvas has no layout box when the module starts: aurora waits for it from a worker,
  // which sleeps in place of emscripten_sleep (no JSPI to suspend with).
  await page.addInitScript(() => {
    document.addEventListener('DOMContentLoaded', () => {
      const canvas = document.getElementById('canvas');
      if (canvas) canvas.style.display = 'none';
    });
  });
  await page.goto(url);
  const env = await page.evaluate(() => ({
    jspi: typeof WebAssembly.Suspending,
    promising: typeof WebAssembly.promising,
    isolated: globalThis.crossOriginIsolated === true,
    cores: navigator.hardwareConcurrency,
  }));
  log(`page: WebAssembly.Suspending ${env.jspi}, promising ${env.promising}, crossOriginIsolated ${env.isolated}, ${env.cores} cores`);
  if (env.jspi !== 'undefined' || env.promising !== 'undefined') problems.push('JSPI is still present on the page');
  if (!env.isolated) problems.push('the page is not cross-origin isolated');
  await sleep(4000);
  const waited = lines.some((l) => /Waiting for the canvas before creating the surface/.test(l));
  log(`0x0 canvas start: aurora waited=${waited}`);
  if (!waited) problems.push('aurora did not wait for the 0x0 canvas');
  await page.evaluate(() => { document.getElementById('canvas').style.display = ''; });

  const deadline = Date.now() + 120000;
  let result = null;
  while (Date.now() < deadline && !result) {
    result = lines.find((l) => /^BATON-TEST (PASS|FAIL)/.test(l)) || null;
    if (problems.some((p) => /crashed|Aborted|RuntimeError/.test(p))) break;
    await sleep(250);
  }
  if (!result) problems.push('no BATON-TEST verdict within 120 s');
  else if (result !== 'BATON-TEST PASS') problems.push(result);
  // A few more seconds: the heartbeat and the per-3-s statistics, and any late error.
  await sleep(6000);

  need(/^\[ios26\] baton glue on the browser main thread: JSPI absent/, 'the baton glue line on the main thread');
  need(/^\[baton-test\] main\(\) on a worker/, 'main() on a worker');
  need(/^\[baton-test\] product imports on the main thread/, "the product's disc, storage and eggy imports answering from the main thread");
  need(/^\[ios26\] aurora: window::initialize .* on the browser main thread/, 'SDL window setup on the main thread');
  need(/^\[ios26\] aurora: WebGPU instance without TimedWaitAny/, 'the WebGPU instance without TimedWaitAny');
  need(/Canvas is ready; creating the surface/, 'aurora creating the surface once the canvas was shown');
  need(/^\[ios26\] baton: context #\d+ is running on its own worker/, 'a context running on its own worker');
  need(/^\[ios26\] baton: .* frames .* hand-offs/, 'the per-frame baton statistics');
  need(/^\[ios26\] heartbeat/, 'the main thread heartbeat');
  need(/^\[ios26\] baton by name, per frame: JS .*\| aurora .*end_frame/, 'the per-frame breakdown by name');
  // baton_test.cpp prints 300 lines from a worker without waiting for the main thread: all of
  // them must still reach the console, in order.
  const consoleLines = lines.map((l) => /^\[baton-test\] console (\d+)$/.exec(l)).filter(Boolean).map((m) => Number(m[1]));
  if (consoleLines.length !== 300 || consoleLines.some((n, i) => n !== i)) {
    problems.push(`a worker's console lines arrived incomplete or out of order (${consoleLines.length} of 300)`);
  }

  const box = await page.evaluate(() => {
    const r = document.getElementById('canvas').getBoundingClientRect();
    return { x: r.x, y: r.y, width: r.width, height: r.height };
  });
  const shot = await page.screenshot({ timeout: 10000 });
  fs.writeFileSync(logFile.replace(/\.log$/, '') + '.png', shot);
  const png = readPng(shot);
  const cx = Math.round(box.x + box.width / 2), cy = Math.round(box.y + box.height / 2);
  const pixel = png.at(Math.min(png.width - 1, cx), Math.min(png.height - 1, cy));
  log(`canvas ${box.width}x${box.height} at ${box.x},${box.y}; centre pixel ${pixel.join(',')} (want 200,40,160)`);
  if (Math.abs(pixel[0] - 200) > 16 || Math.abs(pixel[1] - 40) > 16 || Math.abs(pixel[2] - 160) > 16) {
    problems.push(`the canvas does not show the guest's clear colour (centre pixel ${pixel.join(',')})`);
  }
  for (const l of lines.filter((l) => /^\[baton-test\] \d+ frames|^\[ios26\] baton: \d/.test(l)).slice(-3)) log(`measured: ${l}`);
  for (const p of problems) log(`problem: ${p}`);
  verdict = problems.length ? 1 : 0;
  log(verdict === 0 ? 'BATON TEST PASS' : 'BATON TEST FAIL');
} catch (e) {
  log(`driver error: ${e && e.stack || e}`);
}
console.log(`full log: ${logFile}`);
process.exit(verdict);
