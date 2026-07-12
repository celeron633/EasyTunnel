# EasyTunnel NAT traversal (`nat-dev`)

面向 IPv4 的 TUN-over-UDP 点对点隧道。公网服务器只负责客户端在线登记、列表查询和端点交换；打洞成功后，隧道数据由两个客户端直接传输，不经过服务器。

## 工作流程

1. 客户端使用最终承载隧道数据的 UDP socket 向会合服务器发送 `REG`。
2. 服务器从 UDP 数据报源地址取得该 socket 的公网 `IPv4:port`，并维护房间在线列表。
3. 发起方通过 `CONNECT` 指定目标 `peer_id`，服务器向双方发送 `PEER` 端点。
4. 双方持续发送 `PUNCH`，收到预期端点的 `PUNCH/PUNCH_ACK` 后进入 Connected。
5. TUN IPv4 包直接作为 UDP payload 发送；KEEPALIVE/ACK 用于维持 NAT/CGN 映射。

不需要单独实现 STUN：注册报文的源地址就是该数据 socket 对会合服务器形成的公网映射。

## 构建

```powershell
cmake -S . -B build -DBUILD_GUI=ON
cmake --build build --config Release
```

生成：

- `EasyTunnel`：Console 客户端。
- `EasyTunnel_gui`：GUI 客户端。
- `EasyTunnel_rendezvous`：公网会合服务器，不创建 TUN。

## 会合服务器配置

服务端默认读取当前工作目录的 `EasyTunnel_rendezvous.json`。首次运行时若文件不存在，会自动创建默认 JSON 并继续启动。也可以把其他配置路径作为唯一参数：

```text
EasyTunnel_rendezvous [config.json]
```

默认配置：

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

- `bind_address`：监听 IPv4。
- `port`：监听 UDP 端口。
- `auth_token`：可选的房间共享准入 Token；日志不会打印该值。
- `client_timeout_seconds`：停止注册多久后从在线列表删除，范围 `5..3600`。
- `max_clients_per_room`：每个房间最大客户端数，范围 `2..32`。
- `log_level`：`Debug`、`Info`、`Warn` 或 `Error`。
- `log_file`：日志文件路径；留空表示只输出到控制台。

服务端会记录启动/停止、配置载入、注册、注销、配对、客户端过期和拒绝原因。`Debug` 级别还会记录列表查询和目标尚未上线等诊断信息。

仓库另提供 [conf/rendezvous.json.example](conf/rendezvous.json.example)。

### Windows 运行服务器

```powershell
EasyTunnel_rendezvous.exe
# 或指定配置
EasyTunnel_rendezvous.exe D:\config\rendezvous.json
```

需要在 Windows 防火墙和云安全组放行配置的 UDP 端口。

### Linux 运行服务器

Linux 会合服务器使用 POSIX UDP socket，不创建 TUN，也不依赖 Wintun。建议关闭 GUI，只构建服务端目标：

```bash
cmake -S . -B build -DBUILD_GUI=OFF
cmake --build build --target EasyTunnel_rendezvous
./build/EasyTunnel_rendezvous
```

也可以指定配置：

```bash
./build/EasyTunnel_rendezvous /etc/easytunnel/rendezvous.json
```

默认端口 `3478` 大于 `1024`，通常不需要 root。需要放行云安全组和 Linux 防火墙的入站/出站 UDP 端口。当前服务器只监听 IPv4，可用 `SIGINT` 或 `SIGTERM` 正常停止。

## GUI 使用

1. 两端填写相同的 Rendezvous Server、端口、Room ID 和 Auth Token。
2. 两端填写不同的 My Peer ID、Adapter Name 和 Local TUN IPv4。
3. A 端点击 **Wait for peer**，使用隧道数据 socket 注册并保持在线。
4. B 端点击 **Refresh clients**，选择 A，再点击 **Connect selected**。
5. 服务器向 A/B 下发彼此公网端点，双方开始打洞。

列表查询使用临时 UDP socket，仅用于展示；实际公网端点始终来自等待/连接引擎持有的数据 socket。

GUI 的 **Log** 页显示运行日志，并在 exe 目录写入 `EasyTunnel_gui.log`。GUI 配置修改后自动保存到当前工作目录的 `EasyTunnel_gui.json`，启动时自动加载，界面会显示保存结果及绝对路径。`auth_token` 会明文写入 JSON，请注意文件权限。

同一台 Windows 主机运行两个实例时，必须使用不同的 Adapter Name 和 Local TUN IPv4，避免争用同一个 Wintun Adapter。

## Console 客户端配置

复制 `conf/tunnel.conf.example`：

- `rendezvous_addr`、`rendezvous_port`、`room_id`、`auth_token`：两端相同。
- `peer_id`：两端不同，例如 `node-a` / `node-b`。
- `target_peer_id`：留空表示注册并等待；填写目标 Peer ID 表示主动连接。
- `local_tun_ipv4`：两端不同，例如 `10.66.0.1` / `10.66.0.2`。
- 不配置本地或对端 UDP 地址/端口。

```powershell
EasyTunnel tunnel.conf
```

## 边界与安全

- 适用于 endpoint-independent mapping / port-restricted cone NAT。Symmetric NAT 可能需要 UDP relay 回退。
- `auth_token` 只提供会合服务器准入，不加密隧道数据，也不是强身份认证。
- 非预期公网端点发来的 UDP 数据不会写入 TUN。
- keepalive 超时会将隧道切换到 Error；当前不会自动重连或切换中继。
