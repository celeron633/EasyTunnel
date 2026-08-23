# frp xTCP NAT 穿透逻辑参考

本文记录 EasyTunnel 新 NAT Punch 设计所参考的 frp xTCP 穿透逻辑。分析基于
本机 `D:\code\frp` 的 `dev` 分支，提交 `758f07d5`（2026-08-18）。以下路径均
相对于该 frp 仓库根目录。

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

- EasyTunnel `direct` 大致对应 frp Mode 0 的核心路径，但尚未轮换低 TTL、角色和
  延时组合。
- EasyTunnel regular/easy Range 对应 Mode 1。
- EasyTunnel hard/hard regular dual-range 对应 Mode 3。
- frp Mode 2 的“多 socket receiver + 随机 sender”和 Mode 4 的混合策略仍是
  EasyTunnel Roadmap 中的后续工作。
- EasyTunnel 会合服务目前只配对、交换观察结果和同步开始；frp 则由 frps 集中
  分类、分配双方动作并根据成功报告学习策略。
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

