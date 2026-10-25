# 控制协议

设备与浏览器测试端通过一条 WebSocket JSON 文本通道连接房间控制服务。服务端维护 Janus 会话并转发 SDP/ICE，终端只需实现本文消息与标准 WebRTC 媒体栈。

连接：`ws(s)://<host>:<port>/ws`，连接后首条消息必须是 `hello`。

## 通用约定

- `request_id`：发起方生成的单调字符串，服务端响应原样带回；终端只应用当前操作的响应。带同一 `request_id` 重发 `join` 会命中服务端的去重缓存，返回首次结果
- `device_id`：设备固定 MAC（小写冒号形式）；浏览器测试端使用 `browser-*` 身份
- 错误响应统一为 `{"type":"error","request_id":"…","error":{"code":"…","message":"…"}}`

## 消息

### 握手

```json
→ {"type":"hello","device_id":"98:a3:16:cd:05:ac","token":"dev","request_id":"r1"}
← {"type":"hello_result","request_id":"r1","ok":true,"muted":false,
   "resume_room":{"id":3,"name":"三号房","invite_code":"314159"}|null}
```

`token` 在设备首次 `hello` 时写入服务端记录，之后的 `hello` 必须携带同一 token；`auth_failed` 响应后服务端主动断开连接，终端重连后重试。

### 入房 / 切房（同一消息，服务端按当前状态执行 join 或 changeroom）

```json
→ {"type":"join","request_id":"r2","target":<target>}
```

`target` 为以下五种形式之一：

```json
{"kind":"nfc","entry_id":"<标签UID>"}
{"kind":"device","device_id":"<目标设备MAC 或登记的标签UID>"}
{"kind":"invite","code":"314159"}
{"kind":"room_name","name":"三号房"}
{"kind":"new"}
```

`{"kind":"new"}` 让服务端新建一个房间并把本设备放进去；已在房内时等同切到新房。响应的 `room` 含新房名与邀请码。

```json
← {"type":"join_result","request_id":"r2","ok":true,"need_media":true,
   "room":{"id":3,"name":"三号房","invite_code":"314159","online_count":2}}
```

- `need_media:true`：首次入房或媒体会话不存在，终端建立 PeerConnection 后发 `sdp`
- `need_media:false`：复用现有媒体连接的切房；终端清理下行缓冲后回 `confirm`
- 失败：`{"type":"join_result","ok":false,"error":{"code":"invalid_invite|room_not_joined|target_offline|media_failed","message":"…"}}`，终端保持原会话

### SDP / ICE 转发

```json
→ {"type":"sdp","request_id":"r3","sdp":"<offer sdp>"}
← {"type":"sdp","request_id":"r3","jsep":"answer","sdp":"<answer sdp>"}
→ {"type":"ice","candidate":{"candidate":"…","sdpMid":"0","sdpMLineIndex":0}}
→ {"type":"ice","completed":true}
← {"type":"ice","candidate":{…}} / {"type":"ice","completed":true}
```

`ice` 必须携带 `candidate` 或 `completed:true` 之一，两者皆空返回 `bad_request`。

### 媒体就绪确认

```json
→ {"type":"confirm","request_id":"r4"}
← {"type":"confirm_result","request_id":"r4","ok":true,
   "room":{"id":3,"name":"三号房","invite_code":"314159","online_count":2}}
```

服务端此时把房间写入设备的恢复目标与已加入记录。

### 房间信息

```json
→ {"type":"room_info","request_id":"r5"}
← {"type":"room_info_result","request_id":"r5","ok":true,"name":"三号房",
   "online_count":2,"invite_code":"314159","invite_digits":"3 1 4 1 5 9",
   "members":["98:a3:16:cd:05:ac","browser-test-1"]}
```

### 静音

```json
→ {"type":"set_mute","request_id":"r6","muted":true}
← {"type":"mute_result","request_id":"r6","ok":true,"muted":true}
```

静音是持久用户设置；切房后服务端在 Janus 上恢复同一设置。

