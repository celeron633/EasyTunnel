# EasyTunnel

Windows/Linux 下基于 Wintun/tun 的 IPv4-over-UDP 隧道，外层 UDP 可走 IPv6 或 IPv4。

- 本地从 TUN 读到 IPv4 报文
- 直接封装为 UDP 负载发送到对端
- 对端收到后写回本地 TUN
- 不做加密，仅做隧道转发
- 支持 Console 和 GUI (ImGui) 两种模式

## 依赖

- Windows 10/11 或 Linux
- CMake 3.20+ / Ninja (或 Visual Studio 2022)
- 构建机可访问外网（默认自动下载 Wintun SDK、GLFW、ImGui）
- Linux 额外依赖：`libgl-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev`

## 配置文件

复制 `conf/tunnel.conf.example` 为 `tunnel.conf`，按两端实际地址修改：

- `local_addr`：本地绑定 IPv4/IPv6；留空时按对端地址族监听任意地址
- `peer_addr`：对端公网 IPv4 或 IPv6
- `udp_port`：两端一致
- `local_tun_ipv4`：本地 TUN 口 IPv4
- `tun_prefix`：掩码前缀
- `tun_mtu`：TUN 口 IPv4 MTU（默认 `1452`，适用于 UDP/IPv6 外层开销 48 字节时的 1500 链路；走 IPv4 外层时可按路径 MTU 调整）
- `log_level`：日志级别，支持 `Debug` / `Info` / `Warn` / `Error`（默认 `Info`）

旧配置名 `local_ipv6` / `peer_ipv6` 仍兼容读取。

建议两端分别配置：

- A 端：`local_tun_ipv4=10.66.0.1`
- B 端：`local_tun_ipv4=10.66.0.2`

## 构建

### Linux

```bash
sudo apt install libgl-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev

cmake -S . -B build -G Ninja
cmake --build build
```

生成两个可执行文件：
- `build/EasyTunnel` — Console 模式
- `build/EasyTunnel_gui` — GUI 模式 (ImGui)

若只需 Console 模式，可禁用 GUI：`cmake -S . -B build -DBUILD_GUI=OFF`

### Windows (MSYS2)

在 MSYS2 MinGW64 终端：

```bash
pacman -S --needed mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja

cmake -S . -B build -G Ninja
cmake --build build
```

说明：

- 配置阶段会自动下载并解压 Wintun SDK 到 `build/_deps`。
- 若你想使用本地 SDK，可覆盖：`-DWINTUN_SDK_DIR=/c/dev/wintun`。

### Windows (Visual Studio)

可使用 Visual Studio 2022（安装"使用 C++ 的桌面开发"组件）。

```powershell
cmake -S . -B build-vs -G "Visual Studio 17 2022" -A x64
cmake --build build-vs --config Release
```

## GUI 模式

GUI 模式 (`EasyTunnel_gui`) 提供：

1. **连接页面**：选择本地 IPv6 地址（IPv6 外层使用）、输入对方 IPv4/IPv6（支持历史记录）、连接/断开按钮、收发包统计
2. **设置页面**：网络设置（UDP端口、MTU）、TUN 适配器设置（适配器名、IPv4地址、掩码）、日志级别
3. **状态栏**：实时显示连接状态和收发包数量

连接历史自动保存到 `EasyTunnel.ini`，下次启动可快速选择。

## 运行

### Console 模式

```bash
# Linux
sudo ./build/EasyTunnel tunnel.conf

# Windows (管理员权限)
./build/EasyTunnel.exe tunnel.conf
```

### GUI 模式

```bash
# Linux
sudo ./build/EasyTunnel_gui

# Windows (管理员权限)
./build/EasyTunnel_gui.exe
```

## 连通性建议

- 放通 `udp_port` 的 IPv4/IPv6 入站/出站防火墙规则
- 确认两端都可通过配置的公网 IPv4 或 IPv6 直连
- 启动后可互 ping 对端 TUN IPv4 地址
- 若出现 `bind failed`：说明 `local_addr` 不是本机已分配地址，可留空、改为 `0.0.0.0` / `::`，或填本机真实地址

## 注意

- 本项目演示的是"明文隧道"，没有做加密与认证。
- 生产环境建议增加认证、重放保护和可选加密。
