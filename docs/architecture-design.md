# 产品架构

技术附录：本文描述对讲子系统的内部结构（数据模型、状态机、交互时序）。产品定义、双端架构全貌与设计依据见 [产品设计](product-design.md)，消息格式见 [控制协议](control-protocol.md)。

## 系统架构

设备固件基于 [ESP-IDF](https://github.com/espressif/esp-idf) 和 [ESP-WebRTC](https://github.com/espressif/esp-webrtc-solution)，完成音频处理、实时通信和语音控制。NFC 采用软件入口：服务端解析预登记标签并下发 `op:nfc`，固件经同一房间操作执行链入房，物理读卡器列入后续方向。房间控制服务使用 Go，SQLite 保存设备、标签和房间关系。

音频服务采用 [Janus AudioBridge](https://janus.conf.meetecho.com/docs/audiobridge.html)，在服务器端混合成员声音。每个终端通过一个 WebRTC PeerConnection 上传自己的音频，接收其他成员的混合音频，支持同时发话和收听。

```mermaid
flowchart TB
    N["NFC 入口（op:nfc 注入）"] --> C["房间控制服务"]
    D["设备固件<br/>音频处理 / 语音控制 / 会话管理"]
    D <-->|WSS 控制消息| C["房间控制服务"]
    D <-->|WSS 灵矽会话 · MCP 工具 · TTS| P["灵矽平台<br/>ASR / 智能体 / TTS"]
    C <--> S["SQLite"]
    C <-->|房间操作 / SDP / ICE| J["Janus AudioBridge"]
    P -->|云端 intercom.* 工具| C
    D <-->|WebRTC 音频| J
    D <-->|中继路径| T["TURN"]
    T <-->|加密媒体| J
```

控制服务认证设备身份、解析 NFC 入口和邀请码、维护房间与成员关系，并转发 Janus 信令。设备通过 IP 网络连接服务，TURN 在媒体路径需要中继时转送加密音频。

灵矽平台会话是默认语音入口：唤醒词或命令键开一轮平台 listen，智能体经设备侧 MCP 工具（`self.intercom.*`）发起房间操作；云端 MCP 工具（`intercom.*`）经控制服务的操作投递到达设备。两类入口与 NFC、召唤、断线恢复共用同一组房间操作，下文时序图以控制协议视角描述统一执行路径。

设备上行经过采集、直流阻断高通滤波和 Opus 编码，下行经过解码和播放。音频处理模块预留回声消除接入点，播放参考取实际送往扬声器的 PCM。唤醒词在设备本地检测（esp-sr wakenet）

语音入口是唤醒词与命令键触发的灵矽平台 listen 轮：命令键按住时采集音频送平台、松开结束收音，识别与意图理解由平台 ASR 和智能体完成，房间操作经设备侧 MCP 工具调用回到统一执行路径。进入指令处理时，设备暂停房间上行并继续播放房间声音；操作完成后，按会话状态和静音设置恢复音频。本地提示使用预生成 Opus 片段或平台 TTS。

## 数据模型

- **Device**：以固定 MAC 地址作为 `device_id`，保存认证凭据、静音设置和连接恢复目标。MAC 用于定位设备，凭据用于认证。
- **NFCEntry**：以标签标识作为 `entry_id`，记录入口类型及目标。房间入口指向 `room_id`；设备标签的 UID 预先登记并指向目标设备的 MAC，标签也可直接携带 MAC 内容。
- **Room**：保存 `room_id`、系统生成的唯一名称、唯一数字邀请码 `invite_code` 及 Janus 房间映射。名称采用「一号房」「二号房」等编号形式。
- **Membership**：保存设备与已加入房间的关系，用于语音再次进入。
- **Session**：维护连接状态、当前房间、目标房间和当前操作的 `request_id`。

标签标识保持唯一，设备与房间的成员关系保持唯一。控制服务优先解析设备标签，其余未登记的 UID 作为房间入口，原子创建标签映射和房间；重复请求返回同一结果。

设备在媒体就绪后确认当前房间，并保存连接恢复目标。切房过程中保留原房间作为失败恢复目标；主动退出清除连接恢复目标，保留已加入记录。

会话操作携带请求 ID，设备仅应用当前操作对应的响应。房间名称、邀请码和映射持久化保存，空房间释放媒体资源，再次加入时恢复。

## 状态机

```mermaid
stateDiagram-v2
    state "待机" as Idle
    state "入房" as Joining
    state "通话" as Talking
    state "指令处理" as Command
    state "切房" as Switching
    state "恢复连接" as Recovering
    state "退出" as Leaving
    [*] --> Idle
    Idle --> Joining: NFC 或语音加入
    Joining --> Talking: 媒体就绪
    Joining --> Idle: 加入失败
    Talking --> Command: 按下命令键并暂停上行
    Command --> Talking: 查询或更新静音设置
    Command --> Talking: 命令完成或取消
    Command --> Switching: 选择目标房间
    Talking --> Switching: 触碰其他房间入口
    Switching --> Talking: 切换成功
    Switching --> Talking: 失败后恢复原房间
    Talking --> Recovering: 连接中断
    Command --> Recovering: 连接中断
    Switching --> Recovering: 连接中断
    Recovering --> Talking: 恢复已确认房间
    Command --> Leaving: 退出命令
    Recovering --> Leaving: 退出命令
    Leaving --> Idle: 结束媒体会话
```

静音是独立于会话状态的用户设置。指令处理和切房临时暂停上行，保持静音设置；恢复连接使用最近一次确认的房间。主动退出优先清除恢复目标，使后续连接事件保持待机意图。

## 交互时序

### 组队

控制服务将 NFC 入口或邀请码解析为目标房间，按名称加入时查询设备的已加入记录。控制服务协调 SDP 和 ICE，设备与音频服务建立媒体连接；媒体就绪后提交成员状态，并播报房间名称和在线人数。

```mermaid
sequenceDiagram
    actor U as 用户
    participant D as 加入设备
    participant C as 控制服务
    participant P as 目标设备
    participant J as AudioBridge
    U->>D: NFC 入口注入或说出加入命令
    D->>C: 加入请求（入口或语音目标、请求 ID）
    alt 房间标签
        C->>J: 创建或恢复对应房间
    else 设备标签
        C->>C: 查询目标设备的当前房间
        opt 目标设备在线且无当前房间
            C->>J: 创建房间
            C-->>P: 入房通知与信令
            P->>J: 建立媒体连接
            P->>C: 媒体就绪
        end
    else 邀请码或已加入的房间名
        C->>C: 查询对应房间
        C->>J: 恢复对应房间
    end
    C-->>D: 房间与信令信息
    D->>J: 建立媒体连接
    J-->>D: 媒体就绪
    D->>C: 确认当前房间
    C-->>D: 房间名称与成员状态
    D-->>U: 播报入房结果
```

目标设备离线、入口解析失败或语音目标无效时，控制服务返回对应结果，设备保持原会话状态。对同一入口的重复接触由设备去重，服务端按请求 ID 保证幂等。

### 通话

上行和下行同时进行。AudioBridge 为每位接收者混合其他成员的声音；设备语音命令通过控制通道处理。「房间信息」查询返回当前房间名称、在线人数和邀请码，设备播报结果，邀请码逐位播报。

```mermaid
sequenceDiagram
    actor U as 用户
    participant D as 设备
    participant C as 控制服务
    participant J as AudioBridge
    loop 通话期间
        par 上行
            U->>D: 讲话
            D->>J: Opus 音频
        and 下行
            J-->>D: 其他成员的混合音频
            D-->>U: 播放声音
        end
        opt 查询或静音命令
            U->>D: 按住命令键说出命令
            D->>D: 暂停麦克风上行
            D->>C: 查询或更新静音设置
            opt 静音设置发生变化
                C->>C: 保存用户设置
                C->>J: 更新参与者静音状态
            end
            C-->>D: 操作结果
            D->>D: 按静音设置调整上行
            D-->>U: 播报结果
        end
    end
```

### 房间切换

控制服务按邀请码查询房间，按名称查询设备已加入的房间，或解析 NFC 入口，使用 AudioBridge 的 `changeroom` 操作切换房间。设备沿用媒体连接，清理原房间的播放缓冲，并恢复用户的静音设置。

```mermaid
sequenceDiagram
    actor U as 用户
    participant D as 设备
    participant C as 控制服务
    participant J as AudioBridge
    U->>D: 按住命令键说出加入或切房命令，或 NFC 入口注入
    D->>D: 暂停麦克风上行
    D->>C: 切房请求（目标、请求 ID）
    C->>C: 解析目标房间
    C->>J: 暂时静音并执行 changeroom
    alt 切换成功
        J-->>C: 目标房间状态
        C-->>D: 更新房间与静音设置
        D->>D: 清理旧播放缓冲
        D->>C: 媒体就绪
        C->>J: 恢复用户静音设置
        C-->>D: 切换完成
        D->>D: 按静音设置恢复上行
        D-->>U: 播报目标房间
    else 切换失败
        C->>J: 恢复原房间参与状态
        C-->>D: 原房间与失败原因
        D->>D: 恢复原通话
        D-->>U: 播报结果
    end
```

### 退出

设备先清除连接恢复目标并停止音频，再结束服务端会话。已加入房间记录保留，用于后续再次进入。

```mermaid
sequenceDiagram
    actor U as 用户
    participant D as 设备
    participant C as 控制服务
    participant J as AudioBridge
    U->>D: 按住命令键说出退出命令
    D->>D: 清除恢复目标并停止音频
    D->>C: 退出请求
    C->>J: leave
    J-->>C: 成员已离开
    C->>C: 清除活动会话的房间关联
    C-->>D: 退出完成
    D-->>U: 播报退出并进入待机
```

控制连接中断时，设备仍执行本地退出；服务端在会话失效后回收对应媒体资源。

### 连接恢复

设备保存最近一次确认的房间和静音设置。连接恢复时重新认证，协调媒体连接，并在音频就绪后恢复通话。

```mermaid
sequenceDiagram
    actor U as 用户
    participant D as 设备
    participant C as 控制服务
    participant J as AudioBridge
    D->>D: 检测连接中断
    D-->>U: 提示正在恢复连接
    D->>C: 恢复控制连接并认证
    alt 恢复目标仍有效
        C->>J: 清理旧会话并恢复房间
        C-->>D: 房间、信令与静音设置
        D->>J: 建立媒体连接
        J-->>D: 媒体就绪
        D->>C: 确认恢复结果
        D-->>U: 播报连接恢复
    else 用户已退出
        D->>D: 保持待机
    end
```
