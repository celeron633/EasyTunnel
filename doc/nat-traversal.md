# EasyTunnel NAT 会合与打洞流程

本文描述当前实现中的 IPv4/UDP 会合、默认精确端口打洞和 NAT4 多 socket 回退流程。相关代码主要位于：

```text
nat_traversal.cpp          默认会合、精确端口打洞、保活
nat4_traversal.cpp         NAT4 socket 池和多轮重试
rendezvous/server.cpp      会合服务器 UDP 接收循环
rendezvous/registry.cpp    房间、配对和 NAT4 round 屏障
tunnel_engine.cpp          成功 socket 的数据面接管
```

## 术语和适用范围

本文沿用常见的简化叫法：

- **NAT3**：通常指端口受限锥形 NAT。UDP 映射大体具有 endpoint-independent mapping 特征，但入站来源受到限制。
- **NAT4**：通常指 symmetric NAT / endpoint-dependent mapping。访问会合服务器和访问 Peer 时可能得到不同的公网端口。
- **递增端口型 NAT4**：新建 UDP 映射时，公网端口倾向于小幅递增，而不是完全随机分配。

不同 NAT 测试工具的类型编号可能不同。EasyTunnel 的 NAT4 回退针对的是“公网端口可用固定正偏移近似预测”的场景。

## 总体状态机

```text
等待或选择 Peer
       │
       ▼
会合服务器交换公网端点
       │
       ▼
默认精确端口打洞（默认先尝试 2 秒）
       │
       ├── 成功 ──► 成功 socket 接管隧道数据面
       │
       └── 失败
             │
             ├── nat4_source_port_count = 0
             │      └── 继续精确端口尝试，直到 punch_timeout
             │
             └── NAT4 socket 池多轮打洞
                    ├── 成功 ──► 命中的 socket 接管数据面
                    └── 总超时 ──► Error
```

默认模式和 NAT4 模式共享同一个总打洞期限。收到第一个有效 `PEER` 后开始计算 `punch_timeout`；默认值为 30 秒。

## 会合阶段

控制报文使用 `ETN1` 文本协议，以制表符分隔字段。

### 等待端

`target_peer_id` 为空时，客户端周期性发送：

```text
REG(room_id, peer_id, auth_token)
```

会合服务器从 UDP 数据报的真实来源获得该客户端的公网 `IPv4:port`。等待端在尚未匹配 Peer 时没有实际的短期打洞超时；收到 `PEER` 后才开始计算 `punch_timeout`。

### 主动连接端

主动端周期性发送：

```text
REG(room_id, peer_id, auth_token)
CONNECT(room_id, peer_id, target_peer_id, auth_token)
```

服务器找到目标后，向双方发送：

```text
PEER(peer_public_ip, peer_public_port, peer_id)
```

服务器只交换端点，不转发隧道数据。打洞成功后的 TUN IPv4 数据直接在两个客户端之间传输。

## 默认模式：精确端口打洞

客户端收到 `PEER` 后，使用完成会合的同一个 UDP socket 向服务器提供的精确端点发送：

```text
PUNCH(room_id, my_peer_id)
```

双方流程如下：

```text
Client A                   Rendezvous                   Client B
   │                           │                            │
   │──── REG / CONNECT ───────►│◄──────── REG ─────────────│
   │◄────── PEER(B endpoint) ──│──── PEER(A endpoint) ────►│
   │                           │                            │
   │────── PUNCH ─────────────────────────────────────────►│
   │◄────────────────────────────── PUNCH / PUNCH_ACK ─────│
   │                           │                            │
   │        后续 IPv4 数据和 KEEPALIVE 均不经过服务器       │
```

握手包必须满足以下条件才会被接受：

- 来源公网 IP 与服务器提供的 Peer IP 相同；
- `room_id` 相同；
- 报文中的发送方 Peer ID 与服务器提供的 Peer ID 相同；
- 类型为 `PUNCH` 或 `PUNCH_ACK`。

握手阶段允许来源端口与服务器提供的端口不同。如果报文身份和公网 IP 均正确，客户端会把实际来源端口保存为最终 Peer 端点。收到 `PUNCH` 时会连续发送 5 个 `PUNCH_ACK`，降低确认包丢失导致两端状态不一致的概率。

