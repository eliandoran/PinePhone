// The browser build's smoke test (`just wasm-smoke`): the page served from the build
// (python3's server), headless Chromium driven over its DevTools protocol, phone-ui used
// as someone would. Taps go through the page's mouse at the text's place (probe.c finds
// it, through Module.ccall); what happened is read from the page: the console, the
// files in Emscripten's memory, the <audio>. Any error in the console fails it.
//
//   node smoke.mjs <build directory>      (CHROMIUM: the browser, "chromium" if unset)

import { spawn } from 'node:child_process';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const BUILD = process.argv[2] || 'build';
const HTTP_PORT = 8180 + Math.floor(Math.random() * 100);
const DEBUG_PORT = 9380 + Math.floor(Math.random() * 100);
const SYMBOL_LEFT = '', SYMBOL_PLUS = '';

const sleep = ms => new Promise(r => setTimeout(r, ms));
const profile = mkdtempSync(join(tmpdir(), 'phone-ui-smoke-'));
const children = [];
const console_lines = [], errors = [];
let ws, id = 0;
let current = ''; /* the check running */
const pending = new Map();

function cleanup() {
  for (const c of children)
    c.kill();
  rmSync(profile, { recursive: true, force: true });
}

function fail(why) {
  console.error(`FAIL ${current ? current + ': ' : ''}${why}`);
  if (console_lines.length)
    console.error('  the page said:\n' + console_lines.slice(-30).map(l => '    ' + l).join('\n'));
  cleanup();
  process.exit(1);
}

function send(method, params = {}) {
  return new Promise(r => { pending.set(++id, r); ws.send(JSON.stringify({ id, method, params })); });
}

async function evaluate(expression) {
  const r = await send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
  if (r.exceptionDetails)
    throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
  return r.result.value;
}

// cond() true within ms, or the test fails saying what it waited for
async function waitFor(what, cond, ms = 10000) {
  for (const end = Date.now() + ms; Date.now() < end; await sleep(200)) {
    if (errors.length)
      fail(`while waiting for ${what}: ${errors[0]}`);
    if (await cond())
      return;
  }
  fail(`no ${what} after ${ms / 1000} s`);
}

const probe = (fn, ret, ...args) =>
  evaluate(`Module.ccall(${JSON.stringify(fn)}, ${JSON.stringify(ret)}, ${JSON.stringify(args.map(() => 'string'))}, ${JSON.stringify(args)})`);
const shows = async text => (await probe('probe_find', 'number', text)) >= 0;
const logged = text => console_lines.some(l => l.includes(text));
const files = dir => evaluate(`(() => { try { return Module.FS.readdir(${JSON.stringify(dir)}).filter(f => !f.startsWith('.')); } catch (e) { return []; } })()`);

async function open(app) {
  if (!(await probe('probe_open', 'boolean', app)))
    fail(`no app ${app}`);
}

// A tap on the text where it is shown, as a finger (the mouse) would
async function tap(text) {
  await waitFor(`"${text}" to tap`, () => shows(text));
  const at = await probe('probe_find', 'number', text);
  const r = await evaluate('JSON.parse(JSON.stringify(document.getElementById("canvas").getBoundingClientRect()))');
  const x = r.left + (at % 65536) * r.width / 720, y = r.top + Math.floor(at / 65536) * r.height / 1440;
  for (const type of ['mousePressed', 'mouseReleased']) {
    await send('Input.dispatchMouseEvent', { type, x, y, button: 'left', buttons: type === 'mousePressed' ? 1 : 0, clickCount: 1 });
    await sleep(100);
  }
  await sleep(500); /* the transition */
}

// The page and the browser

children.push(spawn('python3', ['-m', 'http.server', String(HTTP_PORT), '--bind', '127.0.0.1', '--directory', BUILD],
                    { stdio: 'ignore' }));
