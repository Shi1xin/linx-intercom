# 使用说明

本文分两条路径：使用者操作设备与第二端，开发者构建运行整套系统。设备与房间关系见 [产品设计](product-design.md)，消息格式见 [控制协议](control-protocol.md)。

## 使用者

### 设备操作

设备平时是灵矽智能体：唤醒词「你好小智」或按住命令键（BOOT）对它说话，松开结束收音，智能体语音回答。进入对讲后设备切换到房间模式，通话与房间操作并行。

| 操作 | 做法 | 结果 |
| --- | --- | --- |
| 加入房间 | 对设备说「加入对讲房间，邀请码 + 逐位数字」 | 智能体调用工具入房，语音报出房间名、邀请码与在线人数；设备进入对讲模式 |
| 查询房间 | 说「我在哪个房间」「房间邀请码是什么」 | 回答当前模式、房间名、邀请码、静音态、在线人数 |
| 切换房间 | 说「切换到 + 房间名」（限已加入过的房间） | 进入目标房间，静音设置保留；失败回到原房间 |
| 静音 / 恢复 | 双击命令键，或说「静音」「恢复通话」 | 静音时停止房间上行、保持收听；静音态在切房与断线恢复后保持 |
| 退出房间 | 说「退出对讲房间」 | 结束对讲，设备回到灵矽对话 |
| 按键说话 | 房间内按住命令键说话，松开结束 | 语音送往智能体处理，房间成员听不到命令内容 |
| 音量 | 板载音量加/减键 | 每次 ±10，掉电保存 |

命令键的操作约定：按住说话、说完松开；双击切换静音；单击无独立操作。复位键重启设备，重启后按恢复目标自动回房。

### 响应语义

- 每次操作后语音回复与真实结果一致：成功报出实际进入的房间名与邀请码；失败明确说明操作未完成及原因（邀请码无效、未加入过该房间、目标设备离线）。
- 云端或 NFC 触发的入房/切房/退出，设备本地播报结果（「已进入二号房」一类）；设备自己语音触发的操作由智能体回答，本地不再重复播报。
- 唤醒词在灵矽模式与房间模式都可用；命令键按住期间房间上行暂停、收听持续。

### 浏览器第二端与状态面板

控制服务内置静态页面 `http://<server>:8090/`：连接后可作为房间对端收发语音、执行入房/切房/静音/退出，页面显示本端房间与最近动作。`?dev=<设备MAC>` 参数使第二行实时显示目标设备的模式、房间、邀请码与静音态。

真麦克风需要安全上下文：`http://localhost:8090/`（SSH 隧道）或 HTTPS。直连局域网地址时浏览器不提供 `mediaDevices`，页面会给出中文错误提示；勾「用正弦波代替麦克风」只验证信令链路。

只读状态接口：`GET /state?device_id=<MAC>` 返回在线设备的模式、房间、在线人数与静音态；邀请码字段仅在携带匹配的 `X-Admin-Token` 头时返回。

## 开发者

### 组成

| 组件 | 位置 | 说明 |
| --- | --- | --- |
| 控制服务 | `server/` | Go 1.26+；房间业务、SQLite 持久化、Janus 信令适配、`/admin/op`、`/state`、云端 MCP `/mcp` |
| 固件 | `firmware/intercom/` | ESP-IDF 5.5 + esp-webrtc/esp-sr，ESP32-S3 |
| 浏览器端 | `tools/browser/` | 静态页 + `client.test.mjs` 协议/切房回归 |
| 软设备 | `tools/softdev/` | 固件同构的软件设备：持灵矽会话 + 控制会话，脚本化驱动真实入房 |
| 协议探针 | `tools/wsprobe/` | 控制协议 + WebRTC 探针，可向房间发真实 Opus 流并录制下行 |
| MCP 桥 | `tools/mcpbridge/` | 平台 mcpproxy 反向通道 → `/admin/op` 翻译 |
| TURN | `tools/turnsrv/` | pion/turn 中继 |
| 播报片段 | `tools/announce/gen_clips.py` | 生成固件本地播报的 Opus 片段表 |
| 部署 | `infra/` | `testenv/`（Janus 测试环境）、`deploy/`（正式部署样例） |

### 控制服务

从仓库根启动：`/` 路径伺服 `tools/browser` 页面，工作目录必须是仓库根。

```bash
cd server && go build -o bin/intercom-server ./cmd/intercom-server && cd ..
./server/bin/intercom-server \
  -addr :8090 \
  -janus ws://127.0.0.1:8188 \
  -db intercom.db \
  -device-token dev \
  -admin-token <secret> \
  -mcp-target <默认目标设备MAC>
```

