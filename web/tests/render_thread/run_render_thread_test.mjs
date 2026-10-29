// Headless Chromium test of the render thread (EGGY.md, "The render thread") with no game data:
// render_thread_test.cpp built by web/tests/render_thread/run.sh.
//
//   node web/tests/render_thread/run_render_thread_test.mjs BUILD_DIR [LOGFILE]
//
// BUILD_DIR holds render_thread_test.html/.js/.wasm. The page is served cross-origin isolated
// (COOP/COEP, as Eggy serves it: SharedArrayBuffer needs it) and loaded twice:
//   * as is: aurora starts the render thread, which takes the canvas; GX is recorded on the JSPI
//     main thread and every frame is sealed, encoded, submitted and presented on the render thread
//     ("RENDER-THREAD-TEST PASS" checks the counters); then the screenshot must show the clear
//     colour, the vertex-coloured quad on the left and the textured quad on the right;
//   * with ?renderThread=0: the same build draws on the main thread, as a page that opts out does.
// Any WebGPU validation error, trap, abort or deadlock (no verdict within the time limit) fails.
// Exit code 0 on success. The log goes to LOGFILE; the script ends with process.exit because
// browser.close() can hang while a JSPI stack is suspended.
import fs from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import zlib from 'node:zlib';
import { pathToFileURL } from 'node:url';

const buildDir = process.argv[2];
const logFile = process.argv[3] || path.join(os.tmpdir(), 'render-thread-test.log');
if (!buildDir) {
  console.error('usage: run_render_thread_test.mjs BUILD_DIR [LOGFILE]');
  process.exit(2);
}
const pageDir = path.resolve(buildDir);
if (!fs.existsSync(path.join(pageDir, 'render_thread_test.html'))) {
  console.error(`${pageDir}/render_thread_test.html not found (build render_thread_test)`);
  process.exit(2);
}
fs.writeFileSync(logFile, '');
const t0 = Date.now();
const log = (line) => {
  const text = `[${((Date.now() - t0) / 1000).toFixed(2).padStart(7)}] ${line}`;
  fs.appendFileSync(logFile, text + '\n');
  console.log(text);
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const types = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.wasm': 'application/wasm' };
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
const origin = `http://127.0.0.1:${server.address().port}`;

const pw = await import(process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright');
const { chromium } = pw.default ?? pw;

// Minimal PNG decoder (8-bit RGB/RGBA, non-interlaced: what Chromium's screenshots are).
function decodePng(buf) {
  let pos = 8;
  let width = 0, height = 0, channels = 0;
  const idat = [];
  while (pos < buf.length) {
    const len = buf.readUInt32BE(pos);
    const type = buf.toString('latin1', pos + 4, pos + 8);
    const data = buf.subarray(pos + 8, pos + 8 + len);
    if (type === 'IHDR') {
      width = data.readUInt32BE(0);
      height = data.readUInt32BE(4);
      if (data[8] !== 8 || data[12] !== 0) throw new Error('unsupported PNG');
      channels = { 2: 3, 6: 4 }[data[9]];
      if (!channels) throw new Error(`unsupported PNG colour type ${data[9]}`);
    } else if (type === 'IDAT') {
      idat.push(data);
    }
    pos += 12 + len;
  }
  const raw = zlib.inflateSync(Buffer.concat(idat));
  const stride = width * channels;
  const out = Buffer.alloc(height * stride);
  for (let y = 0; y < height; y++) {
    const filter = raw[y * (stride + 1)];
    const line = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));
    for (let x = 0; x < stride; x++) {
      const a = x >= channels ? out[y * stride + x - channels] : 0;
      const b = y > 0 ? out[(y - 1) * stride + x] : 0;
      const c = x >= channels && y > 0 ? out[(y - 1) * stride + x - channels] : 0;
      let v = line[x];
      if (filter === 1) v += a;
      else if (filter === 2) v += b;
      else if (filter === 3) v += (a + b) >> 1;
      else if (filter === 4) {
        const p = a + b - c;
        const pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
        v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
      }
      out[y * stride + x] = v & 255;
    }
  }
  return { width, height, channels, pixel: (x, y) => [...out.subarray(y * stride + x * channels, y * stride + x * channels + 3)] };
}

