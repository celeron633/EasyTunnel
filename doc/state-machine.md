# EasyTunnel 状态机与协商时序

本文汇总客户端和会合服务器的状态切换。图中的状态名称用于说明逻辑；除
`TunnelState` 外，部分状态是对多个字段和循环阶段的归纳，并不都对应代码中的同名
枚举。

相关实现：

- 客户端总控：[tunnel_engine.cpp](../tunnel_engine.cpp)
- Peer 选择：[peer_selection.cpp](../peer_selection.cpp)
- IPv4/NAT4：[nat_traversal.cpp](../nat_traversal.cpp)、[nat4_traversal.cpp](../nat4_traversal.cpp)
- IPv6 Fallback：[ipv6_fallback.cpp](../ipv6_fallback.cpp)
- IPv4 Relay Fallback：[ipv4_relay_fallback.cpp](../ipv4_relay_fallback.cpp)
- 会合控制面：[server.cpp](../rendezvous/server.cpp)、[registry.cpp](../rendezvous/registry.cpp)
- Relay 数据面：[ipv4_relay_app.cpp](../rendezvous/ipv4_relay_app.cpp)

## 1. 客户端顶层状态

客户端对 UI 暴露四种 `TunnelState`：

```mermaid
stateDiagram-v2
    [*] --> Disconnected
    Disconnected --> Connecting: Start(config)
    Error --> Connecting: 再次 Start(config)

    Connecting --> Connected: 传输路径确认且 TUN 打开成功
    Connecting --> Error: 协商失败 / 配置错误 / TUN 打开失败
    Connecting --> Disconnected: Stop

    Connected --> Error: Peer 超时
    Connected --> Disconnected: Stop / 普通工作线程结束
    Connected --> Disconnected: TUN 读取失败后清理

    Error --> Error: 工作线程清理完成但保留错误状态
    Disconnected --> [*]
```

说明：

- `Start` 只负责置为 `Connecting` 并创建客户端工作线程，不会立即打开 TUN。
- 只有传输路径完成确认后才打开 TUN，然后进入 `Connected`。
- Peer keepalive 超时会显式进入 `Error`；普通停止最终进入 `Disconnected`。
- 错误状态下工作线程仍会关闭 UDP socket 和 TUN，但保留 `Error` 供 UI 展示。

## 2. 客户端连接子状态机

四种策略都要求先通过 IPv4 会合通道的 `PEER` 匹配到明确目标。双方注册时上报已启用
模式，服务端按连接发起方的顺序取能力交集，并把协商结果随 `PEER` 返回双方。匹配完成
后，引擎按该协商顺序选择策略；任一策略成功即进入数据面，失败则继续下一项。没有共同
模式时，发起方立即失败，等待方继续保持注册状态。

```mermaid
stateDiagram-v2
    [*] --> OpenIpv4Socket
    OpenIpv4Socket --> Rendezvous: socket 创建成功
    OpenIpv4Socket --> Failed: 创建或解析失败

    Rendezvous --> WaitingPeer: REG / CONNECT
    WaitingPeer --> SelectStrategy: 收到含共同模式的合法 PEER
    WaitingPeer --> Failed: 对端不支持任何已启用模式
    WaitingPeer --> Failed: 首次响应或选定 Peer 超时
    WaitingPeer --> Stopped: Stop

    SelectStrategy --> ExactPunch: 下一项为 nat
    SelectStrategy --> Nat4: 下一项为 nat4
    SelectStrategy --> TryIpv6: 下一项为 ipv6
    SelectStrategy --> TryRelay: 下一项为 ipv4_relay
    SelectStrategy --> Failed: 没有剩余的已启用策略

    ExactPunch --> DataPathReady: PUNCH 或 PUNCH_ACK
    ExactPunch --> SelectStrategy: punch_timeout
    Nat4 --> DataPathReady: 任一 socket 收到合法 Peer PUNCH
    Nat4 --> SelectStrategy: punch_timeout 或失败

    TryIpv6 --> DataPathReady: V6_PUNCH / ACK 确认
    TryIpv6 --> SelectStrategy: 超时或失败

    TryRelay --> DataPathReady: 收到合法 RELAY_READY
    TryRelay --> SelectStrategy: 拒绝或 punch_timeout

    DataPathReady --> OpenTun
    OpenTun --> Connected: TUN 打开和配置成功
    OpenTun --> Failed: TUN 打开失败

    Stopped --> [*]
    Failed --> [*]
```

