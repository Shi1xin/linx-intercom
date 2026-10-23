/* 灵矽同频浏览器测试端 — 实现 docs/control-protocol.md */
const $ = (id) => document.getElementById(id);
const log = (s, cls) => {
  const el = $("log");
  const line = document.createElement("div");
  if (cls) line.className = cls;
  line.textContent = s;
  el.appendChild(line);
  el.scrollTop = el.scrollHeight;
};

let ws = null, pc = null, localStream = null, reqSeq = 0, mediaReady = false;
let currentRoom = null;

function req(prefix) { return `${prefix}${++reqSeq}`; }
function send(obj) { if (ws && ws.readyState === 1) ws.send(JSON.stringify(obj)); }

$("server").value = (location.host || "localhost:8090");

$("btnConn").onclick = async () => {
  const url = `ws://${$("server").value}/ws`;
  ws = new WebSocket(url);
  ws.onopen = () => { $("status").textContent = "已连接"; send({ type: "hello", device_id: $("devid").value, request_id: req("h") }); };
  ws.onclose = () => { $("status").textContent = "离线"; cleanup(); };
  ws.onerror = (e) => log("ws error", "err");
  ws.onmessage = (ev) => onMsg(JSON.parse(ev.data));
};

function onMsg(m) {
  log(`← ${JSON.stringify(m)}`);
  switch (m.type) {
    case "hello_result":
      if (m.resume_room) { log(`恢复目标 ${m.resume_room.name}，自动入房`); doJoin({ kind: "room_name", name: m.resume_room.name }); }
      break;
    case "join_result":
      if (!m.ok) { log(`入房失败: ${m.error?.code} ${m.error?.message}`, "err"); break; }
      if (m.need_media) startMedia(); else { clearPlayback(); send({ type: "confirm", request_id: req("c") }); }
      break;
    case "sdp":
      if (m.jsep === "answer") pc.setRemoteDescription({ type: "answer", sdp: m.sdp });
      break;
    case "ice":
      if (m.completed) pc.addIceCandidate(null);
      else if (m.candidate) pc.addIceCandidate(m.candidate);
      break;
    case "confirm_result":
      if (m.ok) { currentRoom = m.room; $("room").textContent = `${m.room.name}（邀请码 ${m.room.invite_code}，在线 ${m.room.online_count}）`; }
      break;
    case "room_info_result":
      if (m.ok) log(`房间信息：${m.name}，在线 ${m.online_count}，邀请码 ${m.invite_digits}`, "state");
      break;
    case "notify":
      if (m.event === "summon" && m.room) { log(`被召唤进入 ${m.room.name}`); doJoin({ kind: "room_name", name: m.room.name }); }
      break;
    case "leave_result":
      currentRoom = null; $("room").textContent = "—"; cleanup();
      break;
  }
}

async function getAudio() {
  if ($("fakeAudio").checked) {
    const ctx = new AudioContext();
    const osc = ctx.createOscillator(); osc.frequency.value = 440;
    const gain = ctx.createGain(); gain.gain.value = 0.15;
    const dst = ctx.createMediaStreamDestination();
    osc.connect(gain); gain.connect(dst); osc.start();
    return dst.stream;
  }
  return navigator.mediaDevices.getUserMedia({ audio: { echoCancellation: true, noiseSuppression: true } });
}

async function startMedia() {
  cleanup();
  pc = new RTCPeerConnection({ iceServers: [] });
  localStream = await getAudio();
  localStream.getTracks().forEach(t => pc.addTrack(t, localStream));
  pc.ontrack = (ev) => { $("remote").srcObject = ev.streams[0]; };
  pc.onicecandidate = (ev) => {
    if (ev.candidate) send({ type: "ice", candidate: ev.candidate.toJSON() });
    else send({ type: "ice", completed: true });
  };
  pc.onconnectionstatechange = () => {
    if (pc.connectionState === "connected" && !mediaReady) {
      mediaReady = true;
      send({ type: "confirm", request_id: req("c") });
    }
  };
  const offer = await pc.createOffer({ offerToReceiveAudio: true });
  await pc.setLocalDescription(offer);
  send({ type: "sdp", request_id: req("s"), sdp: pc.localDescription.sdp });
}

function clearPlayback() { const a = $("remote"); if (a.srcObject) { a.srcObject = null; } }

function cleanup() {
  mediaReady = false;
  if (localStream) { localStream.getTracks().forEach(t => t.stop()); localStream = null; }
  if (pc) { pc.close(); pc = null; }
  clearPlayback();
}

function doJoin(target) { send({ type: "join", request_id: req("j"), target }); }

$("btnJoin").onclick = () => {
  const kind = $("kind").value, v = $("target").value.trim();
  const t = { kind };
  if (kind === "invite") t.code = v;
  else if (kind === "nfc") t.entry_id = v;
  else if (kind === "room_name") t.name = v;
  else if (kind === "device") t.device_id = v;
  doJoin(t);
};
$("btnLeave").onclick = () => send({ type: "leave", request_id: req("l") });
$("btnInfo").onclick = () => send({ type: "room_info", request_id: req("i") });
$("btnMute").onclick = () => send({ type: "set_mute", request_id: req("m"), muted: true });
$("btnUnmute").onclick = () => send({ type: "set_mute", request_id: req("m"), muted: false });
