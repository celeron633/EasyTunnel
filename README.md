# EasyTunnel

EasyTunnel 是一个面向 IPv4 的点对点 TUN-over-UDP 隧道，支持通过公网会合服务器在 port-restricted NAT / CGN 环境中进行 UDP 打洞。

正常情况下，会合服务器只负责在线登记、客户端列表和公网端点交换，打洞成功后的数据不经过服务器。启用 IPv4 Relay 模式时，会合服务器会转发隧道数据；通常建议把它排在直连模式之后。

## 功能特性

- IPv4 TUN 数据直接封装到 UDP payload
- UDP NAT 打洞与 PUNCH/PUNCH_ACK 成功检测
- 会合服务器在线客户端列表和指定 Peer 连接
- 可排序的会合服务器 IPv4 UDP relay 模式
- KEEPALIVE/ACK 维持 NAT/CGN 映射
- Disconnect 发送会话认证的 `PEER_CLOSE`，让对端立即关闭；丢包时仍由 Peer timeout 兜底
- 对端超时检测和非预期 UDP 来源过滤
- Windows Wintun 与 Linux TUN 支持
- Windows/Linux Console 客户端
- ImGui GUI 客户端
- FTXUI TUI 客户端，支持 Windows/Linux 终端和 SSH 会话
- Windows/Linux IPv4 会合服务器
- GUI JSON 配置自动保存与内置日志页面
- GUI 实时流量、每秒速度和 TX/RX 活动指示灯
- 可选的 GUI 自动等待 Peer 与断线后重新注册
- 会合服务器 JSON 配置和分级日志

## 架构

```text
 Client A                         Rendezvous                         Client B
    |                                 |                                 |
    |----------- REG ---------------->|<--------------- REG ------------|
    |<----- online list / PEER --------|-------- online list / PEER ----->|
    |                                 |                                 |
    |=============== PUNCH / UDP direct connection ====================>|
    |<=============== TUN IPv4 packets + KEEPALIVE =====================|
    |              direct paths fail: optional UDP relay                 |
    |<========================= Rendezvous =============================>|
```

客户端使用两台独立公网 IPv4 STUN 服务从同一个 punch socket 探测映射；会合服务器只交换双方 STUN 结果、分配 session/attempt 并同步开始时间，不再预测 NAT 端口。收到合法 `PUNCH` 的 punch socket 直接接管隧道数据面。

客户端代码中，`stun_client.cpp` 负责 RFC 8489 Binding，`nat_punch_plan.cpp` 负责动态计划，`adaptive_nat_traversal.cpp` 负责完整打洞状态机，`rendezvous_client.cpp` 负责会合协议。STUN 探测和 Peer 打洞复用 punch socket，注册/选 Peer 使用独立控制 socket。

## 构建

要求：

- CMake 3.20+
- 支持 C++17 的编译器
- Windows 10/11，或支持 `/dev/net/tun` 的 Linux
- 构建时可访问依赖下载地址

### Windows

```powershell
cmake -S . -B build -DBUILD_GUI=ON -DBUILD_TUI=ON
cmake --build build --config Release
```

Windows 构建会自动下载 Wintun SDK、GLFW、ImGui、ImPlot、FTXUI 和 JsonCpp（JSON 配置读写）。生成：

- `EasyTunnel.exe`：Console 客户端
- `EasyTunnel_gui.exe`：GUI 客户端
- `EasyTunnel_tui.exe`：TUI 客户端
- `EasyTunnel_rendezvous.exe`：会合服务器
- `EasyTunnel_rendezvous_tui.exe`：带监控和配置界面的会合服务器

客户端需要管理员权限，服务端不需要 Wintun 或 TUN 权限。

### Linux 会合服务器

只部署服务端时建议关闭 GUI，避免安装 OpenGL/X11 依赖：

```bash
cmake -S . -B build -DBUILD_GUI=OFF -DBUILD_TUI=OFF
cmake --build build --target EasyTunnel_rendezvous
```

构建服务端 TUI 时启用 FTXUI：

```bash
cmake -S . -B build -DBUILD_GUI=OFF -DBUILD_TUI=ON
cmake --build build --target EasyTunnel_rendezvous_tui
```