普通 NAT、增强 NAT4 和 Relay 分别使用独立的 `punch_timeout`；IPv6 使用
`ipv6_fallback_timeout`。

### 2.1 各阶段的 socket 接管

| 成功路径 | 最终数据 socket | 客户端保存的 Peer 端点 |
| --- | --- | --- |
| 精确 IPv4 Punch | 初始 IPv4 会合 socket | Peer 实际 IPv4 公网端点 |
| NAT4 | socket 池中收到合法 PUNCH 的 winner socket | PUNCH 的真实来源端点 |
| IPv6 Fallback | 新建的 IPv6 UDP socket，成功后替换原 socket | 对端 IPv6 GUA 与端口 |
| IPv4 Relay | IPv4 relay 协商 socket | 会合服务器 IP 与本会话 relay 端口 |

进入 `Connected` 后，TUN→网络与网络→TUN 两个数据线程只使用表中的最终 socket 和
端点。Relay 不改变数据线程：服务端转发后，客户端看到的 UDP 来源始终是已确认的
relay 端口。

## 3. 客户端完整协商时序

```mermaid
sequenceDiagram
    participant A as Client A
    participant S as Rendezvous Control
    participant B as Client B

    A->>S: REG(room, A, enabled modes A, token)
    B->>S: REG(room, B, enabled modes B, token)
    S-->>A: REGISTERED
    S-->>B: REGISTERED
    A->>S: CONNECT(room, A, B, enabled modes A, token)
    Note over S: 按 A 的顺序计算 A ∩ B
    S-->>A: PEER(B endpoint, modes B, negotiated modes)
    S-->>B: PEER(A endpoint, modes A, negotiated modes)

    par 双向 Punch
        A->>B: PUNCH(room, A)
    and
        B->>A: PUNCH(room, B)
    end

    alt 精确端口直连成功
        A-->>B: PUNCH_ACK
        Note over A,B: 初始 IPv4 socket 进入数据面
    else 精确端口失败且启用 NAT4
        loop 每个 NAT4 round
            A->>S: NAT4_JOIN(room, A, B, round, token)
            B->>S: NAT4_JOIN(room, B, A, round, token)
            S-->>A: NAT4_PEER(B endpoint, round)
            S-->>B: NAT4_PEER(A endpoint, round)
            A->>B: socket pool PUNCH
            B->>A: socket pool PUNCH
        end
        alt NAT4 成功
            Note over A,B: winner socket 进入数据面
        else NAT4 失败
            Note over A,B: 继续 IPv6 或 Relay Fallback
        end
    end
```

## 4. IPv6 Fallback 状态与时序

客户端先验证实际可用的 IPv6 GUA，再创建最终 IPv6 UDP socket。服务器只分配角色和
交换端点，不转发 IPv6 数据。

```mermaid
stateDiagram-v2
    [*] --> ProbeGua
    ProbeGua --> Failed: TCP 探针失败或源地址不是 GUA
    ProbeGua --> Join: 探针成功并绑定 IPv6 UDP socket
    Join --> WaitingPeer: 周期发送 V6_JOIN
    WaitingPeer --> WaitingPunch: V6_PEER(role=listen)
    WaitingPeer --> SendingPunch: V6_PEER(role=connect)
    SendingPunch --> SendingPunch: 每 200ms 发送 V6_PUNCH
    SendingPunch --> Ready: 收到 V6_PUNCH / ACK
    WaitingPunch --> Ready: 收到合法 V6_PUNCH 并回复 ACK
    WaitingPeer --> Failed: ERROR / ipv6_fallback_timeout
    SendingPunch --> Failed: ipv6_fallback_timeout
    WaitingPunch --> Failed: ipv6_fallback_timeout
    Ready --> [*]
    Failed --> [*]
```

