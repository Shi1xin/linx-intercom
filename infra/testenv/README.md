# 测试环境

一套独立的 Janus + 控制服务测试环境，绑定仓库源码版本。

## 组成

| 组件 | 形式 | 地址 |
| --- | --- | --- |
| Janus AudioBridge | docker `canyan/janus-gateway`，host 网络 | ws `8188`，admin ws `7188`（secret 在 `janus/janus.jcfg` 中设置） |
| 控制服务 | Go 进程 | `ws://<host>:8090/ws`，HTTP `8090` |
| TURN | `tools/turnsrv`（pion/turn） | `<host>:3478`，user `intercom` |
| SQLite | `infra/data/intercom.db` | 持久化设备/标签/房间/成员/恢复目标 |

## 启停

```bash
# Janus
cd infra/testenv
docker compose -p intercom-test up -d janus          # 启动
docker compose -p intercom-test logs -f janus        # 日志
docker compose -p intercom-test down                 # 停止
docker logs intercom-test-janus-1 | tail -50         # 诊断

# coturn（可选，跨网段媒体中继；需要 PUBLIC_IP 与 TURN_PASS）
PUBLIC_IP=<host-lan-ip> TURN_PASS=<secret> \
  docker compose -p intercom-test --profile turn up -d coturn

# 控制服务（在项目根目录构建后）
cd server && go build -o bin/intercom-server ./cmd/intercom-server && go test ./...
./server/bin/intercom-server \
  -addr :8090 -janus ws://127.0.0.1:8188 -db infra/data/intercom.db \
  -device-token <token> -admin-token <token>

# TURN（小工具，适合在客户端所在网段跑）
cd tools/turnsrv && go build -o /tmp/turnsrv .
TURN_PASS=<secret> /tmp/turnsrv -addr 0.0.0.0:3478 -public-ip <this-host-lan-ip>

# 媒体探针
cd tools/wsprobe && go build -o /tmp/wsprobe .
TURN_PASS=<secret> /tmp/wsprobe -url ws://<host>:8090/ws -id probe-1 \
  -entry tag-x -turn <turn-host>:3478 \
  -ogg sine880.ogg -record /tmp/rx.ogg -listen 10s
ffmpeg -i /tmp/rx.ogg -af "bandpass=f=880:w=200,volumedetect" -f null -

# 浏览器测试端
cd tools/browser && python3 -m http.server 8321
# 打开 http://localhost:8321，服务填 <host>:8090
```

## 已知行为

- Janus `full_trickle` + `ice_lite`：SDP answer 不含候选，全部经网关 trickle；服务端按序转发（候选在 answer 之后下发）
- 参与者默认 muted 加入，`confirm` 后按设备持久化静音设置恢复
- `janus_room = 700000 + room_id` 持久派生，重启不碰撞
- 测试 ogg 必须单包页：`ffmpeg -page_duration 20000`