不使用 CMake 时，普通 Makefile 默认同时构建会合服务器和 FTXUI TUI。JsonCpp（`pkg-config jsoncpp`
可用，例如 Debian/Ubuntu 的 `libjsoncpp-dev`）是两者共同的硬依赖，缺失时直接报错退出；FTXUI
大多数发行版没有现成的包，缺失时会打印源码构建安装的命令，且只影响 TUI 部分，普通服务端仍会正常构建：

```bash
make -f Makefile.rendezvous
```

只需要普通服务端、不想处理 FTXUI 依赖时：

```bash
make -f Makefile.rendezvous rendezvous
```

会合服务器使用 POSIX UDP socket，不创建 TUN，默认端口大于 `1024` 时不需要 root。

会合服务器源码位于 `rendezvous/`：`main.cpp` 负责启动，`server.cpp` 负责 UDP 控制接收循环，`registry.cpp` 负责房间、配对和 NAT session/attempt 屏障，`ipv4_relay_app.cpp` 独立负责 relay 端口、会话线程与数据透传，`config.cpp` 负责配置读写。

实现文档：

- [客户端与会合服务器状态机、UML 风格协商时序](doc/state-machine.md)
- [TUN 数据面与 Windows/Linux 适配器](doc/tun.md)
- [双 STUN、自适应 NAT Punch 与故障排查](doc/nat-traversal.md)
- [新 NAT 穿透目的、开发 Roadmap、完成情况与后续 TODO](doc/新NAT穿透TODO.md)
- [IPv6 Fallback：启用条件、角色协商、协议与故障排查](doc/ipv6-fallback.md)
- [IPv4 Relay Fallback：会话线程、协议、部署与故障排查](doc/ipv4-relay-fallback.md)

## 快速开始

### 1. 启动会合服务器

首次启动时，服务端会在当前工作目录自动创建 `EasyTunnel_rendezvous.json`：

```powershell
EasyTunnel_rendezvous.exe
```

Linux：

```bash
./build/EasyTunnel_rendezvous
```

也可以指定配置文件：

```text
EasyTunnel_rendezvous [config.json]
```

也可以用相同配置文件启动独立的 TUI 服务端：

```text
EasyTunnel_rendezvous_tui [config.json]
```

TUI 包含 Dashboard、Config、Logs 三个页签。Dashboard 实时列出全部活跃房间及
其中的客户端、端点、空闲时间、配对/NAT 状态和报文计数；Config 可以保存配置
或保存后重启监听服务；Logs 显示带级别颜色的实时服务日志。原
`EasyTunnel_rendezvous` console 程序及其构建目标保持不变。

请在云安全组和系统防火墙中放行配置的 IPv4 UDP 端口。

### 2. 配置两个客户端

两端需要：

- 相同的会合服务器地址、端口、Room ID 和 Auth Token
- 不同的 Peer ID
- 不同的 Adapter Name
- 不同的 Local TUN IPv4，例如 `10.66.0.1` 和 `10.66.0.2`

### 3. 建立连接

GUI 推荐流程：

1. A 端点击 **Wait for peer**，注册并保持在线。
2. B 端点击 **Refresh**。
3. B 端从列表选择 A，点击 **Connect selected**。
4. 双方收到公网端点后自动打洞。
5. PUNCH/PUNCH_ACK 成功后，状态切换为 Connected。

列表除 Peer ID 外还会显示每个客户端的公网端点、支持的穿透能力、已上报的 TUN IP 和空闲时间，
方便在连接前确认对端是否具备可协商的模式。

列表查询使用临时 UDP socket，仅用于展示在线客户端；列表中的公网端点是会合服务器看到的登记地址，
实际打洞使用的公网映射来自等待/连接引擎持有的数据 socket，两者可能不同。

## 会合服务器配置

默认 `EasyTunnel_rendezvous.json`：

```json
{
  "bind_address": "0.0.0.0",
  "port": 3478,
  "auth_token": "",
  "client_timeout_seconds": 60,
  "max_clients_per_room": 32,
  "ipv4_relay_enabled": false,
  "ipv4_relay_port_start": 40000,
  "ipv4_relay_port_end": 40100,
  "log_level": "Info",
  "log_file": "EasyTunnel_rendezvous.log"
}
```

