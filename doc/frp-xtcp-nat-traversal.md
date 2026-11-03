# frp xTCP NAT 穿透逻辑参考

本文记录 EasyTunnel 新 NAT Punch 设计所参考的 frp xTCP 穿透逻辑。分析基于
本机 `D:\code\frp` 的 `dev` 分支，提交 `758f07d5`（2026-08-18）。以下路径均
相对于该 frp 仓库根目录。

2026-11-03 复核：EasyTunnel `be190a3`，本机 frp `d8cf1800`；frp 的
`pkg/nathole` 相比上述参考提交没有变化。本文末尾记录本次源码核对、受控复现和改进优先级。

## 总体流程

1. visitor 先向 frps 做目标和权限预检查，然后 client、visitor 分别执行 STUN
   探测，收集公网映射和本地网卡候选地址。
2. frp 比较同一端从不同 STUN 地址观察到的映射，分成 EasyNAT、HardNAT，并
   判断 IP/端口是否变化以及端口变化是否规律。
3. 双方通过 frps 上报映射。frps 创建 SID、配对双方，并根据两端 NAT 特征选择
   mode、sender/receiver 角色、TTL、延时、范围扫描或随机端口参数。
4. receiver 通常先执行低 TTL 预打洞或创建多个随机 UDP socket；sender 随后向
   对端候选地址、预测范围或随机端口发送带 SID 的探测包。
5. 收到合法 SID 后，receiver 回包，双方选择实际成功的 UDP socket 和远端地址。
   xTCP 随后在该 UDP 通道上建立 KCP/QUIC 会话。
6. client 把成功或失败报告给 frps。frps 会提高成功策略的分数，后续相同网络
   特征组合优先尝试历史上成功的策略。

## STUN 和 NAT 分类

frp 客户端默认只配置一个 `natHoleStunServer`。探测时先请求该 STUN；如果响应
包含 `OTHER-ADDRESS`，再使用同一个 UDP socket 请求这个地址，从而得到至少两个
公网映射。当前默认值是 `stun.easyvoip.com:3478`。如果最终不足两个映射，
`Prepare` 会失败。

frp 这里的 EasyNAT/HardNAT 是它自己的映射行为分类，不等同于传统 NAT1～NAT4：

- 公网 IP 和端口均不变：EasyNAT / `BehaviorNoChange`。
- IP 或端口发生变化：HardNAT。
- 只有端口变化，且观察到的最大端口差为 1～5：认为端口变化规律，可做范围预测。
- 映射 IP 与本机接口 IP 相同：标记为公网直连端，策略中优先作为 receiver。

EasyTunnel 与它的主要区别是：EasyTunnel 显式配置两台解析到不同公网 IPv4 的
STUN，并在真正的 punch socket 上依次探测；frp 的 `Discover` 完成后会关闭探测
socket，再绑定同一个本地 UDP 地址创建打洞 socket。

## 五种穿透策略

| Mode | 网络组合 | 主要策略 |
| --- | --- | --- |
| 0 | Easy + Easy | 一端 receiver 先用 TTL 7、TTL 4 或普通包预打洞，另一端 sender 再直接发送；会轮换角色和发送延时。 |
| 1 | Hard + Easy，Hard 端口变化规律 | Hard 固定为 sender；Easy receiver 对 Hard 的预测端口范围做低 TTL 扫描，范围上限为中心两侧各 10 个端口。 |
| 2 | Hard + Easy，Hard 端口变化不规律 | Hard 固定为 receiver，最多创建 256 个随机监听 socket；Easy sender 延时 3 秒后探测最多 1000 个随机目标端口。 |
| 3 | Hard + Hard，双方端口变化都规律 | 双方都扫描对端预测端口范围，其中一端作为 receiver 使用低 TTL，角色和 TTL 组合会轮换。 |
| 4 | Hard + Hard，仅一端端口变化规律 | 规律端固定为 sender，向对端探测最多 1000 个随机端口；另一端创建最多 256 个 socket，并向 sender 的小范围预测端口预打洞。 |

范围扫描每个端口间隔约 2 ms，随机扫描每个端口间隔约 15 ms。随机监听策略会在
基础超时上额外增加 30 秒。frps 还会让 sender 的策略响应比 receiver 晚约 1 秒，
保证 receiver 先进入准备状态。

策略选择不是固定只试一种：frps 为候选 mode/参数组合维护分数，选中一次会轻微
降分以便轮换，成功报告会加分。因此 TTL、角色和延时组合可以根据历史成功结果
逐步调整。