### 退出

```json
→ {"type":"leave","request_id":"r7"}
← {"type":"leave_result","request_id":"r7","ok":true}
```

服务端清除恢复目标、Janus 参与关系和活动会话房间关联；已加入记录保留。

### 服务端主动通知

```json
← {"type":"notify","event":"peer_joined|peer_left|summon|media_lost","room":{…},"device_id":"…"}
```

`summon`：设备标签组队时通知目标设备建房入房，目标设备按 `join` 流程建立媒体后 `confirm`。
`media_lost`：Janus 侧挂起媒体（keepalive 超时等）时服务端清除该会话的媒体状态并下发；终端按恢复目标重新执行 `join` 流程。

## 状态对应

| 终端状态 | 协议表现 |
| --- | --- |
| 待机 | 已 `hello`，无活动房间 |
| 入房 | `join` → `join_result(need_media)` → `sdp`/ICE → `confirm` |
| 通话 | 媒体收发中 |
| 切房 | `join` → `join_result(need_media:false)` → `confirm` |
| 恢复 | 断线重连后 `hello`，`resume_room` 非空则自动 `join` 该房间 |
| 退出 | `leave` 后回到待机 |

## 服务端主动操作（云端 MCP / 管理面）

云端 MCP 调用与 `/admin/op` 统一经 `op` 帧下投，设备执行路径与本地入房一致，结果经 `op_result` 回报。

```json
← {"type":"op","op_id":"op1","op":"join","op_arg":"468911"}
← {"type":"op","op_id":"op2","op":"switch","op_arg":"三号房"}
← {"type":"op","op_id":"op3","op":"set_mute","mute":true}
← {"type":"op","op_id":"op4","op":"status"}
← {"type":"op","op_id":"op5","op":"leave"}

→ {"type":"op_result","op_id":"op1","ok":true,
   "message":"{\"ok\":true,\"mode\":\"intercom\",\"room\":\"二号房\",\"invite_code\":\"468911\",\"online_count\":1,\"muted\":false}"}
```

`op` 字段取值 `join`（`op_arg`=邀请码）、`switch`（`op_arg`=房间名）、`create`（无 `op_arg`，新建房间并入房，`message` 内含 `invite_code` 与逐字中文数字 `invite_spoken`）、`status`、`set_mute`（`mute`=布尔）、`leave`、`nfc`（`op_arg` 为标签 UID，用于在无物理读卡器时复现 NFC 入房路径）。设备侧对 join/switch/create/nfc 按正常流程建立媒体并 `confirm`，`op_result` 在媒体确认后才发出；失败时 `ok:false`，`message` 内含 `error`/`message` 字段。`set_mute` 沿用持久化语义。

云端入口：

- `GET /mcp`（SSE）+ `POST /mcp/message?sessionId=`：标准 MCP 端点，`intercom.join/switch/create/status/set_mute/leave` 六个工具；目标设备按 `arguments.device_id` → `X-Linx-Device-Id` 头 → `-mcp-target` 默认值的顺序解析；`-mcp-token` 非空时要求 `Authorization: Bearer <token>`
- `POST /admin/op`：`{"device_id","op","arg","muted"}` → 同步返回设备 `op_result`；受 `-admin-token` 保护
- `tools/mcpbridge`：连出平台 `/v1/mcpproxy/connect?token=…`（官方反向通道），JSON-RPC 译为 `/admin/op`

HTTP 只读端点：

- `GET /healthz`：存活探针
- `GET /state?device_id=<id>`：设备实时状态 `{"connected","mode","muted","room":{…}}`；`invite_code` 仅在请求携带 `-admin-token` 匹配的 `X-Admin-Token` 头时返回（未配置 admin token 的本地部署直接返回）
- `GET /admin/rooms`：房间列表（含邀请码与在线数）；受 `-admin-token` 保护
- `POST /admin/register-entry`：`{"entry_id","device_id"}` 登记设备标签绑定；受 `-admin-token` 保护
