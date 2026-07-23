# IPv6 Fallback

跨 IPv4、IPv6 和 Relay 的客户端、服务端状态切换见
[state-machine.md](state-machine.md)。

IPv6 直连是四种可排序穿透模式之一，可以放在普通 NAT、增强 NAT4 或 Relay 的前后。
它不改变 TUN 内层协议：TUN 中仍只转发 IPv4 包，只是把这些包封装在 Peer 之间的
公网 IPv6 UDP 数据报中发送。

如果 IPv6 失败，引擎会继续尝试策略列表中位于它后面的已启用模式；IPv6 成功时仍
保持 Peer 直连，不经过 relay。

## 启用条件

只有同时满足以下条件才会尝试：

1. 本端 `traversal_modes` 中的 `ipv6` 为 `true`；
2. IPv4 会合通道已匹配到目标 Peer，且执行到策略列表中的 `ipv6`；
3. 本机对配置的 `ipv6_probe_host:ipv6_probe_port` 完成 IPv6 TCP 连接；
4. 探测连接实际选出的本地源地址属于 IPv6 GUA（`2000::/3`）；
5. 对端也完成以上检测并向会合服务器发送 `V6_JOIN`；
6. 两端至少有一端配置 `ipv6_accept_inbound=true`。

默认关闭该功能。只有两端都打开总开关并实际通过检测时，服务器才可能收到两端的
`V6_JOIN` 并下发端点，因此一端打开不会误进入 IPv6 数据面。

TCP 探针用于确认不只是“网卡上存在一个 IPv6 地址”，而是确实有 IPv6 默认路由并能
和指定公网服务通信。探针失败时不会继续交换本地 IPv6 地址。探针不承载隧道数据。
默认目标为阿里公共 DNS `2400:3200::1:53`，可按部署地区改为任意稳定的 IPv6 TCP
服务；主机字段既支持不带方括号的 IPv6 字面量，也支持具有 AAAA 记录的域名。

## 角色和状态机

```text
IPv4 exact-port Punch 失败
          |
          v
NAT4 socket pool 失败/关闭
          |
          v
检查 GUA + 公网 IPv6 TCP 探针
          |
          v
通过 IPv4 会合通道发送 V6_JOIN
          |
          v
服务器等待双方就绪并选择 listen/connect 角色
          |
          v
connect 端发送 V6_PUNCH -> listen 端回复 V6_PUNCH_ACK
          |
          v
同一个 IPv6 UDP socket 接管 TUN 数据面
```

角色选择规则：

- 只有一端允许主动入站时，该端为 `listen`，另一端为 `connect`；
- 两端都允许主动入站时，`peer_id` 字典序较小的一端为 `listen`，保证双方得到稳定且
  唯一的角色；
- 两端都不允许主动入站时，服务器返回 `ipv6-no-inbound-peer`，本轮失败。

`connect` 端首先从最终数据 socket 向 `listen` 端发送 UDP，从而为常见的光猫 IPv6
状态防火墙建立出站流。`listen` 端只有收到来源地址、端口、room 和 peer ID 都匹配的
`V6_PUNCH` 后才确认，并从同一源端口回复。两端都不允许主动入站的同步打洞暂不实现。

## 会合服务器是否参与步骤 4～6

步骤 4 需要会合服务器参与，步骤 5～6 不需要：

- 会合服务器适合完成双端开关确认、GUA/端口交换、至少一端允许入站的检查以及确定性
  角色分配；
- `V6_PUNCH`、确认包、keepalive 和全部 TUN 流量都由 Peer 直接通信；
- IPv6 直连确认后，客户端注销临时 IPv4 控制会话，服务器不在数据路径上。

这种划分避免新增中继负载，也避免一端仅凭旧的 Peer 信息单方面启用 Fallback。

## 控制协议

IPv4 会合通道新增消息：

| 方向 | 消息 | 字段 |
| --- | --- | --- |
| Client → Server | `V6_JOIN` | room、peer、target、GUA、UDP port、accept inbound、token |
| Server → Client | `V6_WAIT` | 无；对端尚未就绪 |
| Server → Client | `V6_PEER` | 对端 GUA、UDP port、peer ID、本端角色 |

IPv6 Peer 通道新增消息：

| 方向 | 消息 | 字段 |
| --- | --- | --- |
| connect → listen | `V6_PUNCH` | room、peer ID |
| listen → connect | `V6_PUNCH_ACK` | room、peer ID |

服务器只接受格式正确的 GUA、1～65535 端口和 `0/1` 入站标记。Peer 侧只接受会合服务器
指定的完整 IPv6 地址和端口，并继续验证 room 与 peer ID。客户端和会合服务器需要同步
升级；旧服务器会忽略 `V6_JOIN`，最终表现为 Fallback 超时。

## 配置

| 配置项 | 默认值 | 范围 | 含义 |
| --- | ---: | ---: | --- |
| `traversal_modes` 中的 `ipv6` | `false` | `true/false` | 启用 IPv6 直连并确定其顺序 |
| `ipv6_accept_inbound` | `false` | `true/false` | 本端防火墙/光猫允许主动入站 IPv6 UDP |
| `ipv6_listen_port` | `0` | 0～65535 | 数据 socket 本地端口；0 为系统自动分配 |
| `ipv6_probe_host` | `2400:3200::1` | IPv6 地址或 AAAA 主机名 | 用于验证公网 IPv6 并选择实际出站 GUA |
| `ipv6_probe_port` | `53` | 1～65535 | 探针使用的 TCP 端口 |
| `ipv6_fallback_timeout` | `15` | 1～120 秒 | 端点交换和直连确认的等待时间 |

无界面客户端在 `tunnel.conf` 的 `traversal_modes` 中启用该模式。TUI 和 GUI 的
Settings 页面在 Traversal strategy 表格中提供开关和排序，IPv6 参数位于单独分组。

如果配置固定端口，需要同时在操作系统防火墙和光猫 IPv6 防火墙中允许该 UDP 端口。
`ipv6_accept_inbound=true` 是管理员对网络策略的声明，程序不会自动修改公网入站规则。

## 日志与排查

成功路径会依次出现类似日志：

```text
Trying IPv6 direct connection
IPv6 GUA connectivity verified: 240e::1234
IPv6 peer node-b at [240e::5678]:41000, role=connect
IPv6 fallback confirmed with [240e::5678]:41000
```

常见失败：

- `No reachable IPv6 GUA`：没有 GUA、没有 IPv6 默认路由、主机名没有 AAAA 记录，或
  配置的 TCP 探针端点不可达；
- `ipv6-no-inbound-peer`：双方均未声明允许主动入站；
- `IPv6 fallback timed out`：对端未启用、服务器版本过旧、端口被防火墙拦截，或双方
  到达 Fallback 的时间差超过配置超时；
- `Cannot bind IPv6 fallback socket`：固定端口被占用，改用 `0` 或另一个端口。
