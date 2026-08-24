# EasyTunnel 后续 Roadmap

## 当前阶段

新 NAT Punch 的核心功能已经完成，NAT4（`port-dependent-regular`）到 NAT3
（`endpoint-independent`）的实网样本可以在 1 秒内完成直连。当前阶段不继续调整
正常路径的打洞参数，优先使用现有版本扩大实网测试范围，观察长期连接、重连、关闭、
IPv6 和 IPv4 Relay 回退是否稳定。

服务端策略学习不在后续计划中。每次打洞计划继续只根据本次双 STUN 结果、profile 和
attempt 序号确定性生成。

## 后续优化

以下事项暂不立即开发，待实网测试积累足够样本后按优先级实施。

### 1. 发布前回归和边界测试

- 补充预测端口接近 `1` 和 `65535` 时的裁剪测试。
- 覆盖正负端口 delta、Range/dual-range 边界以及双方计划互补性。
- 在 Windows/Linux 上回归 Direct、Range、dual-range、random/mixed、重连、主动关闭、
  Peer timeout、IPv6 和 IPv4 Relay。

### 2. 同局域网候选地址

- 收集本机可用的私网 IPv4 地址及 punch socket 本地端口。
- 通过现有 `localCandidates` 字段交换候选地址，并校验候选格式和数量上限。
- 在公网 NAT Punch 目标之外优先尝试同网段地址，避免依赖路由器 NAT hairpin。

### 3. STUN 探测容错和延迟

- 使用同一个 punch socket 管理两台 STUN 的 transaction ID，交错发送请求并统一收包，
  避免一台 STUN 丢包时完全串行等待。
- 评估支持第三台备用 STUN；只使用解析到不同公网 IPv4 且最先成功的两台进行分类。
- 保持 STUN 探测、Peer 打洞和成功后数据面复用同一个 socket 的约束。

### 4. 断线后自动重新打洞

- Peer timeout 或 NAT 映射失效后，重新注册并创建新的 session/attempt。
- 使用有限次数和退避间隔，避免网络长期不可用时持续高频重试。
- 区分用户主动 Disconnect、对端 `PEER_CLOSE` 和需要自动恢复的网络异常。

### 5. Disconnect 可靠性

- 评估将三份 `PEER_CLOSE` 改为短间隔发送，降低突发丢包导致全部丢失的概率。
- 如实网仍出现关闭延迟，再增加 `PEER_CLOSE_ACK`；Peer timeout 始终作为最终兜底。

### 6. 困难 NAT 覆盖

- `random/random` 和 `multi-public-IP` 继续回退 IPv6/Relay。
- 在获得真实失败样本前，不扩大随机扫描端口、socket 数量或报文预算。
- 根据失败日志和公网样本决定是否值得增加新策略，不引入服务端策略学习。

## 实网测试记录要求

测试时保留双方 NAT mapping 分类、STUN A/B 映射、delta、计划名称、attempt、屏障耗时、
报文数、成功端点、总耗时和最终回退模式。公网服务器地址和认证信息不要写入仓库。

新 NAT Punch 已完成的详细功能和未完成测试项见
[新 NAT 穿透 TODO](新NAT穿透TODO.md)，协议与排障说明见
[NAT 穿透文档](nat-traversal.md)。
