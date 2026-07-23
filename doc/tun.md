# EasyTunnel TUN 数据面与适配器

本文描述 EasyTunnel 当前的 TUN 数据面、Windows Wintun 和 Linux `/dev/net/tun` 实现，以及地址、路由、MTU、权限和故障排查。NAT 会合与 UDP 打洞状态机见 [nat-traversal.md](nat-traversal.md)。

相关代码：

```text
tun_adapter.h              跨平台 TUN 接口
tun_adapter_windows.cpp    Windows Wintun 实现
tun_adapter_linux.cpp      Linux /dev/net/tun 实现
wintun_loader.cpp          Wintun DLL 动态加载
tunnel_engine.cpp          TUN 与 UDP 之间的数据转发
util.cpp                   地址、MTU 和接口配置命令
```

## 作用边界

EasyTunnel 使用三层 TUN，而不是二层 TAP。TUN 每次读写的是一个完整 IP 数据包，不包含以太网头。

当前数据面只承载 IPv4：

- 从 TUN 读到 IPv4 包后，直接把完整 IP 包作为 UDP payload 发给已确认的 Peer；
- 从 Peer 收到 UDP payload 后，只有合法来源的 IPv4 包才写入 TUN；
- ARP、以太网帧和 IPv6 不进入隧道；
- Peer 控制包 `PUNCH`、`KEEPALIVE`、`PADDING` 等由引擎消费，不写入 TUN。

```text
本机 IPv4 协议栈
       │
       ▼
   TUN adapter
       │  完整 IPv4 packet
       ▼
 TunnelEngine ── UDP payload ──► confirmed peer endpoint
       ▲
       │  完整 IPv4 packet
       └──────────────────────── 对端 TunnelEngine / TUN
```

隧道 payload 没有额外的数据帧头。当前没有加密、压缩、重传或排序；启用 IPv4 Relay Fallback 时，会合服务器只原样转发同一 UDP payload。可靠性仍由被承载的上层协议自行处理。

## 启动与关闭顺序

引擎不会一启动就打开 TUN，实际顺序是：

```text
创建 UDP socket
  → 向会合服务器登记并选择 Peer
  → 完成 PUNCH/PUNCH_ACK、NAT4、IPv6 Fallback 或 IPv4 Relay Fallback
  → 打开并配置 TUN
  → 状态切换为 Connected
  → 启动 TUN→UDP 和 UDP→TUN 两个转发线程
```

因此，如果日志只到 `Rendezvous supplied peer` 就停止，且没有出现 `Data plane is up`，失败发生在 UDP/NAT 穿透阶段，TUN 还没有打开。

停止时，引擎先令收发循环退出并关闭 UDP socket，再释放 TUN session/device。Windows 适配器对象和自动配置的接口参数不会被删除；Linux 非持久 TUN 在文件描述符关闭后由内核释放。

## 数据转发规则

### TUN → UDP

TUN 读取循环最长阻塞约 500 ms，便于及时响应 Stop：

1. 读取一个完整包；
2. 空读取表示超时，继续检查运行状态；
3. 非 IPv4 包记录 Debug 日志并丢弃；
4. IPv4 包通过最终 UDP socket 发往直连 Peer 或已确认的 relay 端点；
5. 成功发送后更新 TX 包数和字节数。

致命 TUN 读取错误会停止引擎。单次 UDP `sendto` 失败只记录错误，不立即拆除隧道。

### UDP → TUN

UDP 接收循环先区分控制包和数据包：

1. 处理 Peer 的 `PUNCH`、`KEEPALIVE`、`KEEPALIVE_ACK` 和 `PADDING`；
2. 丢弃最终确认端点以外的 UDP 来源；
3. 丢弃非 IPv4 payload；
4. 把合法 IPv4 包写入 TUN；
5. 成功写入后更新 RX 包数和字节数。

缓冲区上限为 65535 字节。实际可用包长还受 TUN MTU、路径 MTU 和 UDP/IPv4 分片行为限制。

## 配置项

| 配置 | 默认值 | 说明 |
|---|---:|---|
| `adapter_name` | `EasyTunnel` | Windows Wintun 适配器名或 Linux TUN 接口名 |
| `local_tun_ipv4` | 无 | 必填；本端 TUN IPv4 地址 |
| `tun_prefix` | `24` | IPv4 前缀长度，范围 `0..32` |
| `tun_mtu` | `1452` | TUN MTU，范围 `576..9000` |
| `auto_config_ipv4` | `true` | 是否自动设置地址、MTU及平台相关接口状态 |

两端应使用不同的 `adapter_name` 和 `local_tun_ipv4`。最常见的点对点配置是使用同一隧道网段中的两个地址：

```ini
# Client A
adapter_name=EasyTunnel-A
local_tun_ipv4=10.66.0.1
tun_prefix=24
tun_mtu=1452

# Client B
adapter_name=EasyTunnel-B
local_tun_ipv4=10.66.0.2
tun_prefix=24
tun_mtu=1452
```

## 地址与路由

自动配置只负责接口地址、前缀、MTU和接口启用状态。操作系统通常会据此前缀建立直连路由，例如 `10.66.0.0/24` 指向 TUN。

当前实现不会自动配置：

- 任意远端网段的静态路由；
- 默认路由；
- DNS；
- IP forwarding；
- NAT/masquerade；
- 策略路由或路由 metric。

