# EasyTunnel

EasyTunnel 是一个面向 IPv4 的点对点 TUN-over-UDP 隧道，支持通过公网会合服务器在 port-restricted NAT / CGN 环境中进行 UDP 打洞。

会合服务器只负责在线登记、客户端列表和公网端点交换。打洞成功后，IPv4 数据在两个客户端之间直接传输，不经过服务器。

## 功能特性

- IPv4 TUN 数据直接封装到 UDP payload
- UDP NAT 打洞与 PUNCH/PUNCH_ACK 成功检测
- 会合服务器在线客户端列表和指定 Peer 连接
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
```

会合服务器从 `REG` 数据报的真实源地址取得客户端公网 `IPv4:port`，因此不需要单独部署 STUN。最终打洞和隧道传输始终复用同一个客户端 UDP socket。

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

客户端需要管理员权限，服务端不需要 Wintun 或 TUN 权限。

### Linux 会合服务器

只部署服务端时建议关闭 GUI，避免安装 OpenGL/X11 依赖：

```bash
cmake -S . -B build -DBUILD_GUI=OFF -DBUILD_TUI=OFF
cmake --build build --target EasyTunnel_rendezvous
```

会合服务器使用 POSIX UDP socket，不创建 TUN，默认端口大于 `1024` 时不需要 root。

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
punch_timeout=30
nat4_max_port_offset=20

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
- Settings → Misc 中的 **Auto wait for peer** 默认关闭。启用后，GUI 会在启动、断开或错误退出连接后自动向会合服务器注册并等待其他 Peer；错误重试间隔为 1 秒。

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

对于一端 NAT3、另一端为“外部端口小幅递增”的 NAT4，客户端会在精确端口之外，自动尝试会合服务器观测端口之后的连续端口。`nat4_max_port_offset` 控制最大正向偏移，默认 `20`，GUI/TUI 中对应 **NAT4 Max Port Offset**；设为 `0` 可关闭。打洞确认后会自动切换到实际回包端口。两端都应升级到包含此功能的版本。

以下情况可能失败：

- 外部端口随机分配、递减，或递增量超过配置范围的 symmetric NAT
- 双方都是 symmetric NAT / endpoint-dependent mapping
- 企业或运营商网络禁止 P2P UDP
- 不支持 hairpin 的同公网出口场景
- NAT/CGN 主动改变或回收映射

当前实现没有 UDP relay 回退；打洞失败时会超时进入 Error。

## 安全说明

- `auth_token` 只用于会合服务器准入，不是强身份认证。
- 隧道数据当前为明文，没有加密和完整的抗重放保护。
- 客户端只接受已确认公网端点发送的 IPv4 数据包。
- 生产环境建议增加 AEAD 加密、握手密钥派生和 UDP relay 回退。
