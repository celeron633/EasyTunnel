# EasyTunnel TUI 实现说明

## 目标

`EasyTunnel_tui` 是 GUI 客户端的终端版本，面向 Windows Terminal、Linux 终端和远程 SSH 会话。它复用项目现有的 `TunnelEngine`、`RendezvousClient`、TUN 适配器和日志模块，不维护第二套会合逻辑或隧道数据面。

TUI 源码位于：

```text
tui/
├── tui_main.cpp      # 平台初始化、日志路径、TUI 入口
├── tui_app.h         # 状态、线程和交互接口
├── tui_app.cpp       # 生命周期、主循环和 Tab 编排
├── tui_app_connection.cpp # Connection 页、连接流程和统计
├── tui_app_settings.cpp   # Settings 页和配置同步
├── tui_app_log.cpp        # Log 页和剪贴板操作
├── tui_config.h      # TUI JSON 配置模型
└── tui_config.cpp    # JSON 加载、校验和保存
```

## 技术选型

终端界面使用 FTXUI `v6.1.9`，通过 CMake `FetchContent` 固定版本。项目链接以下模块：

```text
ftxui::component
ftxui::dom
ftxui::screen
```

选择 FTXUI 的原因：

- 支持 Windows 和 Linux
- 支持键盘、鼠标、UTF-8 和终端颜色
- 不依赖 curses/ncurses
- Component/Renderer 模型适合复用 GUI 的状态和操作

## 页面结构

### Connection

- Rendezvous Server Addr/Port
- Room ID、My Peer ID、Auth Token
- 在线客户端刷新和选择
- Wait for peer、Connect selected、Disconnect
- TX/RX 包数、累计字节和每秒速度
- Bytes/KB/MB 单位切换按钮
- 红色 TX、绿色 RX 活动指示
- 当前连接状态

### Settings

- Adapter Name、Local TUN IPv4、Prefix、MTU
- Auto configure IPv4
- Keepalive、Peer Timeout、Punch Timeout
- NAT4 Source Port Start/Count、Peer Port Offset、Round Timeout
- 日志级别
- Auto wait for peer
- JSON 配置保存结果

### Log

- 显示最近 24 行实时日志
- 内存最多保留 2000 行，超过后批量清理旧记录
- Clear log 按钮
- 完整文件日志写入 `EasyTunnel_tui.log`

## 线程模型

```text
FTXUI 主线程
├── 处理输入、页面渲染和配置保存
├── 执行客户端列表查询
├── 启动/停止 TunnelEngine
└── 处理自动等待状态机

200ms Ticker 线程
└── PostEvent(Event::Custom)，驱动统计和页面刷新

TunnelEngine 工作线程
├── NAT 会合和打洞
├── TUN → UDP
├── UDP → TUN
└── 通过线程安全回调上报状态
```

引擎回调和日志回调不直接修改 FTXUI Component，只更新互斥量/原子状态并发送 `Event::Custom`，所有组件渲染和可变 UI 数据操作均在主线程完成。

## 配置持久化

TUI 在当前工作目录读写：

```text
EasyTunnel_tui.json
```

首次运行自动创建默认配置。之后每 200ms 比较一次配置签名，仅在内容变化时写入 JSON。配置字段与 GUI 对齐：

- 会合服务器、房间、Peer 和 Token
- TUN 适配器及 IPv4
- MTU、Prefix、自动配置
- NAT 保活和超时
- 日志等级
- Auto wait for peer

`auth_token` 为明文，请限制配置文件权限。

默认精确端口打洞和 NAT4 多 socket 回退的完整状态机见 [nat-traversal.md](nat-traversal.md)。

## 自动等待

启用 `Auto wait for peer` 后，TUI 会在以下状态自动以空 `target_peer_id` 启动引擎：

- 程序启动
- 手动断开
- 对端超时
- 打洞或网络错误

错误重试有 1 秒退避。退出 TUI 时会设置 `exiting` 和 `suppressAutoWait`，确保停止引擎后不会再次注册。

## 统计实现

- 包数和累计字节直接读取 `TunnelStats` 原子计数器
- 速度每 1 秒按字节差值除以实际时间计算
- 单位可在 Bytes、KB、MB 之间独立切换
- Ticker 检测包计数增加，TX/RX 指示在 350ms 内显示高亮

## 构建

```bash
cmake -S . -B build -DBUILD_TUI=ON
cmake --build build --target EasyTunnel_tui
```

Windows 多配置生成器：

```powershell
cmake --build build --config Release --target EasyTunnel_tui
```

Windows 构建会复制 `wintun.dll` 到 TUI 可执行文件目录，并写入请求管理员权限的 manifest。Linux 客户端需要 root 或 TUN/network capability。

只构建会合服务器时关闭所有客户端 UI：

```bash
cmake -S . -B build -DBUILD_GUI=OFF -DBUILD_TUI=OFF
cmake --build build --target EasyTunnel_rendezvous
```

## 操作提示

- `Tab` / `Shift+Tab`：在控件间移动
- 方向键：切换 Tab、日志等级和客户端列表
- `Enter` / `Space`：按钮、复选框和单位切换
- 鼠标：FTXUI 支持的终端中可直接点击
- Quit：安全停止隧道并退出