## 与 EasyTunnel 当前实现的对应关系

- EasyTunnel `direct` 大致对应 frp Mode 0；第 2/3 次 attempt 已有 TTL 7/4 和
  sender 延时，但 initiator/responder 对应的执行角色固定，尚未轮换方向。
- EasyTunnel regular/easy Range 对应 Mode 1。
- EasyTunnel hard/hard regular dual-range 对应 Mode 3。
- EasyTunnel 已实现 frp Mode 2 的有界基线：“多 socket receiver + 随机 sender”；
  第 2/3 次 attempt 分别使用 TTL 7/TTL 4 receiver 预打洞。
- EasyTunnel 已实现 Mode 4 的有界基线：regular 端随机扫描，random 端创建多 socket
  并扫描 regular 端预测中心附近的小范围；范围随 attempt/profile 从 ±2 渐进扩大。
  `NAT_ARMED` 屏障保证 receiver 先完成资源准备；重试 attempt 的 sender delay 最大
  1 秒且不超过剩余时间的四分之一，不复制 frp 固定 3 秒等待。
- EasyTunnel 会合服务目前只配对、交换观察结果和同步开始；frp 则由 frps 集中
  分类、分配双方动作并根据成功报告学习策略。EasyTunnel 不引入这套学习状态，
  继续根据当前 STUN 结果、profile 和 attempt 序号确定性生成计划。
- 可借鉴的边界包括：随机监听 256 个 socket、随机探测 1000 个端口、范围半径
  10、明确的发送节流、取消流程和成功 socket 接管。具体数值应经过 EasyTunnel
  实网测试后再确定，不直接照搬。

## frp 源码位置

| 相对路径 | 入口或内容 |
| --- | --- |
| `pkg/nathole/discovery.go:29` | `Discover`：STUN 探测；`:120` 处理 `OTHER-ADDRESS` 二次探测。 |
| `pkg/nathole/classify.go:42` | `ClassifyNATFeature`：Easy/Hard、映射变化和规律端口分类。 |
| `pkg/nathole/nathole.go:118` | `Prepare`：探测、分类、本地候选收集和 punch socket 准备。 |
| `pkg/nathole/nathole.go:192` | `MakeHole`：sender/receiver、低 TTL、范围/随机扫描、多 socket 竞争和 winner 选择。 |
| `pkg/nathole/analysis.go:39` | Mode 0～4 的参数组合；`:181` 建立策略优先级，`:289` 选择双方行为。 |
| `pkg/nathole/controller.go:153` | frps 会话配对和开始时序；`:300` 分类并生成双方策略；`:271` 接收成功报告。 |
| `pkg/msg/msg.go:192` | `NatHoleVisitor`、`NatHoleClient`、`NatHoleResp`、`NatHoleSid` 和 report 协议结构。 |
| `client/visitor/xtcp.go:252` | visitor 侧完整的 PreCheck → Prepare → ExchangeInfo → MakeHole 流程。 |
| `client/proxy/xtcp.go:47` | 被访问 client 侧收到 SID 后的探测、结果上报和 KCP/QUIC 接管。 |
| `server/proxy/xtcp.go:47` | xTCP proxy 注册到 NAT hole controller，并把 SID 交给 client。 |
| `pkg/config/v1/client.go:89` | 默认 STUN 地址配置。 |

## 2026-11-03 实现复核

以下为修改前 `be190a3` 的审查记录。后续修复：v4 握手已固定候选路径，sender 只接受
匹配 nonce 的 ACK，receiver 等待 CONFIRM，数据面支持确认重传；新增状态机回归及
Direct/Random/mixed 移交后的双向 payload 测试。其余项目的进度见 NAT TODO。
低 TTL receiver 的后续刷新也已改为低 TTL，确认报文使用恢复后的正常 TTL。

### 总体判断

主要模式已经齐备，优先改进成功确认和执行时序，其次才是增加策略覆盖面。
现有同 socket STUN/打洞/数据面复用、独立 control socket、带 session/attempt/token
的校验、可重传屏障、RAII socket 池、资源上限和 IPv6/Relay 回退都值得保留。
不需要为追齐 frp 引入服务端历史学习；下面的策略轮换也可以确定性实现。

### P1：sender 接受普通 PUNCH，可能与 receiver 选择不同端点

位置：EasyTunnel `adaptive_nat_traversal.cpp:711–746`、
`nat_punch_socket_pool.cpp:150–161`；frp `pkg/nathole/nathole.go:317–334`。