| 配置项 | 说明 |
| --- | --- |
| `bind_address` | 监听 IPv4 地址 |
| `port` | 监听 UDP 端口 |
| `auth_token` | 客户端共享的准入 Token；日志不会输出该值 |
| `client_timeout_seconds` | 停止注册多久后从在线列表移除，范围 `5..3600` |
| `max_clients_per_room` | 单个房间最大客户端数，范围 `2..32` |
| `ipv4_relay_enabled` | 是否允许服务器承担 IPv4 UDP relay；默认关闭 |
| `ipv4_relay_port_start` | 每对 relay 会话可分配 UDP 端口范围的起点 |
| `ipv4_relay_port_end` | relay UDP 端口范围终点；端口数量也是最大并发会话数 |
| `log_level` | `Debug`、`Info`、`Warn` 或 `Error` |
| `log_file` | 日志文件路径；留空表示仅输出到控制台 |

服务端会记录配置加载、启动/停止、注册、注销、配对、客户端过期及拒绝原因。`Debug` 级别还会记录列表查询和目标未上线等诊断信息。

示例文件：[conf/rendezvous.json.example](conf/rendezvous.json.example)

## 客户端配置（三端共用）

Console、GUI 和 TUI 客户端共用当前工作目录下的同一个 JSON 配置：

```text
EasyTunnel.json
```

三端使用同一套读写逻辑（`client_config.cpp`）：文件不存在时自动创建默认配置，数值越界会被夹取，切换任意前端都会保留会合账号、TUN 参数和穿透策略。示例文件：[conf/EasyTunnel.json.example](conf/EasyTunnel.json.example)。

`auth_token` 会明文保存，请限制配置文件访问权限。

### Console 客户端

```powershell
EasyTunnel.exe [EasyTunnel.json] [target_peer_id]
```

- 第一个参数是配置路径，省略时使用当前目录的 `EasyTunnel.json`
- 第二个参数是目标 Peer ID：留空表示注册并等待，填写表示主动连接
- 首次运行会创建默认配置并提示编辑

Linux 客户端通常需要 root 或相应的 TUN/network capability。

## GUI 配置与日志

- 配置修改后自动保存到当前工作目录的 `EasyTunnel.json`。
- 启动时自动加载配置，并在页面显示保存结果和绝对路径。
- **Log** 页面显示实时日志，最多保留 2000 行。
- 文件日志位于 `EasyTunnel_gui.exe` 同目录的 `EasyTunnel_gui.log`。
- Connection 页面按 1 秒间隔显示最近 60 秒的 TX/RX 速度和延迟柱形图。
- Settings → Rendezvous 中的 **Auto wait for peer** 默认关闭。启用后，GUI 会在启动、断开或错误退出连接后自动向会合服务器注册并等待其他 Peer。首次启动会立即注册；会合超时、其他错误或手动 Disconnect 后，会按同一区域的 **Retry Delay Seconds** 延迟重试（默认 5 秒，可配置 1–3600 秒）。
- 进入 **Wait for peer** 并完成注册后会立即上报配置中的 TUN IPv4，因此 GUI/TUI 的在线客户端列表在连接建立前即可显示该地址。
- Settings → **Traversal strategy** 使用表格配置 Adaptive NAT Punch、IPv6 和 IPv4 Relay；每行可单独启用，并可通过 Up/Down 调整尝试顺序。

## TUI 客户端

TUI 复刻 GUI 的连接、设置、在线客户端、统计、日志和自动等待功能，适合终端及 SSH 环境：

```powershell
EasyTunnel_tui.exe
```

Linux：

```bash
sudo ./build/EasyTunnel_tui
```

- `Tab` / `Shift+Tab` 在控件间移动
- 方向键切换页面、日志等级和在线客户端
- `Enter` / `Space` 执行按钮、复选框和统计单位切换
- 支持 FTXUI 终端中的鼠标点击
- 配置自动保存到当前工作目录的 `EasyTunnel.json`（与 Console/GUI 共用）
- Windows 下提供托盘图标：单击切换终端窗口显示/隐藏，右键菜单可退出
- Connection 页面提供与 GUI 一致的最近 60 秒速度和延迟历史图
- 日志写入可执行文件目录的 `EasyTunnel_tui.log`
- Quit 会安全停止隧道并退出
- Settings 中的 **Traversal strategy** 表同样支持逐项开关和 Up/Down 排序

详细设计和线程模型见 [doc/tui.md](doc/tui.md)。

## 本机双实例测试

同一台 Windows 主机运行两个客户端时，必须配置不同的适配器名称和 TUN IPv4：

```text
A: Adapter Name = EasyTunnel-A, Local TUN IPv4 = 10.66.0.1
B: Adapter Name = EasyTunnel-B, Local TUN IPv4 = 10.66.0.2
```

