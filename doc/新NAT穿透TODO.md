# 新 NAT 穿透 TODO

## 目的

使用基于双 STUN 探测和动态打洞计划的新 NAT Punch，彻底替换旧的普通 NAT/NAT4
实现。新方案参考 frp xTCP 的 NAT 探测和自适应打洞思路，但保持 EasyTunnel 的
TUN-over-UDP 数据面不变。

具体目标：

- 使用两台独立公网 IPv4 STUN 服务探测真实 NAT 映射行为。
- STUN 探测、Peer 打洞和成功后的隧道数据面复用同一个 UDP socket。
- 会合服务器只负责配对、信息交换、认证和开始同步，不负责预测端口。
- 根据双方 NAT 映射动态选择 Direct、Range、Random 等互补打洞计划。
- 不保留旧 fixed-offset/manual 算法和旧 NAT/NAT4 穿透协议。
- 困难 NAT 直连失败后，仍可按配置继续尝试 IPv6 或 IPv4 Relay。
- TUN 适配器、TUN 地址配置和现有隧道数据包格式不在本次重构范围内。

## 当前完成情况

### 已完成

#### STUN 探测与 NAT 分类

- [x] 增加结构化 `stun_servers` 配置，并在 GUI/TUI 中提供 STUN A、STUN B
      地址和端口编辑项。
- [x] 实现 RFC 8489 Binding Request、随机事务 ID 校验和
      `XOR-MAPPED-ADDRESS` 解析。
- [x] 使用同一个 punch socket 依次探测两台解析到不同公网 IPv4 的 STUN 服务。
- [x] 识别 endpoint-independent、regular port-dependent、random port-dependent
      和 multi-public-IP 映射。
- [x] 覆盖 STUN 协议解析、异常报文、超时重试和 NAT 映射分类测试。

#### 会合协议

- [x] 为每次配对生成高熵 `session_id`、单调 `attempt_id`、双方角色和随机
      punch token。
- [x] 增加 `NAT_INFO` / `NAT_PEER_INFO`，交换双方 STUN 映射结果。
- [x] 增加 `NAT_ARMED` / `NAT_START` 屏障，在双方准备完成后统一开始打洞。
- [x] 为控制消息增加版本、字段长度、认证、会话、attempt 和来源端点校验。
- [x] 会合服务器只保存短生命周期的 attempt 状态，不再分类 NAT 或推导打洞端口。
- [x] 删除 `NAT4_JOIN` / `NAT4_WAIT` / `NAT4_PEER` 和服务端 NAT4 round 状态。

#### 打洞计划与确认

- [x] 两端使用确定性的 `BuildNatPunchPlan` 生成互补计划。
- [x] 实现 Direct：双方映射稳定时，直接向已知公网端点发送打洞包。
- [x] 实现 regular/easy Range：根据两次 STUN 端口差计算预测中心和动态扫描范围。
- [x] 实现 Mode 3 基线 hard/hard regular：双方执行有界 dual-range 扫描。
- [x] 移除固定 `+20`、固定端口区间和 manual/fixed-offset 回退算法。
- [x] `PUNCH/PUNCH_ACK` 携带 session、attempt、sender、nonce 和认证 token。
- [x] 拒绝旧 room/peer 两字段 PUNCH 格式，只接受当前 attempt 的合法报文。
- [x] 将成功接收打洞包的 socket 和真实来源端点原样交给现有数据面。

#### 引擎、配置和测试

- [x] 分离会合控制 socket 和 punch socket。
- [x] 将旧 `nat`/`nat4` 能力收敛为一个 `nat_punch` 策略。
- [x] 删除旧 NAT4 端口范围、offset、round timeout 和 round limit 配置。
- [x] 失败后按照 `traversal_modes` 顺序继续尝试 IPv6 或 IPv4 Relay。
- [x] 更新 Console、GUI、TUI、示例配置、README、状态机和部署文档。
- [x] 增加本机双 STUN、真实会合 registry 和双客户端打洞集成测试。

### 待完成

#### frp xTCP 困难 NAT 策略

- [ ] 实现 Mode 2 类 Random receiver：限制随机监听 socket 数、随机目标端口数和
      UDP 发送速率。
- [ ] 实现 Mode 4 类 mixed random/range：一端范围预测，另一端多 socket 随机探测。
- [ ] 评估并实现低 TTL 预打洞以及 sender/receiver 延迟发送组合。
- [ ] 收集打洞成功报告，按 NAT 特征记录策略成功率并动态调整尝试顺序。

#### 会合与候选地址

- [ ] 收集本机私网 IPv4 候选地址，通过现有 `localCandidates` 字段交换并优先尝试
      同局域网直连。
- [ ] 增加多 attempt 控制、attempt limit，以及 balanced/aggressive 策略配置。
- [ ] 新旧客户端或会合服务器协议不一致时返回明确错误，不进行静默误判。

#### UI、测试与实网验证

- [ ] GUI/TUI 增加 STUN 连通性测试入口，并显示两次公网映射和分类结果。
- [ ] 补充计划对称性、UDP 端口边界、乱序/重复控制消息和过期 attempt 测试。
- [ ] 完成本机双客户端直连、IPv6、Relay、重连和关闭流程回归。
- [ ] 使用两台公网 STUN，在多种家用路由器和手机热点上记录分类、策略及成功率。
- [ ] 对 Random 策略执行 socket、带宽、突发速率和超时压力测试。
