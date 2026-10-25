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
let remoteDescSet = false, pendingIce = [];
let currentRoom = null, remoteStream = null, muted = false;
let watchTimer = null;

function req(prefix) { return `${prefix}${++reqSeq}`; }
function send(obj) { if (ws && ws.readyState === 1) ws.send(JSON.stringify(obj)); }
function lastAction(s) { $("action").textContent = s; }

$("server").value = (location.host || "localhost:8090");
/* 被监视设备由 ?dev=<device_id> 指定；页面默认不绑定任何具体设备 */
$("watchDev").value = new URLSearchParams(location.search).get("dev") || "";

$("btnConn").onclick = async () => {
  const url = `ws://${$("server").value}/ws`;
  ws = new WebSocket(url);
  ws.onopen = () => { $("status").textContent = "已连接"; send({ type: "hello", device_id: $("devid").value, request_id: req("h") }); };
  ws.onclose = () => { $("status").textContent = "离线"; cleanup(); };
  ws.onerror = (e) => log("ws error", "err");
  ws.onmessage = (ev) => onMsg(JSON.parse(ev.data));
  startWatch();
};

function onMsg(m) {
  log(`← ${JSON.stringify(m)}`);
  switch (m.type) {
    case "hello_result":
      if (m.resume_room) { log(`恢复目标 ${m.resume_room.name}，自动入房`); doJoin({ kind: "room_name", name: m.resume_room.name }); }
      break;
    case "join_result":
      if (!m.ok) { lastAction(`入房失败 ${m.error?.code || ""} ${m.error?.message || ""}`.trim()); log(`入房失败: ${m.error?.code} ${m.error?.message}`, "err"); break; }
      if (m.need_media) { lastAction("建立媒体…"); startMedia(); }
      else {
        /* 媒体复用：同一 PeerConnection 继续承载，远端流保持挂接，
           只需向服务端 confirm。旧实现在此清空 srcObject 导致切房失声。 */
        if (remoteStream && $("remote").srcObject !== remoteStream) {
          $("remote").srcObject = remoteStream;
        }
        const a = $("remote"); if (a.play) a.play().catch(() => {});
        send({ type: "confirm", request_id: req("c") });
        lastAction("切房媒体复用，已确认");
      }
      break;
    case "sdp":
      if (m.jsep === "answer") {
        pc.setRemoteDescription({ type: "answer", sdp: m.sdp }).then(() => {
          remoteDescSet = true;
          pendingIce.forEach(c => pc.addIceCandidate(c));
          pendingIce = [];
        });
      }
      break;
    case "ice":
      if (m.completed) { if (remoteDescSet) pc.addIceCandidate(null); }
      else if (m.candidate) { remoteDescSet ? pc.addIceCandidate(m.candidate) : pendingIce.push(m.candidate); }
      break;
    case "confirm_result":
      if (m.ok) {
        currentRoom = m.room;
        updateRoomPanel();
        lastAction(`已进入 ${m.room.name}`);
      } else {
        lastAction(`确认失败 ${m.error?.code || ""}`);
      }
      break;
    case "room_info_result":
      if (m.ok) log(`房间信息：${m.name}，在线 ${m.online_count}，邀请码 ${m.invite_digits}`, "state");
      break;
    case "mute_result":
      if (m.ok) { muted = m.muted; updateRoomPanel(); lastAction(m.muted ? "已静音" : "已恢复通话"); }
      break;
    case "set_mute_result":
      if (m.ok) { muted = m.muted; updateRoomPanel(); lastAction(m.muted ? "已静音" : "已恢复通话"); }
      break;
    case "notify":
      if (m.event === "summon" && m.room) { log(`被召唤进入 ${m.room.name}`); doJoin({ kind: "invite", code: m.room.invite_code }); }
      else if (m.event === "peer_joined" || m.event === "peer_left") updateRoomPanel();
      break;
    case "leave_result":
      currentRoom = null; muted = false; updateRoomPanel(); lastAction("已退出房间");
      cleanup();
      break;
  }
}

