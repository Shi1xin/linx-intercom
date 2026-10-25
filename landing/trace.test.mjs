// landing trace replay 回归：playTrace 代次守卫
// 验证双击、播放中途点击、自动启动先到/后到都只产出单一完整播放
// 运行：node --test landing/trace.test.mjs
import { test } from "node:test";
import assert from "node:assert";
import { readFileSync } from "node:fs";
import vm from "node:vm";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const dir = dirname(fileURLToPath(import.meta.url));
const html = readFileSync(join(dir, "index.html"), "utf8");
const script = html.match(/<script>([\s\S]*)<\/script>/)[1];

function fakeEl() {
  const el = {
    children: [],
    textContent: "", className: "", disabled: false,
    style: {},
    classList: { add() {}, toggle() {}, contains: () => false },
    appendChild(c) { this.children.push(c); },
    getBoundingClientRect: () => ({ top: 0, bottom: 100, left: 0, right: 100, height: 100, width: 100 }),
    addEventListener() {},
    dataset: {},
  };
  Object.defineProperty(el, "innerHTML", {
    get() { return this._html ?? ""; },
    set(v) { this._html = v; if (!v) this.children.length = 0; },
  });
  return el;
}

function load() {
  const stream = fakeEl();
  const foot = fakeEl();
  const handlers = {};      // element-level click handlers
  const winHandlers = {};   // window-level handlers (load etc.)
  const timers = [];
  const replay = fakeEl();
  replay.addEventListener = (ev, fn) => { handlers[ev] = fn; };
  const reel = fakeEl(); reel.children.push(fakeEl(), fakeEl(), fakeEl());

  const ids = { traceStream: stream, traceFoot: foot, replay, reel };
  const sandbox = {
    console,
    document: {
      getElementById: (id) => ids[id] ?? (ids[id] = fakeEl()),
      createElement: () => fakeEl(),
      querySelector: () => fakeEl(),
      querySelectorAll: () => [],
    },
    window: { addEventListener: (ev, fn) => { winHandlers[ev] = fn; } },
    addEventListener() {},
    innerHeight: 900,
    IntersectionObserver: class { observe() {} unobserve() {} disconnect() {} },
    setTimeout: (fn) => { timers.push(fn); return timers.length; },
    setInterval: () => 0,
    clearInterval() {},
    Date,
    Math,
    location: { search: "" },
  };
  vm.createContext(sandbox);
  vm.runInContext(script, sandbox);

  const step = (n = 1) => { while (n-- > 0 && timers.length) timers.shift()(); };
  const drain = () => { while (timers.length) timers.shift()(); };
  return { stream, foot, handlers, winHandlers, drain, step };
}

const EXPECTED_ROWS = 13; // 3 个 lane 行 + 3+5+2 条消息

test("single autoplay produces exactly one full trace", () => {
  const c = load();
  c.winHandlers.load(); c.drain();
  assert.equal(c.stream.children.length, EXPECTED_ROWS);
});

test("double click does not interleave two replays", () => {
  const c = load();
  c.handlers.click(); c.handlers.click(); c.drain();
  assert.equal(c.stream.children.length, EXPECTED_ROWS);
});

test("click mid-replay restarts cleanly, one run remains", () => {
  const c = load();
  c.handlers.click();
  c.step(4);                       // 首轮同步 1 行 + 4 个 tick = 5 行
  const partial = c.stream.children.length;
  assert.equal(partial, 5);
  c.handlers.click(); c.drain();
  assert.equal(c.stream.children.length, EXPECTED_ROWS);
});

test("click before load-autostart still ends with one trace", () => {
  const c = load();
  c.handlers.click();
  c.step(3);
  c.winHandlers.load(); c.drain();  // 自动启动到达：最新一轮胜出
  assert.equal(c.stream.children.length, EXPECTED_ROWS);
});
