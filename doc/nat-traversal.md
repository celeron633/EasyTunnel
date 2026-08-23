# Adaptive NAT Punch

EasyTunnel 的 IPv4 P2P 穿透使用两台独立 STUN 服务探测 NAT 映射，再由会合服务器
交换结果并同步双方开始时间。旧的精确端口 NAT 与 fixed-offset NAT4 已合并为一个
`nat_punch` 策略；NAT4 socket 池、固定 `+20`、round 和相关配置均已移除。

## 组件

```text
stun_client.cpp              RFC 8489 Binding 与映射分类
nat_punch_plan.cpp           确定性的双方互补打洞计划
adaptive_nat_traversal.cpp   STUN、会合屏障和 PUNCH 状态机
rendezvous_client.cpp        新 NAT 控制消息的收发与校验
rendezvous/registry.cpp      session/attempt 及短生命周期屏障状态
nat_traversal.cpp            连接后的 KEEPALIVE/PADDING 控制包
```

TUN 适配器、TUN IPv4 数据格式和连接后的收发循环没有变化。穿透成功的 punch socket
直接成为数据面 socket。

## 部署要求

客户端必须配置两台解析到不同公网 IPv4 的 STUN 服务：

```json
"punch_timeout": 30,
"nat_punch_attempt_limit": 3,
"nat_punch_profile": "balanced",
"stun_servers": [
  { "host": "stun-a.example.com", "port": 3478 },
  { "host": "stun-b.example.com", "port": 3478 }
]
```

推荐在两台公网服务器上分别部署 Coturn，并启用 `stun-only`。EasyTunnel 主动用同一
UDP socket 访问 A、B，因此不要求单台 Coturn 配置 RFC 5780 的 `OTHER-ADDRESS`，
也不要求 STUN 与会合服务器部署在一起。必须放行客户端到两台服务器的 UDP 3478
及其返回流量。

示例配置中的 `198.51.100.10`、`203.0.113.10` 是文档保留地址，实际部署必须替换。

## 映射探测

客户端依次向 STUN A、STUN B 发送 RFC 8489 Binding Request。请求使用操作系统
CSPRNG 生成的 96-bit transaction ID；响应必须来自请求目标、匹配 magic cookie 和
transaction ID，并包含合法的 `XOR-MAPPED-ADDRESS`。

两次请求始终复用即将打洞的 socket，STUN 完成后不会 close/rebind。分类规则为：

| A/B 观测 | 分类 | 当前计划 |
|---|---|---|
| 公网 IP、端口均相同 | endpoint-independent | Direct |
| 公网 IP 相同，端口差绝对值 1～5 | port-dependent-regular | Range/regular sender/dual-range |
| 公网 IP 相同，端口差大于 5 | port-dependent-random | 与 easy 配对时使用 Random sender/receiver |
| 公网 IP 不同 | multi-public-IP | 尚未实现，转后续策略 |

这里的分类只描述 mapping behavior，不宣称完成 RFC 5780 的 filtering behavior 测试。

## 会合协议

双方通过 `REG` / `CONNECT` 协商 `nat_punch` 后，会合服务器为配对生成：

- 128-bit 随机 `session_id`；
- 单调 `attempt_id`；
- initiator/responder 角色；
- 256-bit 随机 punch token；
- punch protocol version `2`。

这些字段随扩展后的 `PEER` 返回。新客户端不接受旧格式 `PEER`，因此客户端和会合
服务器需要一起升级。

每端随后从独立 punch socket 发送：

```text
NAT_INFO(room, self, peer, session, attempt, version,
         behavior, mapped_A_ip, mapped_A_port,
         mapped_B_ip, mapped_B_port, local_candidates, auth_token)
```

服务器只校验并转发双方报告，不预测端口。第一端收到 `NAT_WAIT`；两端报告完成后
分别收到 `NAT_PEER_INFO`。客户端本地用相同输入运行 `BuildNatPunchPlan`。

计划就绪后双方发送 `NAT_ARMED`。服务器先向单独到达的一端返回
`NAT_ARMED_ACK`，双方都 armed 后向两个 punch socket 发送 `NAT_START`。重复的
`NAT_INFO`、`NAT_ARMED` 会重发当前响应，以覆盖 UDP 丢包。

一次可重试失败后，双方从原控制 socket 重复发送：

```text
NAT_RETRY(room, self, peer, session, current_attempt, auth_token)
```

第一端收到 `NAT_RETRY_WAIT`；双方都请求后，服务器在同一 session 下分配新的全局
attempt ID 和新的 punch token，清空双方 NAT_INFO/ARMED 状态，再通过
`NAT_ATTEMPT(session, attempt, role, version, punch_token)` 同步给两端。丢失
`NAT_ATTEMPT` 的客户端继续发送旧 `NAT_RETRY` 时，服务器会重发当前 attempt；旧
attempt 的 `NAT_INFO`、`NAT_ARMED` 和 Peer PUNCH 均不再有效。

## 当前打洞计划

### easy/easy

双方都向对端 STUN B 的稳定公网端点发送 `PUNCH`。

### regular/easy

regular 一端只向 easy 端稳定端点发送，从而为这个目的地址创建下一条映射。easy
一端根据 regular 的两次观测计算：

```text
delta = mapped_B_port - mapped_A_port
predicted = mapped_B_port + delta
initial_span = min(10, abs(delta) + 5)
```

这是第一 attempt 的基础半径。重试时根据 profile 计算：

```text
span = min(profile_max_span, initial_span * attempt_scale)
```

