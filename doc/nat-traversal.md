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
| 公网 IP 相同，端口差绝对值 1～5 | port-dependent-regular | Range/regular sender |
| 公网 IP 相同，端口差大于 5 | port-dependent-random | 尚未实现，转后续策略 |
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

## 当前打洞计划

### easy/easy

双方都向对端 STUN B 的稳定公网端点发送 `PUNCH`。

### regular/easy

regular 一端只向 easy 端稳定端点发送，从而为这个目的地址创建下一条映射。easy
一端根据 regular 的两次观测计算：

```text
delta = mapped_B_port - mapped_A_port
predicted = mapped_B_port + delta
span = min(10, abs(delta) + 5)
```

然后扫描 `[predicted-span, predicted+span]`，并在 UDP 端口边界处截断。这里没有
固定偏移配置。

### 尚未完成

hard/hard regular、random receiver 和 mixed random/range 仍在 `todo.md` 中。遇到
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

一次 adaptive NAT 流程（STUN、信息交换、屏障和直连确认）共享 `punch_timeout`。
当前只执行一次 attempt；多 attempt 限制和 aggressive profile 尚未实现。失败后按
`traversal_modes` 顺序继续 IPv6 或 IPv4 Relay。

`keepalive_interval` 和 `peer_timeout` 仅属于连接后的 NAT liveness，不参与打洞计划。

## 排障

- `STUN servers must resolve to different public IPv4 addresses`：A/B 实际解析到同一 IP。
- `STUN server did not respond`：检查 Coturn `stun-only`、UDP 3478 和安全组。
- `Random or multi-public-IP NAT plan is not implemented yet`：启用 Relay，或等待
  aggressive 模式完成。
- `Regular symmetric NAT on both peers is not implemented yet`：当前 hard/hard 组合
  尚未接入 dual-range。
- `Timed out at the NAT synchronization barrier`：检查客户端/会合服务器版本是否一致，
  以及 punch socket 到会合服务器的 UDP 返回流量。
