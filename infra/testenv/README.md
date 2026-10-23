# 测试环境（Omarchy）

独立于既有服务的一套 Janus + 控制服务测试环境，绑定本任务源码版本。

## 组成

| 组件 | 形式 | 地址 |
| --- | --- | --- |
| Janus AudioBridge | docker `canyan/janus-gateway`，host 网络 | ws `8188`，admin ws `7188`（secret `intercomnight`） |
| 控制服务 | Go 进程，linx-run job 管理 | `ws://<omarchy>:8090/ws`，HTTP `8090` |
| TURN | Mac 上 `tools/turnsrv`（pion/turn） | `10.234.79.213:3478`，user `intercom` |
| SQLite | `infra/data/intercom.db` | 持久化设备/标签/房间/成员/恢复目标 |

Omarchy 上 `10.234.x` LAN 的 TCP/UDP 被主机防火墙拦（ICMP 通），客户端一律走 tailscale `100.115.18.21`；面包板经 Mac TURN 中继。

## 启停

```bash
# Janus（Omarchy）
cd /home/seth/Work/linx/linx-intercom/infra/testenv
docker compose -p intercom-test up -d janus          # 启动
docker compose -p intercom-test logs -f janus        # 日志
docker compose -p intercom-test down                 # 停止
docker logs intercom-test-janus-1 | tail -50         # 诊断

# 控制服务（Omarchy，linx-run 管理）
linx-run start --host omarchy --cwd /home/seth/Work/linx/linx-intercom \
  --task intercom-server -- ./server/bin/intercom-server \
  -addr :8090 -janus ws://127.0.0.1:8188 -db infra/data/intercom.db
linx-run status --host omarchy --task intercom-server
linx-run logs   --host omarchy --task intercom-server --stream stderr --bytes 8000

# 更新代码（Mac 侧 bundle 同步后）
cd server && go build -o bin/intercom-server ./cmd/intercom-server && go test ./...

# TURN（Mac）
cd tools/turnsrv && go build -o /tmp/turnsrv .
/tmp/turnsrv -addr 0.0.0.0:3478 -public-ip <Mac LAN IP>

# 媒体探针
cd tools/wsprobe && go build -o /tmp/wsprobe .
/tmp/wsprobe -url ws://100.115.18.21:8090/ws -id probe-1 \
  -entry tag-x -turn 10.234.79.213:3478 \
  -ogg sine880.ogg -record /tmp/rx.ogg -listen 10s
ffmpeg -i /tmp/rx.ogg -af "bandpass=f=880:w=200,volumedetect" -f null -

# 浏览器测试端（Mac）
cd tools/browser && python3 -m http.server 8321
# 打开 http://localhost:8321，服务填 100.115.18.21:8090
```

## 已知行为

- Janus `full_trickle` + `ice_lite`：SDP answer 不含候选，全部经网关 trickle；服务端按序转发（候选在 answer 之后下发）
- 参与者默认 muted 加入，`confirm` 后按设备持久化静音设置恢复
- `janus_room = 700000 + room_id` 持久派生，重启不碰撞
- 测试 ogg 必须单包页：`ffmpeg -page_duration 20000`