### 默认模式持续时间

- NAT4 socket 池启用时：精确端口快速路径默认运行约 2 秒，随后自动切换 NAT4。
- `nat4_source_port_count=0` 时：禁用 NAT4 回退，精确端口模式持续到 `punch_timeout`。
- 主动端在一直找不到目标 Peer 时，也会在 `punch_timeout` 后退出。

## NAT4 模式：多 socket 端口预测

精确端口阶段未成功时，客户端先注销并关闭原会合 socket，然后进入独立的 NAT4 状态机。

默认参数为：

```ini
nat4_source_port_start=30000
nat4_source_port_count=25
nat4_peer_port_offset=20
nat4_round_timeout=10
```

### 1. 建立本轮 socket 池

round 0 默认绑定：

```text
30000, 30001, ... 30024
```

其中第一个 socket 同时承担本轮与会合服务器通信的职责，其余 socket 只用于制造候选 NAT 映射和接收 PUNCH。

如果端口段中任意一个本地端口绑定失败，本轮池会整体关闭，并立即尝试下一段端口。因为端口占用导致的跳段不会增加 round ID：round 表示一次真正开始的打洞轮次，而不是本地端口段编号。

### 2. 双方 round 屏障

每端创建好 socket 池后，通过池中的第一个 socket 周期性发送：

```text
NAT4_JOIN(room_id, peer_id, target_peer_id, round, auth_token)
```

服务器记录：

- Peer 当前所在的 round；
- 本轮第一个 socket 的真实公网端点；
- Peer 是否已完成本轮 JOIN；
- 本轮目标 Peer。

只有一端进入本轮，或者两端 round 不同时，服务器返回：

```text
NAT4_WAIT(round)
```

同一对 Peer 都进入相同 round 后，服务器才同时向双方发送：

```text
NAT4_PEER(peer_public_ip, peer_public_port, peer_id, round)
```

客户端只接受与自己当前 round 相同的 `NAT4_PEER`。这个屏障避免 A 已经切换到新 socket 池，而 B 仍在上一轮时使用过期端点打洞。

```text
Client A                 Rendezvous                 Client B
   │                         │                          │
   │── NAT4_JOIN(round 1) ──►│                          │
   │◄── NAT4_WAIT(1) ────────│                          │
   │                         │◄── NAT4_JOIN(round 0) ──│
   │                         │── NAT4_WAIT(0) ────────►│
   │                         │◄── NAT4_JOIN(round 1) ──│
   │◄── NAT4_PEER(round 1) ──│── NAT4_PEER(round 1) ─►│
   │                         │                          │
   │════════════ 双方从同一 round 的池开始 PUNCH ═════│
```

两端不需要知道对方本地使用了 `30000 + n` 中的哪一段。公网端口可能与本地端口完全不同，真正用于预测的是服务器在当前 round 实际观察到的公网端口。

### 3. 预测对端公网端口

假设服务器本轮观察到 Peer B：

```text
203.0.113.20:50000
```

默认 `nat4_peer_port_offset=20`，A 的固定预测目标为：

```text
203.0.113.20:50020
```

A 的全部 25 个 socket 都向该固定目标发送 PUNCH。B 同时对 A 执行相同操作。初次收到本轮 `NAT4_PEER` 时，每个 socket 连续发送 5 次；之后在本轮内约每 2 秒补发一次，以覆盖丢包和两端进入 ready 状态的短暂时间差。

这里与简单的“扫描对端 `+0…+20`”不同：NAT4 模式固定预测一个对端端口，再通过多个本地 socket 创建一批不同的公网映射，逻辑与 n4 的 socket 池模型一致。

### 4. 选择成功 socket

客户端使用 `select` 同时等待池中所有 socket。收到合法 `PUNCH` 或 `PUNCH_ACK` 后：

1. 将收到报文的 socket 选为 winner；
2. 将报文真实来源保存为最终 Peer 端点；
3. winner 连续发送 10 次 `PUNCH_ACK`，每次间隔 200ms；
4. 如果 winner 不是本轮会合 socket，先用会合 socket 发送 `UNREG`；
5. 关闭池中所有非 winner socket；
6. 把 winner 返回给 `TunnelEngine`，继续承载隧道数据和保活。