如果两端使用同一个 Adapter Name，第二个进程可能在 `WintunStartSession` 阶段失败。即使适配器能成功创建，同一主机上的重复 TUN 路由也可能影响完整数据面测试；最终连通性建议使用两台主机或两个虚拟机验证。

## NAT 适用范围

Adaptive NAT Punch 使用同一 UDP socket 依次探测两台独立 STUN，并根据双方公网映射选择 Direct、动态 Range、Random 或 mixed random/range 计划。

客户端先向会合服务器上报 `traversal_modes` 中已启用的模式，再选定对端。三个模式名分别是 `nat_punch`（自适应 NAT 穿透）、`ipv6`（IPv6 直连）和 `ipv4_relay`（IPv4 流量中继）。每个模式必须恰好出现一次，`true/false` 控制开关。双方配置可以不同：服务器取双方能力交集，并严格保留连接发起方的顺序。旧配置中的 `nat`/`nat4` 会自动合并迁移为 `nat_punch`。

```ini
traversal_modes=nat_punch:true,ipv6:false,ipv4_relay:false
```

`stun_servers` 必须包含两台解析到不同公网 IPv4 的标准 RFC 8489 STUN 服务。推荐分别部署 Coturn `stun-only`。easy/easy 使用 Direct；regular/easy 使用互补的稳定端点发送和动态范围扫描；hard/hard regular 使用双方有界范围扫描；easy/random 使用有上限的随机 sender 和多 socket receiver；regular/random 使用随机目标 sender 和小范围多 socket receiver。当前 random/random 和 multi-public-IP 会转后续 IPv6/Relay，不会回退旧 fixed-offset 算法。`nat_punch_attempt_limit` 控制最多执行几次独立 attempt，范围为 1～10，默认 3；第 2/3 次 attempt 会分别尝试 TTL 7/TTL 4 receiver 预打洞和有界 sender delay。`nat_punch_profile` 可选 `balanced`（默认）或 `aggressive`，控制重试扩围速度、随机 socket/目标规模、发送间隔和报文预算。完整协议见 [Adaptive NAT Punch 文档](doc/nat-traversal.md)。

每次 Adaptive NAT Punch attempt 和 IPv4 Relay 分别使用一次 `punch_timeout`；IPv6 使用 `ipv6_fallback_timeout`。只有 STUN/信息交换/屏障/PUNCH 等瞬时失败会请求下一 attempt，配置或策略不支持会直接进入下一 traversal mode。等待模式会在收到 PEER 后才开始执行策略列表。

启用 IPv6 直连时，双方都必须在 `traversal_modes` 中将 `ipv6` 设为 `true`、都具有经公网 TCP 探针验证的 IPv6 GUA，并且至少一端设置
`ipv6_accept_inbound=true`。会合服务器只交换 IPv6 UDP 端点和 `listen/connect` 角色，
直连确认及后续 TUN 数据不会经过服务器。完整设计和配置见
[IPv6 Fallback 文档](doc/ipv6-fallback.md)。

启用 IPv4 流量中继时，双方在 `traversal_modes` 中将 `ipv4_relay` 设为 `true`，且服务器设置 `ipv4_relay_enabled=true` 后，服务器
为每对 Peer 分配一个双方共用的 UDP 端口和独立工作线程。数据仍是原始 TUN IPv4
UDP payload，不增加封装。服务器部署时必须放行配置的 UDP 端口范围。完整设计见
[IPv4 Relay Fallback 文档](doc/ipv4-relay-fallback.md)。

以下情况可能失败：

- 双方端口都随机变化或出现 multi-public-IP
- regular 映射变化超过当前 profile 的最大预测范围
- 企业或运营商网络禁止 P2P UDP
- 不支持 hairpin 的同公网出口场景
- NAT/CGN 主动改变或回收映射

策略列表中的所有已启用模式均失败后，连接进入 Error。

## 安全说明

- `auth_token` 只用于会合服务器准入，不是强身份认证。
- 隧道数据当前为明文，没有加密和完整的抗重放保护。
- 客户端只接受已确认公网端点或已认证 relay 端口发送的 IPv4 数据包。
- Relay 会话使用随机 session ID 和每端独立 access key 绑定公网端点，但隧道数据本身仍未加密。
- 生产环境建议增加 AEAD 加密和握手密钥派生。