```mermaid
sequenceDiagram
    participant A as Listener
    participant S as Rendezvous Control
    participant B as Connector

    A->>A: TCP probe，选择 GUA
    B->>B: TCP probe，选择 GUA
    A->>S: V6_JOIN(A GUA, port, inbound)
    B->>S: V6_JOIN(B GUA, port, inbound)
    S->>S: 检查至少一端允许入站并确定角色
    S-->>A: V6_PEER(B endpoint, role)
    S-->>B: V6_PEER(A endpoint, role)
    B->>A: V6_PUNCH
    A-->>B: V6_PUNCH_ACK
    Note over A,B: IPv6 socket 接管数据面，服务器退出数据路径
```

若双方都允许入站，`peer_id` 字典序较小的一端为 `listen`；只有一端允许入站时，该端
固定为 `listen`。

## 5. 会合服务器控制面状态

会合服务器主线程始终只监听控制端口。每个客户端条目由 `pairedWith`、
`nat4Joined/nat4Round` 和 `ipv6Joined` 等字段共同描述，因此以下是逻辑状态投影，并非
互斥枚举。

```mermaid
stateDiagram-v2
    [*] --> Absent
    Absent --> Registered: REG 或合法会话消息自动登记
    Registered --> Registered: REG 刷新 endpoint / seen
    Registered --> Paired: CONNECT 找到空闲目标
    Paired --> Paired: CONNECT 刷新双方公网端点

    Paired --> Nat4Waiting: NAT4_JOIN(round)
    Nat4Waiting --> Nat4Waiting: 对端未加入同一 round / NAT4_WAIT
    Nat4Waiting --> Nat4Ready: 双方同 round / NAT4_PEER
    Nat4Ready --> Nat4Waiting: 下一 round 的 NAT4_JOIN
    Nat4Waiting --> Paired: REG 重置 join 标志
    Nat4Ready --> Paired: REG 重置 join 标志

    Paired --> Ipv6Waiting: V6_JOIN(endpoint, inbound)
    Ipv6Waiting --> Ipv6Waiting: 对端尚未就绪 / V6_WAIT
    Ipv6Waiting --> Ipv6Ready: 双方就绪 / V6_PEER
    Ipv6Waiting --> Paired: REG 重置 join 标志
    Ipv6Ready --> Paired: REG 重置 join 标志

    Paired --> RelayWaiting: RELAY_JOIN 但目标暂未重新登记
    RelayWaiting --> RelayOffered: 目标到达且 Relay App 创建会话
    Paired --> RelayOffered: RELAY_JOIN 且目标已登记
    RelayOffered --> Paired: Relay App 独立承载数据

    Registered --> Absent: 合法 UNREG / client timeout
    Paired --> Absent: 合法 UNREG / client timeout
    Nat4Waiting --> Absent: 合法 UNREG / client timeout
    Nat4Ready --> Absent: 合法 UNREG / client timeout
    Ipv6Waiting --> Absent: 合法 UNREG / client timeout
    Ipv6Ready --> Absent: 合法 UNREG / client timeout
    RelayWaiting --> Absent: 合法 UNREG / client timeout
    RelayOffered --> Absent: 合法 UNREG / client timeout
```

关键规则：

- `LIST` 只返回 `pairedWith` 为空的客户端。
- `REG` 和 `CONNECT` 都携带本端已启用模式；首个有效 `CONNECT` 决定协商顺序。
- 双方能力没有交集时只向发起方返回 `no-common-traversal-mode`，不占用等待方。
- `CONNECT`、`NAT4_JOIN`、`V6_JOIN` 和 `RELAY_JOIN` 都会校验 room、Peer ID 和 token。
- 任一方已与第三方配对时返回 `peer-busy`。
- 客户端过期或注销后，其他条目中指向它的 `pairedWith` 会被清理。
- 合法 `UNREG` 还会通知 Relay App 立即停止包含该 Peer 的活动会话。

## 6. IPv4 Relay App 状态机

Relay App 与注册表分离。注册表确认双方身份和配对关系后才调用 Relay App；每对 Peer
使用一个 UDP socket、一个公网端口和一个工作线程。