children.push(spawn(process.env.CHROMIUM || 'chromium', [
  '--headless=new', '--no-sandbox', `--remote-debugging-port=${DEBUG_PORT}`, `--user-data-dir=${profile}`,
  '--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--autoplay-policy=no-user-gesture-required',
  '--mute-audio', '--window-size=500,1000', 'about:blank'], { stdio: 'ignore' }));

let page;
for (let i = 0; i < 50 && !page; i++, await sleep(200)) {
  try {
    page = (await (await fetch(`http://127.0.0.1:${DEBUG_PORT}/json/list`)).json()).find(t => t.type === 'page');
  } catch (e) { /* not up yet */ }
}
if (!page)
  fail('Chromium did not start');
ws = new WebSocket(page.webSocketDebuggerUrl);
await new Promise(r => ws.addEventListener('open', r));
ws.addEventListener('message', e => {
  const m = JSON.parse(e.data);
  if (m.id && pending.has(m.id)) {
    pending.get(m.id)(m.result ?? { exceptionDetails: { text: m.error?.message } });
    pending.delete(m.id);
  }
  if (m.method === 'Runtime.consoleAPICalled') {
    const line = m.params.args.map(a => a.value ?? a.description).join(' ');
    console_lines.push(line);
    if (m.params.type === 'error')
      errors.push(line);
  }
  if (m.method === 'Runtime.exceptionThrown')
    errors.push(m.params.exceptionDetails.exception?.description ?? m.params.exceptionDetails.text);
});
await send('Runtime.enable');
await send('Page.enable');
await send('Page.navigate', { url: `http://127.0.0.1:${HTTP_PORT}/index.html?start` });

// The checks

const checks = [
  ['starts, the modem registers', async () => {
    await waitFor('phone-ui running', () => evaluate('!document.getElementById("start")'), 30000);
    await waitFor('the demo network', () => logged('modem: network: Demo Network'));
  }],
  ['a note is saved and listed', async () => {
    await open('Notes');
    await tap(SYMBOL_PLUS);
    await waitFor('the note\'s field', () => probe('probe_type', 'boolean', 'Smoke test'));
    await tap(SYMBOL_LEFT);
    await waitFor('the note in the list', () => shows('Smoke test'));
    if (!(await files('/var/lib/notes')).some(f => f.endsWith('.txt')))
      fail('no note in /var/lib/notes');
  }],
  ['emoji are fetched as drawn', async () => {
    await open('Notes');
    await tap(SYMBOL_PLUS); /* its keyboard: the emoji key */
    await waitFor('an emoji image', () => evaluate(
      '!!Module.emojiFetched && [...Module.emojiFetched].some(f => Module.FS.stat(f).size > 0)'));
    await tap(SYMBOL_LEFT);
  }],
  ['Wi-Fi connects', async () => {
    await open('Settings');
    await tap('Wi-Fi');
    await waitFor('Home connected', () => shows('Connected to Home'));
    await tap('Coffee Shop');
    await waitFor('Coffee Shop connected', () => shows('Connected to Coffee Shop'));
  }],
  ['Gallery shows the demo photos', async () => {
    await waitFor('the demo photos', async () => (await files('/var/lib/photos')).length === 6, 20000);
    await open('Gallery');
    await waitFor('six thumbnails', async () => (await probe('probe_count_textures', 'number')) === 6);
  }],
  ['Music plays', async () => {
    await waitFor('the demo songs', async () => (await files('/var/lib/music/Kevin MacLeod')).length === 3, 30000);
    await open('Music');
    await tap('Fluffing a Duck');
    await waitFor('Now Playing', () => shows('Now Playing'));
    await waitFor('the song playing', () => evaluate('!!Module.player?.audio && Module.player.audio.currentTime > 1'));
  }],
];

for (const [name, run] of checks) {
  current = name;
  try {
    await run();
  } catch (e) {
    fail(e.message);
  }
  if (errors.length)
    fail(errors[0]);
  console.log(`ok   ${name}`);
}
console.log(`${checks.length} checks, 0 failed`);
ws.close();
cleanup();
