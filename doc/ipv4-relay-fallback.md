# IPv4 Relay Fallback

IPv4 Relay 是四种可排序穿透模式之一。会合服务器为一对 Peer 分配一个公网 UDP
端口，并在独立线程中双向透传数据报；通常仍建议把它排在直连模式之后。

客户端、会合控制面和 Relay App 的统一状态图见
[state-machine.md](state-machine.md)。

Relay 不改变 TUN 内层协议，也不增加逐包封装。IPv4 TUN 包、`KEEPALIVE`、
`KEEPALIVE_ACK` 和 `PADDING` 仍使用原有格式，只是网络端点由另一个 Peer 改为
会合服务器的 relay 端口。

## 启用条件与回退顺序

客户端必须设置：

```ini
traversal_modes=nat:true,nat4:true,ipv6:false,ipv4_relay:true
```

会合服务器必须设置：

```json
"ipv4_relay_enabled": true,
"ipv4_relay_port_start": 40000,
"ipv4_relay_port_end": 40100
```

默认建议顺序为：

```text
IPv4 精确端口 Punch
        |
        v
NAT4 socket pool
        |
        v
IPv6 直连（启用时）
        |
        v
IPv4 Relay（启用时）
```

只有 IPv4 阶段已经匹配到目标 Peer 后才允许进入 relay，不能使用 relay 绕过原有
room、Peer ID 和 `auth_token` 校验。Relay 协商使用新一轮 `punch_timeout` 作为等待
时限，因此两端回退配置差异很大时，应适当增大该值。

## 会话与线程模型

每对 Peer 对应：

- 一个双方共用的 UDP relay 端口；
- 一个 UDP socket；
- 一个独立工作线程；
- 一个随机 session ID；
- A、B 各自独立的随机 access key。

会合服务器的主线程只接收和校验 `RELAY_JOIN`，随后将请求交给
`rendezvous/ipv4_relay_app.cpp`。端口分配、数据端点认证、双向转发、统计和资源回收
均由这个独立 app 管理，不占用房间注册表的收包循环。

端口范围内的端口按需绑定。一个活动会话占用一个端口，因此范围大小同时限制最大
relay 并发数；范围耗尽时服务器返回 `ipv4-relay-port-exhausted`。

## 协商流程

```text
Client A                 控制端口                 Relay 线程                 Client B
   | RELAY_JOIN ------------->|                       |                         |
   |<------------ RELAY_OFFER |                       |                         |
   |------------------------- RELAY_HELLO ---------->|                         |
   |                          |<--------------------------- RELAY_JOIN ---------|
   |                          |---------------------------- RELAY_OFFER -------->|
   |                          |<---------- RELAY_HELLO -------------------------|
   |<--------------------------- RELAY_READY |                                 |
   |                          | RELAY_READY ----------------------------------->|
   |<====================== UDP datagram passthrough ==========================>|
```

第一端请求时，如果目标仍在服务器注册表中，服务器会立即创建线程并返回端口；如果
目标刚因 NAT4 换 socket 而短暂注销，则返回 `RELAY_WAIT`。客户端会重发请求，第二端
注册后创建会话，两端最终取得同一个 relay 端口。

控制协议消息如下：

| 方向 | 消息 | 字段 |
| --- | --- | --- |
| Client → 控制端口 | `RELAY_JOIN` | room、peer、target、token |
| 控制端口 → Client | `RELAY_WAIT` | 无；目标尚未重新注册 |
| 控制端口 → Client | `RELAY_OFFER` | relay port、目标 Peer ID、session ID、本端 access key |
| Client → relay 端口 | `RELAY_HELLO` | session ID、本端 Peer ID、本端 access key |
| relay 端口 → Client | `RELAY_READY` | session ID、目标 Peer ID |

`RELAY_JOIN` 和 `RELAY_HELLO` 都可重发，服务器按幂等请求处理。Relay 线程不采用
`RELAY_JOIN` 数据报的源端点作为最终地址；只有携带正确随机 access key 的
`RELAY_HELLO` 才能绑定实际数据端点，从而适配目的地址相关的 NAT 映射并降低端口
劫持风险。

## 生命周期

- 双方未在 `client_timeout_seconds` 内完成 `RELAY_HELLO`，线程退出并归还端口；
- 建立后任意一端超过 `client_timeout_seconds` 没有数据或 keepalive，会话退出；
- 客户端正常注销时，服务器立即停止包含该 Peer 的 relay 会话；
- 会合服务器停止或 TUI 执行重启时，先停止全部 relay socket，再 join 所有工作线程。

默认客户端每 15 秒发送 keepalive，默认服务端超时为 60 秒，两者可以正常配合。

## 部署与安全

除控制端口（默认 `3478/udp`）外，公网防火墙、安全组及容器端口映射必须放行完整的
relay UDP 范围。例如默认值需要放行 `40000-40100/udp`。服务器只会绑定当前活动
会话使用的端口。

Relay 会消耗服务器上下行带宽，并让服务器看到未加密的隧道数据。建议公网部署：

- 设置非空 `auth_token`；
- 根据带宽和线程容量缩小 relay 端口范围；
- 保持 `ipv4_relay_enabled=false`，直到防火墙和容量规划完成；
- 不要把 session ID 或 access key 写入日志。

## 前端设置

- Console：在 `tunnel.conf` 的 `traversal_modes` 中启用并排序 `ipv4_relay`；
- TUI/GUI：Settings → Traversal strategy 表格；
- 会合服务器 TUI：Config → IPv4 relay，可配置开关及端口范围；Dashboard 显示活动
  relay 数、已转发数据报和字节数；Relay Tab 列出每个 relay 会话的房间、双方节点、
  公网端点、中继端口、状态和空闲时间。即使控制端口注册记录已超时，活动 relay 会话
  仍会显示。

## 常见错误

- `IPv4 relay is disabled on the rendezvous server`：服务器配置中的
  `ipv4_relay_enabled` 未开启；这不是对方客户端不支持；
- `ipv4-relay-port-exhausted`：端口范围已全部占用，或范围中的端口无法绑定；
- `ipv4-relay-resource-unavailable`：服务器无法创建会话对象或工作线程；
- `IPv4 relay fallback timed out`：对端未启用、两端进入 fallback 的时间差过大、数据
  端口未放行，或服务器版本过旧；
- 已连接后发生 Peer timeout：服务端端口映射被中间防火墙回收，或另一端已经退出。
