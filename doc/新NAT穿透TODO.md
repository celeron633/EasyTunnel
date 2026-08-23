# 新 NAT 穿透 TODO

## 目的

使用基于双 STUN 探测和动态打洞计划的新 NAT Punch，彻底替换旧的普通 NAT/NAT4
实现。新方案参考 frp xTCP 的 NAT 探测和自适应打洞思路，但保持 EasyTunnel 的
TUN-over-UDP 数据面不变。

frp xTCP 的流程、五种模式和源码索引见
[frp xTCP NAT 穿透逻辑参考](frp-xtcp-nat-traversal.md)。

具体目标：

- 使用两台独立公网 IPv4 STUN 服务探测真实 NAT 映射行为。
- STUN 探测、Peer 打洞和成功后的隧道数据面复用同一个 UDP socket。
- 会合服务器只负责配对、信息交换、认证和开始同步，不负责预测端口。
- 根据双方 NAT 映射动态选择 Direct、Range、Random 等互补打洞计划。
- 不保留旧 fixed-offset/manual 算法和旧 NAT/NAT4 穿透协议。
- 困难 NAT 直连失败后，仍可按配置继续尝试 IPv6 或 IPv4 Relay。
- TUN 适配器、TUN 地址配置和现有隧道数据包格式不在本次重构范围内。

## 开发 Roadmap

### 阶段 0：新协议和基础策略

状态：已完成。

- 双 STUN 同 socket 探测和 NAT mapping behavior 分类。
- 精简会合协议以及 session、attempt、token 和开始屏障。
- easy/easy Direct、regular/easy Range 和 hard/hard regular dual-range。
- PUNCH 认证、winner socket 接管以及 IPv6/Relay 顺序回退。

### 阶段 1：公网实网验证

状态：进行中。

- GitHub Actions 对 `new-nat-punch` 同时构建 Windows/Linux 产物并运行测试。
- 使用 `txy2.lvsrobot.top:3479` 和 `txy.lvsrobot.top:3479` 作为两台独立 STUN。
- 2026-08-23 完成第一组公网 Direct 样本：本端 A/B 映射均为
  `218.17.213.3:60738`、delta 为 0，计划为 `direct`、目标数为 1，最终从
  `163.125.4.38:1801` 收到认证打洞包并建立隧道。
- 上述样本确认双方都满足当前的 endpoint-independent 映射条件；这只描述
  映射行为，不等同于完整测出了传统 NAT1/NAT2/NAT3/NAT4 的过滤类型。
- 实网类型样本持续补充，但不阻塞后续开发；暂时找不到的组合先用假 STUN 映射、
  本地双客户端和受控端口变化测试覆盖，最终发布前再补真实网络回归。
- 在家庭宽带、公司网络和手机热点之间测试 Direct、Range 和 dual-range。
- 记录双方 STUN A/B 映射、delta、计划名称、目标数、成功端点和总耗时。
- 验收条件：现有三种策略均有真实网络样本，失败可以定位到 STUN、分类、屏障或 PUNCH。

### 阶段 2：诊断和可观测性

状态：已完成。

- [x] GUI/TUI 增加异步 STUN 测试入口，使用同一 socket 显示 A/B 映射、delta、
      分类和本端可用计划提示。
- [x] 为一次穿透生成可关联的单行 attempt 日志摘要，包含 session、attempt、
      双方 peer、角色、分类、计划、目标数、成功端点、耗时和结果。
- [x] 区分 STUN 超时、STUN 错误、策略不支持、屏障超时、PUNCH 超时、停止和
      控制错误，并记录每种 traversal strategy 的结果及下一回退项。

### 阶段 3：多 attempt 策略框架

状态：已完成。

- [x] 增加 `nat_punch_attempt_limit`（默认 3、最大 10）；双方同步重试后使用新的
      attempt ID 和 punch token，并拒绝旧 attempt 报文。
- [x] 增加 balanced/aggressive profile，控制范围大小、发送间隔和单 attempt 报文预算。
- [x] Direct 保持单目标；Range/dual-range 从首轮小范围开始，后续 attempt 逐步扩大，
      达到上限后再进入 Random 或其他回退策略。

### 阶段 4：frp Mode 2 类 Random receiver

状态：待开始。

- 困难 NAT 端创建有上限的随机监听 socket，并选择第一个成功 socket。
- 对端在限定数量、速率和时间预算内探测随机目标端口。
- 增加 socket、带宽、突发速率和取消流程压力测试。

### 阶段 5：frp Mode 4 类 mixed random/range

状态：待开始。

- regular 一端执行范围预测，random 一端使用受控多 socket 策略。
- 评估低 TTL 预打洞和 sender/receiver 延迟发送组合的实际收益。
- 保证任一策略失败后都能及时释放 socket 并继续 IPv6/Relay。

### 阶段 6：策略学习和发布收敛

状态：待开始。

- 收集不含敏感端点明文的成功/失败统计，按 NAT 特征调整策略优先级。
- 完成协议不兼容、乱序、重复、过期 attempt 和资源上限测试。
- 完成 Windows/Linux、重连、关闭以及长期 liveness 回归后再合并主分支。

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
- [x] 增加结构化 NAT Punch attempt 结果和单行摘要，失败阶段使用稳定分类名称。
- [x] 每个 NAT Punch、IPv6、IPv4 Relay 尝试均记录结果、耗时和下一回退策略。
- [x] Console/GUI/TUI 共用 `nat_punch_attempt_limit`，引擎只对瞬时失败执行有界重试。
- [x] 会合服务器通过 `NAT_RETRY_WAIT` / `NAT_ATTEMPT` 为双方同步新 attempt，
      重试丢包时可重发当前状态，旧 NAT_INFO 会被拒绝。
- [x] Console/GUI/TUI 共用 `nat_punch_profile`；Balanced 最大 Range 半径 48、
      Aggressive 最大半径 128，并通过波次间隔和单 attempt 报文预算限制发送量。
- [x] Range/dual-range 按本地 attempt 序号确定性扩围；Direct 和 regular sender
      继续使用单目标计划。

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
- [ ] 新旧客户端或会合服务器协议不一致时返回明确错误，不进行静默误判。

#### UI、测试与实网验证

- [x] GUI/TUI 增加 STUN 连通性测试入口，并显示两次公网映射、delta、分类和
      本端可用计划提示。
- [ ] 补充计划对称性、UDP 端口边界、乱序/重复控制消息和过期 attempt 测试。
- [ ] 完成本机双客户端直连、IPv6、Relay、重连和关闭流程回归。
- [ ] 使用两台公网 STUN，在多种家用路由器和手机热点上记录分类、策略及成功率。
- [ ] 对 Random 策略执行 socket、带宽、突发速率和超时压力测试。
