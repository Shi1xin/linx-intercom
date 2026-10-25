# 测试环境

一套独立的 Janus + 控制服务测试环境，绑定仓库源码版本。

## 组成

| 组件 | 形式 | 地址 |
| --- | --- | --- |
| Janus AudioBridge | docker `canyan/janus-gateway`，host 网络 | ws `8188`，admin ws `7188`（secret 在 `janus/janus.jcfg` 中设置） |
| 控制服务 | Go 进程 | `ws://<host>:8090/ws`，HTTP `8090` |
| coturn（可选） | `docker-compose.turn.yml` 叠加文件 | `<host>:3478`，需要 `TURN_PASS`/`PUBLIC_IP` |
| TURN（备选） | `tools/turnsrv`（pion/turn） | `<host>:3478`，user `intercom` |
| SQLite | `infra/data/intercom.db` | 持久化设备/标签/房间/成员/恢复目标 |

## 启停

以下命令均从仓库根执行，除非另有说明。

```bash
# Janus（主 compose 只含 Janus，无需 TURN 变量）
docker compose -p intercom-test -f infra/testenv/docker-compose.yml up -d janus   # 启动
docker compose -p intercom-test -f infra/testenv/docker-compose.yml logs -f janus # 日志
docker compose -p intercom-test -f infra/testenv/docker-compose.yml down          # 停止
docker logs intercom-test-janus-1 | tail -50                                      # 诊断

# coturn（可选叠加：仅当测试机与被测端跨网段需要中继时启用）
TURN_PASS=<secret> PUBLIC_IP=<host-lan-ip> \
  docker compose -p intercom-test \
    -f infra/testenv/docker-compose.yml \
    -f infra/testenv/docker-compose.turn.yml up -d coturn

# 控制服务
(cd server && go build -o bin/intercom-server ./cmd/intercom-server)
./server/bin/intercom-server \
  -addr :8090 -janus ws://127.0.0.1:8188 -db infra/data/intercom.db \
  -device-token <token> -admin-token <token>

# TURN 备选（小工具，适合在客户端所在网段跑）
(cd tools/turnsrv && go build -o /tmp/turnsrv .)
TURN_PASS=<secret> /tmp/turnsrv -addr 0.0.0.0:3478 -public-ip <this-host-lan-ip>

# 媒体探针
(cd tools/wsprobe && go build -o /tmp/wsprobe .)
TURN_PASS=<secret> /tmp/wsprobe -url ws://<host>:8090/ws -id probe-1 \
  -entry tag-x -turn <turn-host>:3478 \
  -ogg sine880.ogg -record /tmp/rx.ogg -listen 10s
ffmpeg -i /tmp/rx.ogg -af "bandpass=f=880:w=200,volumedetect" -f null -

# 浏览器测试端（控制服务本身也在 :8090/ 伺服该页面，也可独立起静态服务）
(cd tools/browser && python3 -m http.server 8321)
# 打开 http://localhost:8321，服务填 <host>:8090
```

## 已知行为

- Janus `full_trickle` + `ice_lite`：SDP answer 不含候选，全部经网关 trickle；服务端按序转发（候选在 answer 之后下发）
- 参与者默认 muted 加入，`confirm` 后按设备持久化静音设置恢复
- `janus_room = 700000 + room_id` 持久派生，重启不碰撞
- 测试 ogg 必须单包页：`ffmpeg -page_duration 20000`
