# New NAT Punch TODO

目标：用基于双 STUN 探测和动态打洞计划的新 NAT Punch 替换现有普通 NAT/NAT4
实现。TUN 适配器、TUN 地址配置、隧道数据包格式、IPv6 直连和 IPv4 Relay 不在
本次重构范围内。

## 已确定的边界

- [x] 开发分支使用 `new-nat-punch`。
- [x] 使用两台独立公网 IPv4 STUN 服务；STUN 不合并进会合服务器。
- [x] STUN 服务采用标准 RFC 8489 Binding，推荐部署 Coturn `stun-only`。
- [x] 不保留 fixed-offset/manual 兼容模式；新实现稳定后直接删除旧 NAT4。
- [x] STUN 探测和 Peer 打洞必须复用同一个 UDP socket，避免 NAT 映射失效。

## Phase 1：STUN 基础能力

- [x] 增加结构化 `stun_servers` 客户端配置，并映射到运行时配置。
- [x] 实现 RFC 8489 Binding Request、事务 ID 校验和 XOR-MAPPED-ADDRESS 解析。
- [x] 实现同一 socket 依次探测两台 STUN 服务的客户端。
- [x] 根据两次映射结果识别 endpoint-independent、regular port-dependent、
      random port-dependent 和 multi-public-IP。
- [x] 覆盖协议解析、异常报文和 NAT 映射分类单元测试。
- [x] GUI/TUI 增加 STUN A、STUN B 地址和端口编辑项。
- [ ] GUI/TUI 增加 STUN 连通性测试入口。

## Phase 2：会合协议瘦身

- [x] 为每次配对生成高熵 `session_id`、单调 `attempt_id` 和双方角色。
- [x] 增加 `NAT_INFO` / `NAT_PEER_INFO`，交换 STUN 映射及本地候选地址。
- [x] 增加 `NAT_ARMED` / `NAT_START` 屏障，统一双方开始时间。
- [x] 给新控制消息补充版本号、字段上限、认证和过期校验。
- [x] 会合服务器只保存短生命周期的 attempt 状态，不再推导打洞端口。
- [x] 删除 `NAT4_JOIN` / `NAT4_WAIT` / `NAT4_PEER` 及服务器 NAT4 round 状态。

## Phase 3：自适应打洞计划

- [x] 两端使用确定性的 `BuildPunchPlan`，相同输入必须得到互补计划。
- [x] 实现 Direct：映射稳定时双方对已知公网端点打洞。
- [x] 实现 Range：按两次 STUN 端口差动态计算预测范围，移除固定 `+20`。
- [ ] 实现 Random receiver：限制 socket 数、目标探测数和 UDP 突发速率。
- [ ] 实现 Mixed random/range 和 hard/hard regular 策略。
- [x] PUNCH2/PUNCH2_ACK 携带 session、attempt、sender、nonce 和认证 token。
- [x] 只接受当前 attempt 的包；获胜 socket 原样交给现有数据面。

## Phase 4：引擎切换与配置收敛

- [x] 将会合控制 socket 与 punch socket 分离。
- [x] 将 `nat`/`nat4` 合并为一个 adaptive NAT Punch 能力和一次策略尝试。
- [ ] 保留 `punch_timeout`，增加 attempt limit 和 balanced/aggressive profile。
- [x] 删除 `nat4_source_port_start`、`nat4_source_port_count`、
      `nat4_peer_port_offset`、`nat4_round_timeout`、`nat4_round_limit`。
- [x] 更新 GUI、TUI、示例配置、README、状态机和部署文档。
- [x] 旧配置中只要 `nat` 或 `nat4` 开启，就迁移为 adaptive NAT Punch 开启。

## Phase 5：验证与发布

- [x] 本机假双 STUN + 真实会合 registry 双客户端集成测试，覆盖 winner socket 接管。
- [ ] 单元测试覆盖计划对称性、端口边界、乱序/重复控制消息和过期 attempt。
- [ ] 本机双客户端回归：直连、IPv6、Relay、重连和关闭流程。
- [ ] 双公网 STUN + 多种家用路由器/手机热点实测并记录分类与成功率。
- [ ] 对随机端口策略做 socket/带宽上限和超时压力测试。
- [ ] 新旧客户端与会合服务器版本不兼容时返回明确错误，不静默误判。
