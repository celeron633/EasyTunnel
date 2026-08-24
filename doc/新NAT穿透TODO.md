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
- 不收集 NAT 特征历史或打洞成功报告，不实现服务端策略学习；计划只由本次 STUN
  结果、profile 和 attempt 序号确定性生成。
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
- 使用部署在不同公网 IPv4 服务器上的两套独立 STUN 服务进行实网验证。
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

状态：已完成。

- [x] 困难 NAT 端创建有上限的随机监听 socket，并选择第一个成功 socket。
- [x] 对端在限定数量、速率和时间预算内探测不重复的随机目标端口。
- [x] 增加重复执行、资源耗尽、超时和取消流程压力测试；socket pool 使用 RAII
      统一管理，只有 winner 显式移交数据面。

### 阶段 5：frp Mode 4 类 mixed random/range

状态：进行中。

- [x] regular 端固定为随机端口 sender；random 端使用受控多 socket，并扫描 regular
      端的小范围预测端口。
- [x] mixed range 按 profile/attempt 从 ±2 渐进扩大，且共享 socket、发送间隔和
      单 attempt 报文预算上限。
- [x] 复用 RAII socket pool，失败时释放全部 socket 并继续 IPv6/Relay。
- [x] attempt 2/3 分别实现 TTL 7/TTL 4 receiver 预打洞，以及最大 1 秒且受剩余
      attempt 时间约束的可取消 sender delay。
- [ ] 在公网 regular/random 样本上对比无延迟、TTL 7 和 TTL 4 的实际收益。

### 阶段 6：发布收敛

状态：待开始。

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
- [x] 实现 Mode 4 基线 regular/random：regular 端随机扫描，random 端通过多 socket
      扫描预测中心附近的小范围。
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
- [x] 实现 easy/random 的 Mode 2 基线：random 端使用 32～256 个受限 receiver
      socket，easy 端按 profile 探测 256～1000 个不重复随机端口。
- [x] 多 socket 使用系统 poll 等待首个认证 PUNCH/PUNCH_ACK；关闭所有 loser，
      将 winner socket 原样交给 TUN 数据面。
- [x] 集成测试覆盖 Aggressive 最大 256 socket、1000 随机目标、真实 registry 屏障、
      attempt/token 校验和 winner 接管。
- [x] socket pool 测试连续 12 轮创建/释放 256 socket，并注入辅助 socket 创建失败；
      集成测试覆盖最大池在 barrier 取消和 PUNCH 超时后的自动回收。
- [x] 增加低 TTL unit test，覆盖 `IP_TTL` 设置、显式/析构恢复、无效 socket 和可取消
      sender delay；集成测试覆盖 Mode 2 TTL 4 与 Mode 4 TTL 7 双端成功路径。
- [x] 将 `NAT_ARMED` 屏障限制为不超过 8 秒，记录本端 ACK 和屏障耗时；集成测试覆盖
      首批 ARMED 报文丢失重传、正常启动以及对端未就绪时的快速超时。
- [x] 会合状态机测试覆盖提前/重复 `NAT_ARMED`、重复 `NAT_INFO`/`NAT_RETRY`、
      `NAT_START` 丢失重发，以及过期和未来 attempt 消息拒绝。

### 待完成

#### frp xTCP 困难 NAT 策略

- [x] 实现 Mode 4 类 mixed random/range，并通过假 STUN + 真实 registry 双端集成测试。
- [x] 实现低 TTL 预打洞以及 sender/receiver 延迟发送组合；TTL 设置/恢复、角色轮换、
      短超时上限和取消等待均有 unit test。
- [ ] 收集公网样本，评估低 TTL 与 sender delay 的实际收益并决定最终默认轮换顺序。

#### 会合与候选地址

- [ ] 收集本机私网 IPv4 候选地址，通过现有 `localCandidates` 字段交换并优先尝试
      同局域网直连。
- [x] 新旧客户端或会合服务器协议不一致时返回明确错误，不进行静默误判。

#### UI、测试与实网验证

- [x] GUI/TUI 增加 STUN 连通性测试入口，并显示两次公网映射、delta、分类和
      本端可用计划提示。
- [ ] 补充计划对称性和 UDP 端口边界测试。
- [ ] 完成本机双客户端直连、IPv6、Relay、重连和关闭流程回归。
- [ ] 使用两台公网 STUN，在多种家用路由器和手机热点上记录分类、策略及成功率。
- [x] 对 Random 策略执行 socket 生命周期、资源耗尽、取消和超时压力测试；带宽与
      突发速率继续由 profile 报文预算和发送间隔约束，发布前仍需补公网长期负载样本。