// render_thread_test.cpp's colours and where they are, as fractions of the canvas.
const EXPECT = [
  { what: 'clear colour', fx: 0.5, fy: 0.04, rgb: [30, 60, 200] },
  { what: 'vertex-coloured quad', fx: 0.25, fy: 0.5, rgb: [220, 40, 40] },
  { what: 'textured quad', fx: 0.75, fy: 0.5, rgb: [40, 200, 90] },
];
const TOLERANCE = 40;

const problems = [];
let browser;
async function run(label, query, expectThread) {
  const page = await (await browser.newContext({ viewport: { width: 1024, height: 768 } })).newPage();
  const lines = [];
  const BAD = [/WebGPU error/, /RuntimeError/, /SuspendError/, /Aborted\(/, /unreachable/, /FATAL/, /\[render\]/,
    /memory access out of bounds/, /Uncaught/, /RENDER-THREAD-TEST FAIL/, /pthread_create: /];
  page.on('console', (msg) => {
    for (const line of msg.text().split('\n')) {
      lines.push(line);
      log(`${label} console.${msg.type()}: ${line}`);
      if (BAD.some((re) => re.test(line))) problems.push(`${label}: ${line}`);
    }
  });
  page.on('pageerror', (e) => { log(`${label} pageerror: ${e.name}: ${e.message}`); problems.push(`${label}: ${e.name}: ${e.message}`); });
  page.on('crash', () => problems.push(`${label}: the page crashed`));
  await page.goto(`${origin}/render_thread_test.html${query}`);
  const isolated = await page.evaluate(() => globalThis.crossOriginIsolated === true).catch(() => false);
  log(`${label}: crossOriginIsolated=${isolated}`);
  if (!isolated) problems.push(`${label}: the page is not cross-origin isolated`);

  const deadline = Date.now() + 120000;
  let verdict = null;
  while (Date.now() < deadline && !verdict) {
    verdict = lines.find((l) => /^RENDER-THREAD-TEST (PASS|FAIL)/.test(l)) || null;
    await sleep(250);
  }
  if (!verdict) problems.push(`${label}: no RENDER-THREAD-TEST verdict within 120 s (a deadlock?)`);
  const started = lines.some((l) => /Render thread started: it owns/.test(l));
  if (started !== expectThread) problems.push(`${label}: render thread ${started ? 'started' : 'did not start'}`);
  // A few more frames, so a late validation error is still seen and the picture has settled.
  await sleep(2000);
  const box = await page.evaluate(() => {
    const r = document.getElementById('canvas')?.getBoundingClientRect();
    return r ? { x: r.x, y: r.y, width: r.width, height: r.height } : null;
  });
  const shot = logFile.replace(/\.log$/, '') + `-${label}.png`;
  if (!box || box.width < 16 || box.height < 16) {
    problems.push(`${label}: the canvas has no size (${JSON.stringify(box)})`);
  } else {
    try {
      const png = await page.screenshot({ path: shot, clip: box, timeout: 10000 });
      const img = decodePng(png);
      for (const e of EXPECT) {
        const x = Math.min(img.width - 1, Math.floor(e.fx * img.width));
        const y = Math.min(img.height - 1, Math.floor(e.fy * img.height));
        const got = img.pixel(x, y);
        const ok = got.every((v, i) => Math.abs(v - e.rgb[i]) <= TOLERANCE);
        log(`${label}: ${e.what} at (${x}, ${y}) of ${img.width}x${img.height}: rgb(${got}) expected rgb(${e.rgb}) ${ok ? 'ok' : 'WRONG'}`);
        if (!ok) problems.push(`${label}: ${e.what} is rgb(${got}), expected rgb(${e.rgb})`);
      }
    } catch (e) {
      problems.push(`${label}: screenshot failed: ${e.message}`);
    }
  }
  await page.close().catch(() => {});
}

let code = 1;
try {
  browser = await chromium.launch({
    channel: 'chromium',
    args: ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-vulkan=swiftshader',
      '--use-webgpu-adapter=swiftshader', '--use-angle=swiftshader', '--ignore-gpu-blocklist'],
  });
  await run('render-thread', '', true);
  await run('main-thread', '?renderThread=0', false);
  for (const p of problems) log(`problem: ${p}`);
  code = problems.length ? 1 : 0;
  log(code === 0 ? 'RENDER THREAD TEST PASS' : 'RENDER THREAD TEST FAIL');
} catch (e) {
  log(`driver error: ${e && e.stack || e}`);
}
console.log(`full log: ${logFile}`);
process.exit(code);
