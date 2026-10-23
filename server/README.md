# 房间控制服务

本目录承载设备身份、NFC 入口解析、房间关系和 Janus 信令协调

计划采用 Go 与 SQLite，维护设备、入口、房间、成员及在线会话。服务端向设备返回入房结果和房间状态，音频由设备与 Janus AudioBridge 直接交换

运行入口、接口契约和配置示例随控制服务落地补充

- [产品与架构设计](../docs/product-design.md)
- [实施方案](../docs/implementation-plan.md)