```mermaid
stateDiagram-v2
    [*] --> NoSession
    NoSession --> WaitingHello: 分配端口、session ID、A/B access key并启动线程
    WaitingHello --> WaitingHello: 单端 RELAY_HELLO / 记录或更新端点
    WaitingHello --> Forwarding: A、B 均完成认证绑定
    Forwarding --> Forwarding: 转发已绑定端点的数据报
    Forwarding --> Forwarding: 合法 RELAY_HELLO / 允许认证重绑定

    WaitingHello --> Closing: 握手超过 client_timeout_seconds
    Forwarding --> Closing: 任一端空闲超过 client_timeout_seconds
    WaitingHello --> Closing: Peer 注销 / 服务停止
    Forwarding --> Closing: Peer 注销 / 服务停止
    Closing --> Finished: 停止 socket，线程退出
    Finished --> NoSession: 主线程 join 并归还端口
```

Relay 端口范围决定最大并发会话数。线程仅接收本会话端口的数据，且只转发来自已经
认证绑定端点的数据报；未知来源和 access key 错误的 `RELAY_HELLO` 会被丢弃。

### 6.1 Relay 控制面与数据面时序

```mermaid
sequenceDiagram
    participant A as Client A
    participant C as Rendezvous Control
    participant R as Per-pair Relay Thread
    participant B as Client B

    Note over A,B: 此图以 B 仍在注册表中为例
    A->>C: RELAY_JOIN(room, A, B, token)
    C->>R: 创建 session、UDP port、线程
    C-->>A: RELAY_OFFER(port, B, session, key-A)
    A->>R: RELAY_HELLO(session, A, key-A)

    B->>C: RELAY_JOIN(room, B, A, token)
    C-->>B: RELAY_OFFER(port, A, session, key-B)
    B->>R: RELAY_HELLO(session, B, key-B)

    R-->>A: RELAY_READY(session, B)
    R-->>B: RELAY_READY(session, A)

    loop Connected 数据面
        A->>R: IPv4 payload / KEEPALIVE / PADDING
        R->>B: 原样转发
        B->>R: IPv4 payload / KEEPALIVE_ACK / PADDING
        R->>A: 原样转发
    end
```

`RELAY_JOIN` 与 `RELAY_HELLO` 都是幂等的。Offer 丢失时服务器返回同一会话信息；
Hello 或 Ready 丢失时客户端继续发送 Hello，线程在双方已绑定后重新发送 Ready。
如果 B 已从注册表暂时消失，A 首次请求会收到 `RELAY_WAIT`；B 的 `RELAY_JOIN` 重新
登记并创建会话后，A 重发 `RELAY_JOIN` 即可取得同一会话的 Offer。

## 7. 超时、错误与停止汇总

| 位置 | 条件 | 后续状态/动作 |
| --- | --- | --- |
| 会合首次响应 | 5 秒无合法服务端响应 | 客户端 `Error` |
| 指定 Peer 等待 | `punch_timeout` 内未匹配 | 客户端 `Error` |
| 模式能力协商 | 双方没有共同启用模式 | 发起方立即 `Error`，等待方继续在线 |
| 空目标等待模式 | 未收到 PEER | 持续等待，直到 Stop |
| 普通 NAT | `punch_timeout` | 策略列表下一项或 `Error` |
| NAT4 | `punch_timeout` | 策略列表下一项或 `Error` |
| IPv6 直连 | `ipv6_fallback_timeout` | 策略列表下一项或 `Error` |
| IPv4 Relay 协商 | 独立的 `punch_timeout` | 策略列表下一项或 `Error` |
| 已连接客户端 | `peer_timeout` 无合法 Peer 流量 | 客户端 `Error` |
| 服务端注册表 | `client_timeout_seconds` 未刷新 | 删除客户端条目 |
| Relay 未就绪/已连接 | `client_timeout_seconds` 达到对应超时 | 线程退出并回收端口 |
| 任意客户端阶段 | 用户 Stop | 停止循环、关闭 socket、释放 TUN |
| 会合服务器停止 | running=false | 停止主循环和全部 relay 线程并 join |

## 8. 修改状态机时的同步清单

后续新增或调整协商阶段时，应同步检查：

1. 客户端是否保留了正确的 `matchedPeerId`、最终 socket 和最终 Peer 端点；
2. 控制消息是否只接受来自配置的会合服务器或已确认数据端点；
3. 服务端重复请求是否幂等，旧 round 或旧 session 是否会污染新连接；
4. Stop、超时、注销和服务重启是否都能唤醒阻塞收包并 join 线程；
5. Console、TUI、GUI、服务端 TUI、示例配置和本文档是否保持一致。
