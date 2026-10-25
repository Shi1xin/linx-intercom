# 使用说明

本文分两部分：设备与第二端的使用者操作、整套系统的开发者构建与测试。设备与房间关系见 [产品设计](product-design.md)，消息格式见 [控制协议](control-protocol.md)。

## 使用者操作

### 设备操作

设备默认处于灵矽智能体模式：唤醒词「你好小智」或按住命令键（BOOT）对它说话，松开结束收音，智能体语音回答。进入对讲后设备切换到房间模式，通话与房间操作并行。

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
- 云端或 NFC 触发的入房/切房/退出，设备本地播报结果（「已进入二号房」一类）；设备自己语音触发的操作由智能体回答，本地播报此时让位。
- 唤醒词在灵矽模式与房间模式都可用；命令键按住期间房间上行暂停、收听持续。

### 浏览器第二端与状态面板

控制服务内置静态页面 `http://<server>:8090/`：连接后可作为房间对端收发语音、执行入房/切房/静音/退出，页面显示本端房间与最近动作。`?dev=<设备MAC>` 参数使第二行实时显示目标设备的模式、房间、邀请码与静音态。跨网段或丢包环境下浏览器到 Janus 的媒体可经 TURN 中继：`?turn=<user>:<pass>@<host:port>`（如 `turn=intercom:secret@10.0.0.5:3478`）。

真麦克风需要安全上下文：`http://localhost:8090/`（SSH 隧道）或 HTTPS。直连局域网地址时浏览器不提供 `mediaDevices`，页面会给出中文错误提示；勾选「用正弦波代替麦克风」仅验证信令链路。

```bash
ssh -L 8090:localhost:8090 <server-host>   # 之后本机打开 http://localhost:8090/
```

只读状态接口：`GET /state?device_id=<MAC>` 返回在线设备的模式、房间、在线人数与静音态；邀请码字段仅在携带匹配的 `X-Admin-Token` 头时返回。

## 开发者构建与测试

### 组件清单

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
| softdev | `softdev -ota ota.json -device-id <id> -client-id <id> -ctrl ws://<server>:8090/ws -script <脚本>`；替代固件执行平台会话与真实房间操作 |
| wsprobe | 控制协议 + WebRTC 探针：入房、向房间发送 ogg/Opus、计数与录制下行 |
| mcpbridge | `mcpbridge -endpoint <平台mcpproxy-wss带token> -server http://<server>:8090 -target <设备MAC> [-admin-token <t>]` 把平台反向通道上的 `intercom.*` 调用译成 `/admin/op` |
| browser | `tools/browser/index.html`，`?dev=` 监视设备；`node --test tools/browser/client.test.mjs` 执行协议回归 |
| gen_clips | `python3 tools/announce/gen_clips.py` 重新生成 `firmware/intercom/main/announce_clips.h`（macOS `say` + opusenc） |

注入式测试路径：平台 `SELFTEST_TEXT` 文本注入覆盖平台智能体、工具协议与 TTS 下行；`/admin/op` 覆盖 NFC/云端接管路径；`softdev -script` 覆盖设备智能体会话。三者分别落在真实执行代码上，可在无硬件、无语音环境下回归；真机麦克风采集、唤醒词检测与通话听感属人工验收项。

### 测试命令

```bash
(cd server && go test ./internal/server/...)                # 控制服务
node --test tools/browser/client.test.mjs                  # 浏览器端协议回归
(cd firmware/intercom && python3 -m unittest discover tests)  # 固件宿主测试（逻辑桩 + 源码守卫）
```

## 演示脚本

### 空库复现流程：建房 → 获取邀请码 → 加入 → 按名切回

空数据库没有任何房间与邀请码；首个房间由「加入一个不存在的 NFC 入口」触发——服务端原子建房并将该 `entry_id` 绑定到新房间。以下步骤已在干净实例（新数据库与同一 Janus 实例）上完整验证。

1. 启动服务（浏览器/探针客户端的 `hello` 不携带 token，省略 `-device-token`）：

   ```bash
   ./server/bin/intercom-server -addr :8090 -janus ws://<janus>:8188 \
     -db /tmp/fresh-intercom.db -admin-token <secret>
   ```

2. 创建第一间房并获取邀请码，以下两种方式任选其一：
   - 浏览器：`http://localhost:8090/` → 服务填 `<host>:8090` → 连接 → 下拉选「NFC 标签」→ 值填 `tag-demo-1` → 「入房/切房」。页面显示 `一号房（邀请码 xxxxxx，在线 1）`。
   - wsprobe：`wsprobe -url ws://<server>:8090/ws -id dev-1 -entry tag-demo-1 -listen 3s` → `JOINED room=一号房 code=XXXXXX`。

3. 创建第二间房：使用新的 `entry_id`（`tag-demo-2`）重复上述操作，得到 `二号房` 与第二个邀请码。

4. 第二端加入：更换身份（浏览器修改「身份」框，或 wsprobe `-id dev-2`），`kind=invite` 填写第一间房的邀请码入房。

5. 首次加入另一房间并切回：第一端用第二间房的邀请码 join；此后该房间进入其「已加入」记录，可按名称切回——浏览器选择「房间名」并填写 `一号房`，或 `wsprobe -invite <二号房码> -switch 一号房 -listen 3s`（一次运行完成 join + 按名切换）。

6. 对照：`GET /admin/rooms`（带 `X-Admin-Token`）列出全部房间与邀请码。

固件设备遵循同一协议：首次对设备说「加入对讲房间，邀请码 + 逐位数字」入房，之后对设备说「切换到 + 房间名」在已加入房间间切换。

### 演示环境

房间邀请码由服务端在建房时生成，随环境变化。获取当前房间与邀请码：

```bash
curl -H "X-Admin-Token: <admin-token>" http://<server>:8090/admin/rooms
# {"ok":true,"rooms":[{"name":"一号房","invite_code":"…", …}]}
```

也可直接对设备说「房间邀请码是什么」，智能体播报当前房间邀请码。

推荐演示顺序：唤醒词或按住命令键自我介绍 → 「加入对讲房间，邀请码 + 逐位数字」→ 查询房间 → 双击静音再恢复 → 「切换到 + 房间名」→ 从第二端或云端接管切回 → 「退出对讲房间」。云端接管示例：

```bash
curl -X POST -H "X-Admin-Token: <admin-token>" -H "Content-Type: application/json" \
  -d '{"device_id":"<设备MAC>","op":"join","arg":"<邀请码>"}' \
  http://<server>:8090/admin/op
```

设备收到下投操作后本地播报房间名；也可以在另一灵矽会话中调用 `intercom.*` 工具完成同样的接管。