function updateRoomPanel() {
  $("room").textContent = currentRoom
    ? `${currentRoom.name}（邀请码 ${currentRoom.invite_code}，在线 ${currentRoom.online_count}）${muted ? " · 已静音" : ""}`
    : "—";
}

/* 轮询 /state 显示被监视设备（面包板）的真实房间状态 */
function startWatch() {
  if (watchTimer) clearInterval(watchTimer);
  watchTimer = setInterval(pollDeviceState, 2000);
  pollDeviceState();
}

async function pollDeviceState() {
  const id = $("watchDev").value.trim();
  if (!id) { $("devState").textContent = "—"; return; }
  try {
    const r = await fetch(`http://${$("server").value}/state?device_id=${encodeURIComponent(id)}`);
    const s = await r.json();
    if (!s.connected) { $("devState").textContent = `${id} 离线`; return; }
    // invite_code 只对带管理员令牌的 /state 请求返回；缺省时面板照常显示其余字段
    const room = s.room
      ? `${s.room.name}（${s.room.invite_code ? `邀请码 ${s.room.invite_code}，` : ""}在线${s.room.online_count}）`
      : "—";
    $("devState").textContent = `${s.mode === "intercom" ? "对讲" : "灵矽"} · ${room} · ${s.muted ? "已静音" : "未静音"}`;
  } catch (e) {
    $("devState").textContent = "查询失败";
  }
}
$("watchDev").onchange = pollDeviceState;

async function getAudio() {
  if ($("fakeAudio").checked) {
    const ctx = new AudioContext();
    const osc = ctx.createOscillator(); osc.frequency.value = 440;
    const gain = ctx.createGain(); gain.gain.value = 0.15;
    const dst = ctx.createMediaStreamDestination();
    osc.connect(gain); gain.connect(dst); osc.start();
    return dst.stream;
  }
  if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
    throw new Error("非安全上下文，麦克风 API 不可用，请改用 localhost 隧道入口（见使用说明），或勾选正弦波");
  }
  return navigator.mediaDevices.getUserMedia({ audio: { echoCancellation: true, noiseSuppression: true } });
}

async function startMedia() {
  cleanup();
  pc = new RTCPeerConnection({ iceServers: [] });
  try {
    localStream = await getAudio();
  } catch (e) {
    const msg = e && e.name === "NotAllowedError"
      ? "麦克风权限被拒绝：请在浏览器中允许麦克风后重新入房，或勾选“用正弦波代替麦克风”"
      : `无法打开麦克风：${e && e.message ? e.message : e}`;
    lastAction(msg);
    log(msg, "err");
    cleanup();
    return;
  }
  localStream.getTracks().forEach(t => pc.addTrack(t, localStream));
  pc.ontrack = (ev) => { remoteStream = ev.streams[0]; $("remote").srcObject = remoteStream; };
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

function cleanup() {
  mediaReady = false;
  remoteDescSet = false;
  pendingIce = [];
  remoteStream = null;
  if (localStream) { localStream.getTracks().forEach(t => t.stop()); localStream = null; }
  if (pc) { pc.close(); pc = null; }
  const a = $("remote"); if (a.srcObject) { a.srcObject = null; }
}

function doJoin(target) { send({ type: "join", request_id: req("j"), target }); }

$("btnJoin").onclick = () => {
  const kind = $("kind").value, v = $("target").value.trim();
  const t = { kind };
  if (kind === "invite") t.code = v;
  else if (kind === "nfc") t.entry_id = v;
  else if (kind === "room_name") t.name = v;
  else if (kind === "device") t.device_id = v;
  lastAction("已发送入房请求");
  doJoin(t);
};
$("btnLeave").onclick = () => { lastAction("已发送退出请求"); send({ type: "leave", request_id: req("l") }); };
$("btnInfo").onclick = () => send({ type: "room_info", request_id: req("i") });
$("btnMute").onclick = () => send({ type: "set_mute", request_id: req("m"), muted: true });
$("btnUnmute").onclick = () => send({ type: "set_mute", request_id: req("m"), muted: false });