| 参数 | 说明 |
| --- | --- |
| `-janus` | Janus WebSocket；缺省时使用内置假媒体，仅协议测试可用 |
| `-device-token` | 设备 `hello` 携带的令牌 |
| `-admin-token` | `/admin/*` 管理接口的 `X-Admin-Token` 校验 |
| `-mcp` / `-mcp-target` / `-mcp-token` | 云端 MCP（SSE）开关、缺省目标设备、共享密钥 |

管理接口：`POST /admin/register-entry` 登记设备标签，`GET /admin/rooms` 房间列表，`POST /admin/op {"device_id","op","arg","muted"}` 向在线设备下投操作（`op` = `join` 邀请码 / `switch` 房间名 / `status` / `set_mute` / `leave` / `nfc` 标签 UID），同步返回设备 `op_result`。

### 固件

```bash
cd firmware/intercom
idf.py set-target esp32s3
idf.py menuconfig        # Intercom 菜单：Wi-Fi / 控制服务 / 灵矽身份 / 自检
idf.py -p <串口> build flash monitor
```

关键配置（`main/Kconfig.projbuild`，本机值写 `sdkconfig`，勿提交）：

| 配置 | 说明 |
| --- | --- |
| `INTERCOM_WIFI_SSID/PASSWORD` | 设备网络 |
| `INTERCOM_CTRL_URL` / `INTERCOM_DEVICE_TOKEN` | 控制服务地址与令牌 |
| `INTERCOM_TURN_*` | 媒体中继 |
| `INTERCOM_LINX_*` | 灵矽 OTA 地址、`DEVICE_ID`/`CLIENT_ID`（虚拟身份）、`BOARD_TYPE` |
| `INTERCOM_LINX_SELFTEST_TEXT` | 非空时每次平台会话建立自动发 `listen.detect`，串口可见 hello→stt→tts→stop 全链；置空关闭 |
| `INTERCOM_SPEAKER_ENABLE` | 置 n 时静音全部播放（电信号链路照常），用于无声 bench 验证 |
| `INTERCOM_WAKE_SELFTEST` / `INTERCOM_WAKE_PCM_TRACE` | 唤醒链注入与 PCM 导出，bench 调试 |

验证口径：烧录后串口出现 `linx session up` → `tts frame` → `tts stop`，期间无 `rx underrun`/`send failed`，即会话→智能体→TTS→播放链路正常。

### 工具与注入测试

| 工具 | 用法 |
| --- | --- |
| softdev | `softdev -ota ota.json -device-id <id> -client-id <id> -ctrl ws://<server>:8090/ws -script <脚本>`；替代固件跑平台会话与真实房间操作 |
| wsprobe | 控制协议 + WebRTC 探针：入房、向房间发送 ogg/Opus、计数与录制下行 |
| mcpbridge | `mcpbridge -endpoint <平台mcpproxy-wss带token> -server http://<server>:8090 -target <设备MAC> [-admin-token <t>]` 把平台反向通道上的 `intercom.*` 调用译成 `/admin/op` |
| browser | `tools/browser/index.html`，`?dev=` 监视设备；`node --test tools/browser/client.test.mjs` 跑协议回归 |
| gen_clips | `python3 tools/announce/gen_clips.py` 重新生成 `firmware/intercom/main/announce_clips.h`（macOS `say` + opusenc） |

注入式测试路径：平台 `SELFTEST_TEXT` 文本注入覆盖语音链路；`/admin/op` 覆盖 NFC/云端接管路径；`softdev -script` 覆盖设备智能体会话。三者分别落在真实执行代码上，可在无硬件、无语音环境下回归。

### 测试命令

```bash
cd server && go test ./internal/server/...          # 控制服务
node --test tools/browser/client.test.mjs          # 浏览器端协议回归
cd firmware/intercom && python3 -m unittest discover tests  # 固件宿主测试（真实 C 桩）
```

## 演示脚本

当前演示环境（本机局域网）：控制服务 `http://<server>:8090`，设备经 `?dev=<MAC>` 面板监视。

| 房间 | 邀请码 |
| --- | --- |
| 一号房 | 723827 |
| 二号房 | 468911 |
| 三号房 | 286509 |

推荐演示顺序：唤醒词或按住命令键自我介绍 → 「加入对讲房间，邀请码四六八九一一」→ 查询房间 → 双击静音再恢复 → 「切换到三号房」→ 从第二端或云端接管切回 → 「退出对讲房间」。云端接管：`POST /admin/op` 下投 `{"op":"join","arg":"468911"}`，设备本地播报房间名；或在另一灵矽会话中调用 `intercom.*` 工具。