EasyTunnel 对 sender/receiver 使用相同成功条件：普通 PUNCH 和匹配 nonce 的 ACK
均可成功，随后立即关闭其他 socket。nonce 在整个 attempt 内共享，也没有候选端点
提名/确认阶段。frp 的 sender 则忽略非 response 报文，只等待 response。

一个可发生的交错是：

1. receiver 的 socket X 发出的预打洞 PUNCH 已排入 sender 接收队列。
2. sender 开始扫描，先向 receiver 的 socket Y 发出 PUNCH。
3. receiver 在 Y 收到该包，选择 Y、回 ACK，并关闭 X。
4. sender 先读到队列中的 X 的 PUNCH，向 X 回 ACK 后选择 X，忽略稍后到达的 Y ACK。

两端均可报告成功，但 sender 指向已关闭的 X。连接后 `nat_traversal.cpp:27`
以及 `tunnel_engine.cpp` 的源端点检查也不会自动把 Peer 修正为 Y。

本次用未修改的生产 sender 对象代码、真实 registry/双 STUN 和模拟双 socket receiver
构造上述顺序，复现输出为：

```text
sender_success=1 receiver_selected_y=1
sender_peer=127.0.0.1:62969 receiver_winner=127.0.0.1:62970
endpoint_mismatch=1
```

这是受控协议复现，不是公网 NAT 成功率测试；receiver 手动执行了当前实现的
“在 Y 收到 PUNCH → ACK → 保留 Y/关闭 X”动作。复现程序保存在本地
`build/nat-review-repro/repro.cpp`，使用本次测试构建的生产 `.obj` 链接。

建议先让执行角色参与收包状态机：sender 不因普通 PUNCH 提交成功，receiver
选定后固定候选，处理 ACK 丢失和重复包；进一步按候选 socket/endpoint 绑定确认，
保证双方提交同一条路径。不能只给每个 socket 加 nonce 而仍允许任意 PUNCH 提交成功。
需要补“X/Y 报文交叉到达、ACK 丢失/延迟、移交后双向 payload”回归。

### P1：低 TTL 后过早恢复普通探测，削弱 receiver-first 的目的

位置：EasyTunnel `adaptive_nat_traversal.cpp:96–133,527–542,615–694`；
frp `pkg/nathole/nathole.go:193–251,317–333`。

当前 receiver 在 ARMED 前低 TTL 发一轮，立即恢复 socket TTL；收到 START 后立刻
进入普通 TTL 的重复发送循环。此时 sender 正在等待最多 1 秒，尚未主动打开其入站路径。
因此日志里的 TTL 7/4 仅描述预打洞波次，并不表示 receiver 在等待 sender 的阶段保持
低 TTL 探测。frp 对配置 TTL 的 receiver 探测使用该 TTL，随后等待并以正常 TTL 回复。

代码时序差异可以确认；它在具体路由器上的影响尚需实网验证。对于需要避免提前到达
对端、防止产生不利过滤状态的网络，过早发普通包可能抵消低 TTL 方案的收益。

建议将 receiver 分为预打洞、等待 sender、确认三个阶段：等待期间只接收或按需刷新
低 TTL 探测，收到有效 sender PUNCH 后用正常 TTL 回 ACK。首轮普通双向打洞可继续保留。
验证应检查真正的发送时序和 TTL，不能只断言 `pre_punch_ttl` 日志或 socket TTL 恢复。

### P2：重试只扩围，没有跨模式回退，也没有角色方向轮换

位置：EasyTunnel `nat_punch_plan.cpp:82–177,264–371`；
frp `pkg/nathole/analysis.go:39–50,94–101,181–222`。

- `BuildNatPunchPlan` 由双方分类固定选择 mode；attempt/profile 改的是范围和资源。
  regular/easy 即使连续 Range 失败，仍不会切到多 socket/random；regular/regular
  也不会从 dual-range 切到 mixed。此前 TODO 中“扩围后进入 Random”的说法尚未落地。
- Direct 和 dual-range 始终由 initiator 当 sender；attempt 2/3 只改变 TTL，4+ 又回到
  无 TTL、无延迟，不能覆盖反向 receiver-first。frp 为这两类模式提供双向组合。
- 两个 STUN 端口差为 1～5 只是规律性的启发式证据，不能证明后续对端映射也遵循该规律。
  frp 同样使用启发式分类，但候选策略中保留跨模式尝试。

