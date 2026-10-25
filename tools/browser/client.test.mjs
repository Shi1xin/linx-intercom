/* Regression test for tools/browser/client.js.
 *
 * Covers the room-switch playback bug: join_result with need_media=false
 * reuses the PeerConnection, so the <audio> element must keep its
 * srcObject; the old code cleared it and playback went silent because no
 * new ontrack event ever arrives on the same PeerConnection.
 *
 * Run: node --test tools/browser/client.test.mjs
 */
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import vm from "node:vm";

const src = readFileSync(
  join(dirname(fileURLToPath(import.meta.url)), "client.js"), "utf8");

function makeEl() {
  return { value: "", textContent: "", checked: false, srcObject: null,
           onclick: null, onchange: null, play: () => Promise.resolve(),
           appendChild() {}, scrollTop: 0, scrollHeight: 0 };
}

function loadClient(navOverride, fetchOverride) {
  const els = new Map();
  const sent = [];
  const remoteStream = { id: "remote-1" };
  const local = { getTracks: () => [{ stop() {} }] };
  let wsInst = null, pcInst = null;

  const document = {
    getElementById: (id) => {
      if (!els.has(id)) els.set(id, makeEl());
      return els.get(id);
    },
    createElement: () => makeEl(),
  };
  class WebSocket {
    constructor() { wsInst = this; this.readyState = 1; }
    send(s) { sent.push(JSON.parse(s)); }
  }
  class RTCPeerConnection {
    constructor() { pcInst = this; this.connectionState = "new"; }
    addTrack() {}
    createOffer() { return Promise.resolve({ sdp: "offer-sdp" }); }
    setLocalDescription(d) { this.localDescription = d; return Promise.resolve(); }
    setRemoteDescription() { return Promise.resolve(); }
    addIceCandidate() { return Promise.resolve(); }
    close() {}
  }
  const sandbox = {
    document, WebSocket, RTCPeerConnection,
    location: { host: "test:8090", search: "" },
    URLSearchParams,
    navigator: navOverride ?? { mediaDevices: { getUserMedia: () => Promise.resolve(local) } },
    AudioContext: class {},
    fetch: fetchOverride ?? (() => Promise.resolve({ json: () => Promise.resolve({ connected: false }) })),
    setInterval: () => 1,
    clearInterval: () => {},
    Promise,
  };
  vm.createContext(sandbox);
  vm.runInContext(src, sandbox);
  return {
    els, sent, remoteStream,
    el: (id) => document.getElementById(id),
    ws: () => wsInst, pc: () => pcInst,
    deliver: (m) => wsInst.onmessage({ data: JSON.stringify(m) }),
  };
}

const tick = () => new Promise((r) => setImmediate(r));

test("switch with need_media=false keeps remote stream and confirms", async () => {
  const { el, sent, remoteStream, ws, pc, deliver } = loadClient();
  const remote = el("remote");
  const confirms = () => sent.filter((m) => m.type === "confirm").length;

  el("btnConn").onclick();
  ws().onopen();
  assert.equal(sent.at(-1).type, "hello");

  // first join: full media path
  el("kind").value = "invite";
  el("target").value = "468911";
  el("btnJoin").onclick();
  deliver({ type: "join_result", ok: true, need_media: true });
  await tick(); await tick();
  assert.ok(pc(), "PeerConnection created");
  assert.equal(lastType(sent), "sdp");

  pc().ontrack({ streams: [remoteStream] });
  assert.equal(remote.srcObject, remoteStream);

  pc().connectionState = "connected";
  pc().onconnectionstatechange();
  assert.equal(confirms(), 1);
  deliver({ type: "confirm_result", ok: true,
            room: { name: "二号房", invite_code: "468911", online_count: 1 } });
  assert.match(el("room").textContent, /二号房/);

  // switch: server reuses the PeerConnection (need_media=false)
  el("target").value = "286509";
  el("btnJoin").onclick();
  deliver({ type: "join_result", ok: true, need_media: false });
  await tick();
  assert.equal(remote.srcObject, remoteStream,
               "remote stream must stay attached on media reuse");
  assert.equal(confirms(), 2, "switch must be confirmed");
  assert.equal(pc().connectionState, "connected");

  deliver({ type: "confirm_result", ok: true,
            room: { name: "三号房", invite_code: "286509", online_count: 2 } });
  assert.match(el("room").textContent, /三号房/);

  // a stale-cleared srcObject self-heals on the next reuse
  remote.srcObject = null;
  el("btnJoin").onclick();
  deliver({ type: "join_result", ok: true, need_media: false });
  assert.equal(remote.srcObject, remoteStream, "reattaches saved stream");

  // leave releases everything
  deliver({ type: "leave_result", ok: true });
  assert.equal(remote.srcObject, null);
  assert.equal(el("room").textContent, "—");
});

test("join failure reports the error as last action", () => {
  const { el, deliver, ws } = loadClient();
  el("btnConn").onclick();
  ws().onopen();
  deliver({ type: "join_result", ok: false,
            error: { code: "no_such_room", message: "邀请码无效" } });
  assert.match(el("action").textContent, /入房失败/);
  assert.match(el("action").textContent, /no_such_room/);
});

test("mic denial surfaces a readable error and sends no sdp", async () => {
  const denied = Object.assign(new Error("denied"), { name: "NotAllowedError" });
  const c = loadClient({ mediaDevices: { getUserMedia: () => Promise.reject(denied) } });
  c.el("btnConn").onclick();
  c.ws().onopen();
  c.deliver({ type: "join_result", ok: true, need_media: true });
  await tick(); await tick();
  assert.match(c.el("action").textContent, /权限被拒绝/);
  assert.equal(c.sent.at(-1).type, "hello", "no media offer after denial");
});

test("insecure context points the user at the localhost entry", async () => {
  const c = loadClient({});           // mediaDevices absent, like http://IP:8090
  c.el("btnConn").onclick();
  c.ws().onopen();
  c.deliver({ type: "join_result", ok: true, need_media: true });
  await tick(); await tick();
  assert.match(c.el("action").textContent, /localhost/);
});

test("state panel renders a room whose invite_code is admin-gated", async () => {
  const gated = () => Promise.resolve({ json: () => Promise.resolve({
    connected: true, mode: "intercom", muted: false,
    room: { name: "一号房", online_count: 2 },  // no invite_code field
  }) });
  const c = loadClient(undefined, gated);
  c.el("watchDev").value = "dev-s";
  c.el("watchDev").onchange();
  await tick(); await tick(); await tick();
  const text = c.el("devState").textContent;
  assert.match(text, /一号房/);
  assert.match(text, /在线2/);
  assert.doesNotMatch(text, /undefined/);
});

function lastType(sent) { return sent.at(-1).type; }