NAT4 池 socket 带有 1 秒接收超时，因此 winner 接管后仍可正常运行 KEEPALIVE、Peer 超时检查和停止唤醒逻辑。

### 5. 本轮失败和下一轮

单轮最多等待 `nat4_round_timeout` 秒，同时不会超过剩余的总 `punch_timeout`。失败后：

```text
发送 UNREG
关闭本轮所有 socket
source_port_start += source_port_count
round += 1
```

默认端口段变化如下：

```text
round 0: 30000～30024
round 1: 30025～30049
round 2: 30050～30074
```

默认 `punch_timeout=30` 时，扣除约 2 秒精确端口尝试后，通常可以运行两轮完整的 10 秒 NAT4 尝试和一轮缩短的尝试。需要更多轮次时应增大 `punch_timeout`。

## 成功后的数据面

无论通过默认模式还是 NAT4 模式成功，最终都只保留一个 UDP socket 和一个确认后的 Peer 端点：

```text
TUN IPv4 packet ──► winning UDP socket ──► confirmed peer endpoint
KEEPALIVE       ──► winning UDP socket ──► confirmed peer endpoint
```

默认参数：

- `keepalive_interval=15`：每 15 秒发送一次 `KEEPALIVE`；
- `peer_timeout=45`：45 秒内没有合法 Peer 数据或控制包则连接进入 Error；
- `punch_timeout=30`：收到 Peer 后，默认模式和 NAT4 模式共享的打洞总期限。

数据面只接受最终确认端点发来的 IPv4 数据。握手之后不会继续接受同 IP 的任意端口变化。

## 配置项

| 配置 | 默认值 | 约束 | 说明 |
|---|---:|---:|---|
| `punch_timeout` | 30 | 正整数；GUI/TUI 上限 600 秒 | 收到 Peer 后的总打洞期限 |
| `nat4_source_port_start` | 30000 | 1～65535 | 第一段连续本地端口起点 |
| `nat4_source_port_count` | 25 | 0～60 | 每轮 socket 数量；0 表示关闭 NAT4 |
| `nat4_peer_port_offset` | 20 | 0～256 | 预测公网端口的固定正偏移 |
| `nat4_round_timeout` | 10 | 1～60 秒 | 单轮 socket 池等待时间 |
| `keepalive_interval` | 15 | 正整数 | 连接后的保活间隔 |
| `peer_timeout` | 45 | 大于保活间隔 | 连接后的 Peer 失活超时 |

`nat4_source_port_start + nat4_source_port_count - 1` 不能超过 65535。Windows 使用 `select` 管理池，因此 socket 数量限制为 60，为控制 socket 集合保留余量。

## 典型日志

切换到 NAT4：

```text
Exact-port punch did not complete; switching to NAT4 socket pool
NAT4 round 0, source ports 30000-30024, peer offset=+20
```

服务器完成同轮屏障：

```text
NAT4 ready room=... round=0 peer=... endpoint=... target=... endpoint=...
```

客户端获得预测目标并成功：

```text
NAT4 peer observed as 203.0.113.20:50000, target 203.0.113.20:50020
NAT4 socket pool matched local port 30017 with peer 203.0.113.20:50020
NAT4 hole punching confirmed with 203.0.113.20:50020
```

## 部署与兼容性

NAT4 round 屏障依赖 `NAT4_JOIN`、`NAT4_WAIT` 和 `NAT4_PEER`，因此客户端和会合服务器必须一起升级。旧会合服务器会忽略 `NAT4_JOIN`，客户端最终表现为 NAT4 打洞超时。

以下情况仍可能失败：

- 公网端口随机分配、递减或增长量超过配置偏移；
- NAT/CGN 针对不同目的地址使用不可预测的公网 IP；
- 运营商或企业网络禁止 P2P UDP；
- 同公网出口但 NAT 不支持 hairpin；
- 两端进入相同 round 的时间差持续超过单轮超时；
- NAT 映射在握手或连接期间被主动回收。

当前没有 UDP relay 回退。打洞总超时后连接进入 Error。
