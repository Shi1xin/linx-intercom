# 控制协议

设备与浏览器测试端通过一条 WebSocket JSON 文本通道连接房间控制服务。服务端维护 Janus 会话并转发 SDP/ICE，终端只需实现本文消息与标准 WebRTC 媒体栈。

连接：`ws(s)://<host>:<port>/ws`，连接后首条消息必须是 `hello`。

## 通用约定

- `request_id`：发起方生成的单调字符串，服务端响应原样带回；终端只应用当前操作的响应
- `device_id`：设备固定 MAC（小写冒号形式）；浏览器测试端使用 `browser-*` 身份
- 错误响应统一为 `{"type":"error","request_id":"…","code":"…","message":"…"}`

## 消息

### 握手

```json
→ {"type":"hello","device_id":"98:a3:16:cd:05:ac","token":"dev","request_id":"r1"}
← {"type":"hello_result","request_id":"r1","ok":true,"muted":false,
   "resume_room":{"id":3,"name":"三号房","invite_code":"314159"}|null}
```

### 入房 / 切房（同一消息，服务端按当前状态执行 join 或 changeroom）

```json
→ {"type":"join","request_id":"r2","target":<target>}
```

`target` 四选一：

```json
{"kind":"nfc","entry_id":"<标签UID>"}
{"kind":"device","device_id":"<目标设备MAC 或登记的标签UID>"}
{"kind":"invite","code":"314159"}
{"kind":"room_name","name":"三号房"}
```

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
← {"type":"notify","event":"peer_joined|peer_left|summon","room":{…},"device_id":"…"}
```

`summon`：设备标签组队时通知目标设备建房入房，目标设备按 `join` 流程建立媒体后 `confirm`。

## 状态对应

| 终端状态 | 协议表现 |
| --- | --- |
| 待机 | 已 `hello`，无活动房间 |
| 入房 | `join` → `join_result(need_media)` → `sdp`/ICE → `confirm` |
| 通话 | 媒体收发中 |
| 切房 | `join` → `join_result(need_media:false)` → `confirm` |
| 恢复 | 断线重连后 `hello`，`resume_room` 非空则自动 `join` 该房间 |
| 退出 | `leave` 后回到待机 |
