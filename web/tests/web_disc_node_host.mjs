// Runs one web test module (web_disc_test.mjs, web_riivolution_test.mjs) under Node; needs
// JSPI: Node 24+ with --experimental-wasm-jspi.
// usage: node --experimental-wasm-jspi web_disc_node_host.mjs <module.mjs> <Module options JSON>
//        (the options carry `arguments` and the mkw* settings, see web/js/library_mkw_dvd.js)
import { pathToFileURL } from 'node:url';

const [modulePath, optionsJson] = process.argv.slice(2);
if (typeof WebAssembly.Suspending != 'function') {
  console.error('FAIL: this Node has no JSPI (WebAssembly.Suspending); use Node 24+ with --experimental-wasm-jspi');
  process.exit(2);
}

let finish;
const finished = new Promise((resolve) => (finish = resolve));
const timer = setTimeout(() => finish('FAIL: timed out'), 180_000);
const print = (line) => {
  console.log(line);
  if (/^(PASS|FAIL) web (disc|riivolution) test|^FAIL: /.test(line)) finish(line);
};

// `eggySource: {manifestUrl, baseUrl}` (Eggy): the page supplies the disc itself through
// Module.mkwDiscSource instead of the module fetching it. Here the source reads the same test
// server over HTTP, so every assertion on the server's request log holds for both paths.
const options = JSON.parse(optionsJson);
const eggySource = options.eggySource;
delete options.eggySource;
if (eggySource) {
  options.mkwDiscSource = {
    manifest: async () => (await fetch(eggySource.manifestUrl)).json(),
    read: async (mount, relPath, offset, length) => {
      const url = eggySource.baseUrl + relPath.split('/').map(encodeURIComponent).join('/');
      const response = await fetch(url, { headers: { Range: `bytes=${offset}-${offset + length - 1}` } });
      if (response.status === 404 || response.status === 410) return null;
      if (response.status === 416) return new Uint8Array(0);
      if (response.status !== 206 && response.status !== 200) throw new Error(`HTTP ${response.status}`);
      const bytes = new Uint8Array(await response.arrayBuffer());
      return response.status === 206 ? bytes : bytes.subarray(offset, offset + length);
    },
  };
}

const createModule = (await import(pathToFileURL(modulePath).href)).default;
createModule({
  ...options,
  print,
  printErr: (line) => console.error(line),
  onAbort: (what) => finish(`FAIL: aborted: ${what}`),
}).catch((error) => finish(`FAIL: ${error && error.stack || error}`));

const result = await finished;
clearTimeout(timer);
if (!result.startsWith('PASS')) console.log(result);
process.exit(result.startsWith('PASS') ? 0 : 1);
