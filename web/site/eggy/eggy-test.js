// The engine's test page (web/site/eggy.html): boots Eggy mode with no clicks, loads one race from
// a config v1 built from the URL, holds the pads it is told to, and shows what the engine reports.
// The CI smoke opens it headless and keeps the console and a screenshot every ten seconds.
//
// URL parameters (all optional):
//   course=0x01  character=0x03  vehicle=0x08   the first human's picks
//   humans=1     myPort=0                        humans on ports 0..humans-1; the screen follows myPort
//   field=4      cc=150  mirror=0  items=1  cpu=2  mode=race|balloon|coin  laps=3  seed=12345
//   hold=A       buttons every human holds from frame 0 (A B X Y L R Z Up Down Left Right), x=0 y=0
//   skip=20      draw one frame in `skip` (render skip on for the others; 0 = draw every frame)
//   finishAll=-1 frame at whose end everyone still racing is finished by place
//   away=        "port:frame" hands that port to the computer from frame+1
//   auto=1       go as soon as the race is parked
// Race API 2 (each only when given):
//   drawEvery=2  the engine draws one pass in n (1-4); use with skip=0
//   slowDraw=0   every drawn race frame takes this many microseconds longer (proves the pace)
//   pacing=1     0 runs the race without wall-clock catch-up (one frame per pass, as before)
//   results=0    seconds of the game's own finish and results screens after the report
//   hashParts=1  the engine logs each part of every hash it takes, to find where two runs part
// Logged: the hash at frame 0 and at every lockstep checkpoint ((frame + 1) % 60 == 0), perf()
// every 2 s, and the race's game frames per wall second from go to the report.
(function () {
  'use strict';
  const params = new URLSearchParams(location.search);
  const num = (name, fallback) => {
    const v = params.get(name);
    if (v === null || v === '') return fallback;
    const n = Number(v);
    return Number.isFinite(n) ? n : fallback;
  };
  const PAD = { Left: 0x0001, Right: 0x0002, Down: 0x0004, Up: 0x0008, Z: 0x0010, R: 0x0020, L: 0x0040, A: 0x0100, B: 0x0200, X: 0x0400, Y: 0x0800 };

  const mode = params.get('mode') || 'race';
  const battle = mode !== 'race';
  const humans = Math.max(1, Math.min(4, num('humans', 1)));
  const field = Math.max(humans, Math.min(12, num('field', 4)));
  const myPort = num('myPort', 0);
  const picks = [
    [num('character', 0x03), num('vehicle', 0x08)],
    [num('character2', 0x10), num('vehicle2', 0x01)],
    [num('character3', 0x09), num('vehicle3', 0x05)],
    [num('character4', 0x0a), num('vehicle4', 0x13)],
  ];
  const cpus = [[0x00, 0x01], [0x07, 0x13], [0x0f, 0x04], [0x0b, 0x02], [0x02, 0x14], [0x13, 0x16], [0x17, 0x0e], [0x16, 0x0f], [0x08, 0x00], [0x05, 0x12], [0x12, 0x10], [0x14, 0x11]];
  const hold = (params.get('hold') ?? 'A').split(/[ ,+]/).filter(Boolean).reduce((b, k) => b | (PAD[k] || 0), 0);
  const stickX = num('x', 0), stickY = num('y', 0);
  const skip = num('skip', 20);
  const finishAllAt = num('finishAll', -1);
  const auto = num('auto', 1) !== 0;
  const away = (params.get('away') || '').split(',').filter(Boolean).map((s) => s.split(':').map(Number));
  const drawEvery = num('drawEvery', 0);
  const slowDraw = num('slowDraw', 0);
  const pacing = num('pacing', 1) !== 0;
  const resultsSeconds = num('results', 0);
  const hashParts = num('hashParts', 0) !== 0;

  // Config v1 (games/mkw/kit/config.ts): 24 header words, then 12 players of 8 words.
  function buildConfig() {
    const c = new Int32Array(24 + 12 * 8);
    c[0] = 1;
    c[1] = num('seed', 0x5eed1234) | 0;
    c[2] = battle ? 3 : 1;
    c[3] = mode === 'coin' ? 1 : 0;
    c[4] = battle ? 0 : ({ 50: 0, 100: 1, 150: 2 }[num('cc', 150)] ?? 2);
    c[5] = num('mirror', 0) ? 1 : 0;
    c[6] = [3, 0, 1, 2][num('items', 1)] ?? 0;
    c[7] = [3, 0, 1, 2][num('cpu', 2)] ?? 1;
    c[8] = battle ? 1 : 0;
    c[9] = num('course', battle ? 0x21 : 0x01);
    c[10] = battle ? 0 : num('laps', 3);
    c[11] = 0;
    c[12] = 1;
    c[13] = 0;
    c[14] = field;
    c[15] = humans;
    c[16] = myPort;
    c[17] = resultsSeconds;
    for (let slot = 0; slot < 12; slot++) {
      const at = 24 + slot * 8;
      if (slot >= field) {
        c[at] = 5; c[at + 1] = -1; c[at + 5] = 2;
        continue;
      }
      const human = slot < humans;
      const [character, vehicle] = human ? picks[slot] : cpus[slot - humans];
      c[at] = human ? 0 : 1;
      c[at + 1] = human ? slot : -1;
      c[at + 2] = character;
      c[at + 3] = vehicle;
      c[at + 4] = 0;
      c[at + 5] = battle ? slot % 2 : 2;
      c[at + 6] = 0;
    }
    return c;
  }

  const $ = (id) => document.getElementById(id);
  const say = (...a) => console.log('[eggy-test]', ...a);
  const fmt = (o) => JSON.stringify(o, (k, v) => (typeof v === 'number' && !Number.isInteger(v) ? Math.round(v * 100) / 100 : v));

  const engine = createEggyEngine({ canvas: $('canvas') });
  window.eggyEngine = engine;
  if (engine.dead) {
    $('state').textContent = 'dead: ' + engine.dead;
    say('dead', engine.dead);
    return;
  }

  let lastLogged = -1;
  let report = null;
  engine.setPadSource((frame) => {
    const pads = [];
    for (let port = 0; port < 4; port++) pads.push(port < humans ? { buttons: hold, x: stickX, y: stickY } : { buttons: 0, x: 0, y: 0 });
    return pads;
  });
  engine.onFrame((frame) => {
    if (skip > 0) engine.renderSkip(frame % skip !== skip - 1);
    if (frame === 0 || (frame + 1) % 60 === 0) {
      const k = engine.kart(myPort);
      say('frame', frame, 'hash', (engine.hash() >>> 0).toString(16), 'kart', fmt(k));
    }
  });
  let perfSeq = -1;
  setInterval(() => {
    const p = engine.perf();
    if (p && p.seq !== perfSeq && (p.seq % 2) === 0) say('perf', fmt(p));
    if (p) perfSeq = p.seq;
  }, 500);

  function render() {
    const st = engine.state;
    const frame = engine.frame;
    $('state').textContent = `${st}  frame ${frame}`;
    const k = engine.kart(Math.max(0, myPort));
    $('kart').textContent = k ? `speed ${k.speed.toFixed(1)} / ${k.maxSpeed.toFixed(1)}  mt ${k.mt}/${k.mtMax}  place ${k.place} lap ${k.lap}  countdown ${k.countdown}` : '';
    const lines = [];
    lines.push(`config  course ${params.get('course') || 'default'}  humans ${humans}  myPort ${myPort}  field ${field}  hold 0x${hold.toString(16)}`);
    lines.push(`kart(${myPort}) ${fmt(k)}`);
    for (let p = 0; p < humans; p++) if (p !== myPort) lines.push(`kart(${p}) ${fmt(engine.kart(p))}`);
    const hud = engine.hud();
    lines.push(`hud ${hud ? 'frame ' + hud.frame : 'null'}`);
    if (hud) for (const l of hud.lines) lines.push('  ' + fmt(l));
    lines.push(`hash ${(engine.hash() >>> 0).toString(16)}  timing ${fmt(engine.timing())}`);
    lines.push(`report ${fmt(report)}`);
    const d = engine.debug();
    if (d) {
      lines.push(`debug ${fmt(Object.fromEntries(Object.entries(d).filter(([key]) => key !== 'diag')))}`);
      lines.push(`diag ${d.diag.join(' ')}`);
    }
    $('panel').textContent = lines.join('\n');
    if (frame !== lastLogged && frame >= 0 && frame % 60 === 0) lastLogged = frame;
    setTimeout(render, 500);
  }
  render();

  (async () => {
    try {
      say('boot');
      await engine.boot();
      say('booted (idle)');
      engine.audio(true, true);
      const config = buildConfig();
      say('load', Array.from(config).join(','));
      await engine.load(config);
      say('parked');
      for (const [port, frame] of away) engine.away(port, frame);
      if (finishAllAt >= 0) engine.finishAll(finishAllAt);
      if (drawEvery > 0) engine.drawEvery(drawEvery);
      if (slowDraw > 0) engine.slowDraw(slowDraw);
      if (!pacing) engine.pacing(false);
      if (hashParts) engine.hashParts(true);
      let wentAt = 0;
      if (auto) {
        engine.go();
        wentAt = performance.now();
        say('go');
      }
      report = await engine.ended();
      say('ended', fmt(report));
      if (wentAt > 0 && report) {
        const secs = (performance.now() - wentAt) / 1000;
        say('race wall', 'frames', report.endFrame + 1, 'in', secs.toFixed(2), 's =', ((report.endFrame + 1) / secs).toFixed(1), 'game frames/s');
      }
    } catch (e) {
      say('failed', e && e.stack || e);
      $('state').textContent = 'failed: ' + (e && e.message || e);
    }
  })();
})();
