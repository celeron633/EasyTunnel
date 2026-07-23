# EasyTunnel

EasyTunnel 是一个面向 IPv4 的点对点 TUN-over-UDP 隧道，支持通过公网会合服务器在 port-restricted NAT / CGN 环境中进行 UDP 打洞。

正常情况下，会合服务器只负责在线登记、客户端列表和公网端点交换，打洞成功后的数据不经过服务器。可选的 IPv4 Relay Fallback 会在全部直连路径失败后由会合服务器转发数据。

## 功能特性

- IPv4 TUN 数据直接封装到 UDP payload
- UDP NAT 打洞与 PUNCH/PUNCH_ACK 成功检测
- 会合服务器在线客户端列表和指定 Peer 连接
- 可选的会合服务器 IPv4 UDP relay 最终回退
- KEEPALIVE/ACK 维持 NAT/CGN 映射
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
    |================ PUNCH / UDP direct connection ====================>|
    |<=============== TUN IPv4 packets + KEEPALIVE =====================|
    |              direct paths fail: optional UDP relay                 |
    |<========================= Rendezvous =============================>|
```

会合服务器从控制数据报的真实源地址取得客户端公网 `IPv4:port`，因此不需要单独部署 STUN。默认模式复用会合 socket；NAT4 模式由 socket 池中实际收到 PUNCH 的 winner socket 接管隧道传输。

客户端代码中，`rendezvous_client.cpp` 负责会合服务器协议、响应解析、首次响应超时、在线列表和注销；`nat_traversal.cpp` 与 `nat4_traversal.cpp` 只负责 Peer 打洞和 NAT 状态机。由于同一个 UDP socket 同时接收服务器控制包与 Peer 打洞包，NAT 收包循环会按来源把服务器消息交给 `RendezvousClient`。

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

Windows 构建会自动下载 Wintun SDK、GLFW、ImGui 和 FTXUI。生成：

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

不使用 CMake 时，普通 Makefile 仍默认只构建原 console 服务端；如果系统已安装
FTXUI（`pkg-config ftxui` 可用），可显式构建 TUI：

```bash
make -f Makefile.rendezvous
make -f Makefile.rendezvous rendezvous_tui
```

会合服务器使用 POSIX UDP socket，不创建 TUN，默认端口大于 `1024` 时不需要 root。

会合服务器源码位于 `rendezvous/`：`main.cpp` 负责启动，`server.cpp` 负责 UDP 控制接收循环，`registry.cpp` 负责房间、配对和 NAT4 round 状态，`ipv4_relay_app.cpp` 独立负责 relay 端口、会话线程与数据透传，`config.cpp` 负责配置读写。

实现文档：

- [客户端与会合服务器状态机、UML 风格协商时序](doc/state-machine.md)
- [TUN 数据面与 Windows/Linux 适配器](doc/tun.md)
- [NAT 会合、精确端口打洞、NAT4 socket 池与故障排查](doc/nat-traversal.md)
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
其中的客户端、端点、空闲时间、配对/NAT4 状态和报文计数；Config 可以保存配置
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
2. B 端点击 **Refresh clients**。
3. B 端从列表选择 A，点击 **Connect selected**。
4. 双方收到公网端点后自动打洞。
5. PUNCH/PUNCH_ACK 成功后，状态切换为 Connected。

列表查询使用临时 UDP socket，仅用于展示在线客户端；实际公网映射来自等待/连接引擎持有的数据 socket。

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

## Console 客户端配置

复制 [conf/tunnel.conf.example](conf/tunnel.conf.example)，分别修改两端配置：

```ini
rendezvous_addr=203.0.113.10
rendezvous_port=3478
room_id=example-room
peer_id=node-a
target_peer_id=
auth_token=change-this-secret

keepalive_interval=15
peer_timeout=45
dummy_traffic_enabled=false
punch_timeout=30
nat4_source_port_start=30000
nat4_source_port_count=25
nat4_peer_port_offset=20
nat4_round_timeout=10
ipv6_fallback_enabled=false
ipv6_accept_inbound=false
ipv6_listen_port=0
ipv6_probe_host=2400:3200::1
ipv6_probe_port=53
ipv6_fallback_timeout=15
ipv4_relay_fallback_enabled=false