然后扫描 `[predicted-span, predicted+span]`，并在 UDP 端口边界处截断。这里没有
固定偏移配置。

### hard/hard regular

双方都根据对端两次 STUN 映射的端口差计算预测中心，并同时执行上述 Range 扫描。
每端第一 attempt 的范围半径最多为 10；后续 attempt 按 profile 扩大。所有目标仍由
同一个 punch socket 发送。该策略是 frp Mode 3 的有界 dual-range 基线，暂不包含低 TTL
预打洞和延迟发送变体。

### Profile 和资源上限

| Profile | attempt scale（1/2/3/4+） | 最大半径 | 最小发送波次间隔 | 单 attempt 报文预算 |
|---|---:|---:|---:|---:|
| `balanced`（默认） | 1 / 2 / 4 / 8 | 48 | 200 ms | 16,384 |
| `aggressive` | 1 / 4 / 8 / 16 | 128 | 75 ms | 131,072 |

Direct 和 regular sender 仍只有一个目标，不会因为 profile 无意义地扩展。Range 与
dual-range 首轮保持原来的小范围，只有同步到新 attempt 后才扩大。一次发送波次会
遍历当前全部目标；如果 `punch_timeout` 很长，实际波次间隔会自动增加，保证发送量
不超过对应 profile 的单 attempt 报文预算。

Random sender/receiver 使用另一组受限资源：

| Profile | receiver socket（attempt 1/2/3+） | 随机目标（attempt 1/2/3+） | 每目标间隔 |
|---|---:|---:|---:|
| `balanced` | 32 / 64 / 128 | 256 / 512 / 1000 | 15 ms |
| `aggressive` | 64 / 128 / 256 | 512 / 1000 / 1000 | 5 ms |

### easy/random

port-dependent-random 一端固定为 receiver，在发送 `NAT_ARMED` 前绑定 profile 指定
数量的 UDP socket；easy 一端固定为 sender。`NAT_START` 后 receiver 从每个 socket
向 easy 的稳定端点发送认证 PUNCH，为这些 socket 创建到目标地址的 NAT 映射。sender
先尝试对端两次 STUN 的已知端口，再使用 CSPRNG 生成的起点和互质步长，在
1024～65535 内无重复探测限定数量的端口。

receiver 同时轮询整个 socket pool，首个收到合法 session/attempt/token 报文的 socket
成为 winner；其余 socket 立即关闭，winner 原样交给 TUN 数据面。sender 每个随机端口
只探测一次，receiver 的重复波次仍受 profile 报文预算限制。当前基线不使用低 TTL，
也不额外等待 frp 的 3 秒 sender delay。

socket pool 由单次 attempt 独占的 RAII 对象管理。成功时只释放 winner 的所有权；
屏障取消、PUNCH 超时、协议错误和资源不足等其他退出路径都会自动关闭主 socket 与
全部辅助 socket。自动化测试覆盖连续 12 轮最大 256-socket 池、确定性的中途创建失败、
真实会合屏障取消以及发送阶段超时。

### 尚未完成

mixed random/range、random/random 和 multi-public-IP 仍在
[新 NAT 穿透 TODO](新NAT穿透TODO.md) 中。遇到
这些组合会返回明确错误并继续 IPv6/Relay（若已启用），不会调用旧 fixed-offset
算法。

## Peer 确认

直连报文为：

```text
PUNCH(session, attempt, sender, nonce, punch_token)
PUNCH_ACK(session, attempt, sender, echoed_nonce, punch_token)
```

客户端只接受当前 session/attempt、指定 Peer ID、正确 token 且公网 IP 与对端 STUN
报告相符的报文。ACK 还必须回显本端 nonce。旧的 room/peer 两字段 PUNCH 格式不再兼容。
收到合法包的真实源端点成为最终 Peer，punch socket 原样交给现有 TUN 数据面。

Punch token 用于阻止无关 UDP 包误接管连接，不加密后续 TUN 流量。数据机密性仍需
在上层或后续加密层解决。

## 超时与回退

单次 adaptive NAT attempt（STUN、信息交换、屏障和直连确认）共享
`punch_timeout`。`nat_punch_attempt_limit` 范围为 1～10、默认 3，包含第一次尝试；
每次重试重新创建 punch socket、重新执行 STUN，并使用新 attempt ID 和 token。
STUN 超时、Peer 信息超时、屏障超时和 PUNCH 超时允许重试；无效配置、确定不支持的
NAT 组合和控制协议错误不会盲目重试。达到上限后按 `traversal_modes` 继续 IPv6 或
IPv4 Relay。`nat_punch_profile` 选择 `balanced` 或 `aggressive`；总墙钟时间由
每次 attempt 的 `punch_timeout` 和相邻 attempt 间的重试同步等待共同限制，实际发送量
另受 profile 报文预算限制。

`keepalive_interval` 和 `peer_timeout` 仅属于连接后的 NAT liveness，不参与打洞计划。

## 排障

- `STUN servers must resolve to different public IPv4 addresses`：A/B 实际解析到同一 IP。
- `STUN server did not respond`：检查 Coturn `stun-only`、UDP 3478 和安全组。
- `NAT mapping combination requires an unsupported mixed-random strategy`：当前只支持
  easy/random；random/regular、random/random 和 multi-public-IP 请启用 Relay。
- `Timed out at the NAT synchronization barrier`：检查客户端/会合服务器版本是否一致，
  以及 punch socket 到会合服务器的 UDP 返回流量。