建议先保留首轮，后续按明确的 attempt 序列切换方向/模式，双方根据同一组观测和角色
计算互补计划。无需服务端学习；若增加计划版本或协商字段，应同步升级协议，避免混合
客户端选择不互补计划。也不要只把默认 attempt 数提高到 10，延长失败等待。

### P2：缺少局域网候选，同一 NAT 后可能依赖 hairpin

位置：EasyTunnel `adaptive_nat_traversal.cpp:407–413,720`；
frp `pkg/nathole/nathole.go:126–153,206–207`。

协议虽有 `localCandidates`，发送值仍为 `"-"`，计划只使用公网映射。
同一局域网的两台机器若路由器不支持 NAT hairpin，可能公网打洞失败后转 Relay，
即使它们本可通过私网直接通信。frp 会收集接口地址，并将辅助地址加入 sender 候选。

建议收集实际接口 IPv4 加同一个 punch socket 的绑定端口，排除回环和本项目 TUN
接口，交换少量候选并做认证探测。必须同时修改当前“来源 IP 必须等于 STUN 公网 IP”
的验证条件，允许经过候选认证确认的私网路径；只填候选字段无法完成支持。

### P2：总报文预算不等于突发限速

位置：EasyTunnel `adaptive_nat_traversal.cpp:106–115,637–694`；
frp `pkg/nathole/nathole.go:383–401`。

Range 和多 socket receiver 每个波次连续发送全部目标，中间没有逐包 pacing。
例如 balanced 第 3 次 mixed 为 `128 × 17 = 2176` 包/波；aggressive 第 3 次为
`256 × 33 = 8448` 包/波，第 4 次可达 `256 × 65 = 16640` 包/波。
波次间隔限制长期均值，但不能消除瞬时突发；预打洞循环也有相同问题。
frp 的范围扫描按端口间隔约 2 ms，但这也不等于它在所有路径上都有统一限速。

建议使用小批次发送与收包交替的调度器，限制短时突发，在批次间检查取消和 deadline。
随机扫描也按“下次发送时刻与剩余时间”决定 poll 超时，并记录实际探测目标数及 send
失败数。目前普通发送无论 `Send` 成败都增加 `datagramsSent`，预打洞则只累计成功发送，
二者统计口径不一致。默认 1000 个随机端口、15 ms 间隔至少需要约 15 秒，STUN/屏障
共用 timeout 时不能把配置的目标数当成实际完整扫描数。

### 次要改进与边界

- `QueryStunBinding` 不接收取消标志或外层 deadline，先完成内部重试再由调用方检查
  超时；DNS 解析也没有这个总 deadline 约束。建议把取消和总时间预算向下传递。
- 端口预测越界时直接拒绝策略，尚不能用观测端口、小范围边界候选或其他模式降级。
  排序可优先试预测中心与已知映射，之后向外扩展；当前范围始终从小到大扫描。
- multi-public-IP 与 random/random 是明确的未支持边界。frp 会在部分困难组合上作
  保底尝试，且能向多个候选 IP 探测，但这不表示它能可靠穿透任意双随机 NAT。
  这两项应排在确认逻辑、TTL 时序和 LAN 候选之后，保留 Relay 兜底。
- 第三次及更多独立目标观测可以评估分类置信度，但也会消耗映射并影响预测中心，
  不能简单追加 STUN 后继续使用原来的 `B + delta`。

### 验证结果与建议顺序

Windows/MSVC Release 下从当前源码重新构建并运行以下 6 项测试，全部通过：
`rendezvous_capabilities`、`stun_protocol`、`nat_punch_plan`、
`adaptive_nat_integration`、`nat_punch_socket_pool`、`nat_punch_transport`。
构建位于 `build/nat-review`，关闭 GUI/TUI 以避免原构建目录缺失 Qt 的影响。

现有集成测试用 loopback 加伪造 STUN 端口差触发各策略；没有真实模拟 NAT 的逐目的
映射、过滤、hairpin 或路由 TTL 消耗。双端成功检查之后也没有验证 winner 之间的
实际双向数据通信。因此“测试通过”仅证明当前覆盖的协议和资源路径，不能推出困难
NAT 下的成功率。上述额外受控复现确认了测试未覆盖的端点选择竞态。

建议实施顺序：成功确认与移交回归 → receiver-first 时序 → LAN 候选或确定性跨模式
回退（按实际使用场景选择）→ 突发限速和完整 NAT 仿真 → 公网分组对比。
本次只做审查、复现和文档修正，未修改生产打洞逻辑，也未进行公网成功率测量。