adapter_name=EasyTunnel-A
local_tun_ipv4=10.66.0.1
tun_prefix=24
tun_mtu=1452
auto_config_ipv4=true
log_level=Info
```

`target_peer_id` 留空表示注册并等待；填写目标 Peer ID 表示主动连接：

```powershell
EasyTunnel.exe tunnel.conf
```

Linux 客户端通常需要 root 或相应的 TUN/network capability。

## GUI 配置与日志

- GUI 配置修改后自动保存到当前工作目录的 `EasyTunnel_gui.json`。
- 启动时自动加载配置，并在页面显示保存结果和绝对路径。
- `auth_token` 会明文保存，请限制配置文件访问权限。
- **Log** 页面显示实时日志，最多保留 2000 行。
- 文件日志位于 `EasyTunnel_gui.exe` 同目录的 `EasyTunnel_gui.log`。
- Connection 页面按 1 秒间隔显示最近 60 秒的 TX/RX 速度和延迟柱形图。
- Settings → Rendezvous 中的 **Auto wait for peer** 默认关闭。启用后，GUI 会在启动、断开或错误退出连接后自动向会合服务器注册并等待其他 Peer。首次启动会立即注册；会合超时、其他错误或手动 Disconnect 后，会按同一区域的 **Retry Delay Seconds** 延迟重试（默认 5 秒，可配置 1–3600 秒）。

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
- 配置自动保存到当前工作目录的 `EasyTunnel_tui.json`
- Connection 页面提供与 GUI 一致的最近 60 秒速度和延迟历史图
- 日志写入可执行文件目录的 `EasyTunnel_tui.log`
- Quit 会安全停止隧道并退出

详细设计和线程模型见 [doc/tui.md](doc/tui.md)。

## 本机双实例测试

同一台 Windows 主机运行两个客户端时，必须配置不同的适配器名称和 TUN IPv4：

```text
A: Adapter Name = EasyTunnel-A, Local TUN IPv4 = 10.66.0.1
B: Adapter Name = EasyTunnel-B, Local TUN IPv4 = 10.66.0.2
```

如果两端使用同一个 Adapter Name，第二个进程可能在 `WintunStartSession` 阶段失败。即使适配器能成功创建，同一主机上的重复 TUN 路由也可能影响完整数据面测试；最终连通性建议使用两台主机或两个虚拟机验证。

## NAT 适用范围

典型 port-restricted cone NAT 同时具备 endpoint-independent mapping 时，双方持续发送 PUNCH 后通常可以建立直连。

普通精确端口打洞会先运行 2 秒；未成功时自动进入 n4 风格的 NAT4 socket 池模式。双方默认分别绑定连续的本地 UDP 端口 `30000～30024`，全部向会合服务器观测到的对端端口 `+20` 发送 PUNCH。一轮默认等待 10 秒；失败后源端口段整体增加 25，再开始下一轮。收到对端 PUNCH 的 socket 会接管后续隧道数据面。

NAT4 模式需要同步升级会合服务器。每端创建好本轮 socket 池后发送带 round ID 的 `NAT4_JOIN`；服务器只有在同一对 Peer 都进入相同 round 后，才向双方发送 `NAT4_PEER` 和本轮观测到的公网端点。客户端收到该消息后才开始发送 PUNCH，避免一端使用新端口段、另一端仍处于旧端口段。

- `nat4_source_port_start`：第一轮连续源端口的起点，默认 `30000`。
- `nat4_source_port_count`：每轮 socket 数量，默认 `25`、最大 `60`；设为 `0` 可关闭 NAT4 回退。
- `nat4_peer_port_offset`：对端公网端口预测增量，默认 `20`。
- `nat4_round_timeout`：每轮端口池等待时间，默认 `10` 秒。

整个普通打洞和 NAT4 多轮尝试仍受 `punch_timeout` 限制；等待模式会在收到 PEER 后才开始计算该超时。客户端和会合服务器都应升级到包含此功能的版本。

可选的 IPv6 Fallback 会在上述 IPv4 尝试全部失败后运行。双方必须都设置
`ipv6_fallback_enabled=true`、都具有经公网 TCP 探针验证的 IPv6 GUA，并且至少一端设置
`ipv6_accept_inbound=true`。会合服务器只交换 IPv6 UDP 端点和 `listen/connect` 角色，
直连确认及后续 TUN 数据不会经过服务器。完整设计和配置见
[IPv6 Fallback 文档](doc/ipv6-fallback.md)。

可选的 IPv4 Relay Fallback 是最后一级回退。双方设置
`ipv4_relay_fallback_enabled=true` 且服务器设置 `ipv4_relay_enabled=true` 后，服务器
为每对 Peer 分配一个双方共用的 UDP 端口和独立工作线程。数据仍是原始 TUN IPv4
UDP payload，不增加封装。服务器部署时必须放行配置的 UDP 端口范围。完整设计见
[IPv4 Relay Fallback 文档](doc/ipv4-relay-fallback.md)。

以下情况可能失败：

- 外部端口随机分配、递减，或递增量超过配置范围的 symmetric NAT
- 双方 NAT4 的公网端口无法用相同固定正偏移近似预测
- 企业或运营商网络禁止 P2P UDP
- 不支持 hairpin 的同公网出口场景
- NAT/CGN 主动改变或回收映射

未启用 IPv4 Relay Fallback 或服务器 relay 不可用时，全部直连路径失败后会进入 Error。

## 安全说明

- `auth_token` 只用于会合服务器准入，不是强身份认证。
- 隧道数据当前为明文，没有加密和完整的抗重放保护。
- 客户端只接受已确认公网端点或已认证 relay 端口发送的 IPv4 数据包。
- Relay 会话使用随机 session ID 和每端独立 access key 绑定公网端点，但隧道数据本身仍未加密。
- 生产环境建议增加 AEAD 加密和握手密钥派生。
