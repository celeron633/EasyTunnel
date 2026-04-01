# 6Tunnel

Windows 下基于 Wintun 的 IPv4-over-IPv6 隧道。

- 本地从 Wintun 读到 IPv4 报文
- 直接封装为 UDP/IPv6 负载发送到对端
- 对端收到后写回本地 Wintun
- 不做加密，仅做隧道转发

## 依赖

- Windows 10/11
- MSYS2 (建议 `mingw64` 环境)
- CMake + Ninja
- 构建机可访问 `wintun.net`（默认自动下载 Wintun SDK）

## 配置文件

复制 `conf/tunnel.conf.example` 为 `tunnel.conf`，按两端实际地址修改：

- `local_ipv6`：本地绑定 IPv6（建议先用 `::` 监听全部本机 IPv6）
- `peer_ipv6`：对端公网 IPv6
- `udp_port`：两端一致
- `local_tun_ipv4`：本地 TUN 口 IPv4
- `tun_prefix`：掩码前缀

建议两端分别配置：

- A 端：`local_tun_ipv4=10.66.0.1`
- B 端：`local_tun_ipv4=10.66.0.2`

## 构建 (MSYS2)

在 MSYS2 MinGW64 终端：

```bash
pacman -S --needed mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja

cmake -S . -B build -G Ninja
cmake --build build
```

说明：

- 配置阶段会自动下载并解压 Wintun SDK 到 `build/_deps`。
- 若你想使用本地 SDK，可覆盖：`-DWINTUN_SDK_DIR=/c/dev/wintun`。

## 构建 (Visual Studio)

可使用 Visual Studio 2022（安装“使用 C++ 的桌面开发”组件）。

### 方式 1：Developer PowerShell / x64 Native Tools 命令行

```powershell
cmake -S . -B build-vs -G "Visual Studio 17 2022" -A x64
cmake --build build-vs --config Release
```

生成的可执行文件通常在：

`build-vs/Release/6tunnel.exe`

### 方式 2：Visual Studio GUI

1. 在 VS 中选择“打开本地文件夹”，打开项目目录。
2. VS 会识别 `CMakeLists.txt`，点击“CMake 设置”。
3. 选择 x64 + Release 配置，执行“生成全部”（首次会自动下载 SDK）。

可选：如果不希望自动下载，可在 CMake 变量中添加 `WINTUN_SDK_DIR`（例如 `C:/dev/wintun`）。

### 运行（VS 构建产物）

1. 将 `wintun.dll` 放到 `6tunnel.exe` 同目录，或放到系统 PATH。
2. 以管理员权限运行（自动配置网卡 IPv4 需要管理员权限）。

```powershell
./build-vs/Release/6tunnel.exe tunnel.conf
```

## 运行

1. 将 `wintun.dll` 放到可执行文件同目录（或系统 PATH 可见目录）。
2. 管理员权限运行（自动配置网卡 IP 需要管理员权限）。

```bash
./build/6tunnel.exe tunnel.conf
```

提示：默认构建会尝试自动把 `wintun.dll` 复制到输出目录；若复制失败，再手动放置 DLL。

### Wintun 驱动说明

- 仅有 `wintun.dll` 还不够，运行时仍然需要 Wintun 虚拟网卡驱动。
- 常见部署方式是随程序分发官方 `wintun.dll`：首次创建适配器时，DLL 会触发驱动安装流程。
- 首次安装/创建适配器通常需要管理员权限。
- 若创建适配器失败，优先检查：是否管理员运行、系统策略是否禁止驱动安装、`wintun.dll` 是否来自官方版本且与架构匹配（x64）。

## 连通性建议

- 放通 `udp_port` 的 IPv6 入站/出站防火墙规则
- 确认两端都可直连对方公网 IPv6
- 启动后可互 ping 对端 TUN IPv4 地址
- 若出现 `bind failed. err=10049`：说明 `local_ipv6` 不是本机已分配地址，改为 `local_ipv6=::` 或填本机真实 IPv6

## 注意

- 本项目演示的是“明文隧道”，没有做加密与认证。
- 生产环境建议增加认证、重放保护和可选加密。