若需要通过 Peer 访问其后方 LAN，必须另外配置双方路由，并在作为网关的一端启用 IP forwarding；是否需要 NAT 取决于网络拓扑。TUN 网段也应避免与本机现有 LAN、VPN、容器网络重叠，否则系统可能选择错误路由。

`auto_config_ipv4=false` 时，EasyTunnel 只打开 TUN，不执行任何地址或 MTU 命令，以上内容全部由管理员预先配置。

## MTU

默认 `tun_mtu=1452`，为外层 IPv4 和 UDP 头以及实际网络环境留出空间。配置解析器在 MTU 大于 1472 时给出分片风险警告。

MTU 过大可能导致外层 UDP 分片，而部分 NAT、防火墙或运营商网络会丢弃分片。典型表现包括：小包或 ping 正常，大流量、文件传输或特定网站卡住。遇到这种情况可逐步降低两端 MTU，并保持双方一致。

## Windows Wintun

Windows 实现位于 `tun_adapter_windows.cpp`：

- 从可执行文件搜索路径动态加载 `wintun.dll`；
- 按 `adapter_name` 打开已有适配器，不存在时创建类型为 `EasyTunnel` 的适配器；
- 使用 4 MiB Wintun ring 启动 session；
- 通过 Wintun read event 最长等待 500 ms；
- 关闭连接时结束 session 并关闭 adapter handle，但不删除系统中的适配器。

构建系统会把 `wintun.dll` 复制到客户端输出目录。客户端 manifest 请求管理员权限，因为创建/配置网络适配器需要提升权限。

当 `auto_config_ipv4=true` 时依次执行：

```text
netsh interface ipv4 set address name="<adapter>" static <ip> <mask>
netsh interface ipv4 set subinterface "<adapter>" mtu=<mtu> store=persistent
Disable-NetAdapterBinding -Name "<adapter>" -ComponentID ms_tcpip6
```

这些设置会保留在系统中。尤其是 IPv6 binding 被显式禁用，EasyTunnel 退出时不会自动恢复。需要恢复时执行：

```powershell
Enable-NetAdapterBinding -Name "EasyTunnel-A" -ComponentID ms_tcpip6
```

同一台 Windows 主机运行两个客户端时必须使用不同适配器名。两个进程复用同一 Wintun 适配器可能导致 `WintunStartSession` 失败，重复的 TUN 地址和路由也会让数据面测试失真。

## Linux TUN

Linux 实现位于 `tun_adapter_linux.cpp`：

- 以读写模式打开 `/dev/net/tun`；
- 使用 `TUNSETIFF` 创建 `IFF_TUN | IFF_NO_PI` 接口；
- `IFF_NO_PI` 表示读写内容直接是 IP 包，没有额外的 4 字节 packet-information 头；
- `adapter_name` 受内核 `IFNAMSIZ - 1` 长度限制；
- 使用 `poll` 最长等待 500 ms 后读取数据包；
- 关闭文件描述符后释放当前非持久 TUN 接口。

当 `auto_config_ipv4=true` 时执行：

```bash
ip addr replace <local_tun_ipv4>/<tun_prefix> dev <adapter_name>
ip link set <adapter_name> up
ip link set <adapter_name> mtu <tun_mtu>
```

客户端通常需要 root，或同时具备打开 `/dev/net/tun`、执行 `TUNSETIFF` 和修改网络接口所需的权限。只授予可执行文件 capability 时，还要确认外部 `ip` 命令具备相应权限。

## 故障排查

### 没有出现 `Data plane is up`

先看 NAT 穿透日志。TUN 只在打洞确认后打开；`Rendezvous supplied peer` 后退出不属于 TUN 故障。Windows UDP `10054` 的历史问题和诊断方式见 [nat-traversal.md](nat-traversal.md#故障修复记录与排查)。

### `Failed to load Wintun`

确认 `wintun.dll` 与客户端可执行文件位于同一输出目录、架构一致，并检查日志中的 `LoadLibraryW` 或缺失 symbol 信息。

### `WintunCreateAdapter` 或配置命令失败

以管理员权限运行，确认适配器名有效，并检查是否被终端安全软件或网络管理策略阻止。自动配置命令及退出码会写入日志。

### `WintunStartSession failed`

检查是否已有另一个进程正在使用同名适配器。同机双实例应使用不同 `adapter_name`。

### Linux 无法打开 `/dev/net/tun`

确认内核启用了 TUN、设备节点存在，并检查当前用户权限。在容器中还需要把 `/dev/net/tun` 映射进容器并授予网络管理能力。

### 能连接但对端 TUN IP 不通

依次检查：

1. 双方 `local_tun_ipv4` 是否不同且处于预期前缀；
2. TUN 网段是否与现有 LAN/VPN/容器网段冲突；
3. 系统路由是否确实指向 EasyTunnel 适配器；
4. 主机防火墙是否允许该接口和隧道网段；
5. 两端 MTU 是否一致，降低 MTU 后大包是否恢复；
6. Debug 日志是否有 `Skip non-IPv4`、`Drop UDP packet from unexpected source` 或 TUN read/write 错误。

## 安全边界

TUN 中可能包含应用层敏感数据，而当前 UDP 隧道是明文的。`auth_token` 只控制会合服务器准入，不为数据面提供加密或完整性保护。需要在不可信网络中承载敏感流量时，应在 EasyTunnel 之上使用 TLS/SSH 等安全协议，或在数据面实现认证加密。
