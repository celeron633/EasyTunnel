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
├── tui_theme.h       # 共用的按钮样式、区块标题和标签行
├── windows_tray.h    # Windows 托盘图标（仅 Windows 编译）
└── windows_tray.cpp
```

JSON 配置模型与读写位于仓库根目录的 `client_config.h/cpp`，三个客户端共用。

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

## 整体框架

界面与 `EasyTunnel_rendezvous_tui` 保持一致的外壳，全部页面共用同一套边框和留白：

```text
 EasyTunnel Client  [ CONNECTED ]                              [Quit]
                     Connection  Settings  Log
─────────────────────────────────────────────────────────────────────
 当前页面内容（flex）
─────────────────────────────────────────────────────────────────────
 当前连接状态                                        按键提示（dim）
```

- 标题行右侧只保留 Quit，Tab 切换条独占一行并居中
- 状态徽标按 `TunnelState` 着色：Connected 绿、Connecting 黄、Waiting 蓝、Error 红、Disconnected 灰
- 连接状态移到全局底栏，任何页面都能看到，不再占用 Connection 页空间
- 底栏右侧的按键提示随当前页面变化
- 按钮统一渲染为 `[Label]`，聚焦时反色，替换 FTXUI 默认的动画色块

页面布局按默认大小的终端窗口（约 120x30）设计，无需滚动即可看完；窗口更小时各滚动区域会跟随焦点滚动。

## 页面结构

### Connection

- `Online peers` 面板：Refresh / Wait for peer / Connect selected / Disconnect 与在线数量同处工具栏
- 列表按列显示 Peer ID、公网端点、能力、TUN IP 和空闲时间，选中项下方显示该客户端能力的完整名称
- 无在线客户端时面板居中显示提示，不再留下空白列表
- `Traffic` 面板：TX/RX 两行对齐显示包数、累计字节和每秒速度，行首圆点表示最近一个采样周期内是否有流量，末行显示延迟
- 累计字节和速度单元格可回车切换 Bytes/KB/MB，聚焦时反色
- `Last 60 seconds` 面板：TX 速度、RX 速度和 RTT 三张柱形图并排，随窗口高度拉伸

### Settings

两列布局，左列是会合与时序参数，右列是数据面参数，两列各自独立滚动：

左列

- Rendezvous：Server address/port、Room ID、My peer ID、Auth token、Retry delay、Auto wait for peer
- NAT liveness：Keepalive、Peer timeout
- NAT Punch：Punch timeout、Attempt limit、STUN A/B 地址和端口
- Log and misc：日志级别（横向 Toggle，占一行）、1 KiB/s dummy traffic

右列

- TUN adapter：Adapter name、Local TUN IPv4、Prefix、MTU、Auto configure IPv4
- Traversal strategy 表格：Adaptive NAT Punch、IPv6、IPv4 Relay 逐项开关、优先级和 `[^]` / `[v]` 排序
- IPv6 direct connection：主动入站声明、监听端口、TCP 探针主机/端口和超时

底部单独一行显示 JSON 配置保存结果。

### Log

- 视口保留最近 500 行并固定贴在底部，新日志不会让页面跳动
- 按日志级别着色：Debug 灰、Info 白、Warn 黄、Error 红
- 标题行显示当前行数，右侧为 Copy selection / Copy all / Clear
- 内存最多保留 2000 行，超过后批量清理旧记录
- 底部显示完整日志文件路径，复制结果就地反馈

## 线程模型

```text
FTXUI 主线程
├── 处理输入、页面渲染和配置保存
├── 执行客户端列表查询
├── 启动/停止 TunnelEngine
└── 处理自动等待状态机

1s Ticker 线程
└── PostEvent(Event::Custom)，驱动统计和页面刷新

TunnelEngine 工作线程
├── NAT 会合和打洞
├── TUN → UDP
├── UDP → TUN
└── 通过线程安全回调上报状态
```

引擎回调和日志回调不直接修改 FTXUI Component，只更新互斥量/原子状态并发送 `Event::Custom`，所有组件渲染和可变 UI 数据操作均在主线程完成。

## Windows 托盘图标

Windows 构建在进入 FTXUI 主循环前创建托盘图标，复用 exe 内嵌的应用图标资源。控制台窗口属于终端宿主进程（conhost 或 Windows Terminal），无法像 GUI 那样子类化窗口过程拦截最小化/关闭，因此托盘由独立线程驱动一个隐藏窗口接收消息：

- 左键单击 / 双击：切换终端窗口显示与隐藏
- 右键菜单：Show/Hide window 和 Exit（通过 `ExitLoopClosure` 安全退出）
- Explorer 重启（`TaskbarCreated` 广播）后自动重新注册图标

终端窗口的定位：conhost 下直接使用 `GetConsoleWindow()`；Windows Terminal 下该句柄是隐藏的 ConPTY 伪窗口，退回启动时的前台窗口。注意 Windows Terminal 的隐藏会作用于整个终端窗口（包括其他标签页）。托盘创建失败只记录日志，不影响 TUI 运行。

## 刷新频率与闪烁

页面上所有统计量的精度都是 1 秒，因此刷新策略按这个精度收敛，避免整屏重绘造成撕裂：

- Ticker 由 200ms 放宽到 1 秒，与 `EasyTunnel_rendezvous_tui` 一致
- 日志回调的 `PostEvent` 限流到最多每 200ms 一次，突发日志不再触发几十次整屏重绘；被限流掉的内容由下一次 Ticker 补上
- TX/RX 活动指示的判定窗口相应放宽到 1500ms，保证一个采样周期内的流量仍会点亮

启动时不再调用 `SetConsoleScreenBufferSize` / `SetConsoleWindowInfo` 强行把控制台放大到 110x40。Windows Terminal 会异步响应该请求并在 FTXUI 已经开始向备用屏幕缓冲区渲染时重排缓冲区，这是默认窗口大小下闪烁的直接原因。现在布局本身适配默认窗口，控制台尺寸完全交给用户。

## 配置持久化

TUI 在当前工作目录读写与 Console/GUI 客户端共用的配置：

```text
EasyTunnel.json
```

读写、校验和引擎配置转换统一在根目录 `client_config.cpp`（`ClientConfig` 结构体）实现。首次运行自动创建默认配置。之后每次 Ticker 比较一次配置签名，仅在内容变化时写入 JSON。配置字段：

- 会合服务器、房间、Peer 和 Token
- TUN 适配器及 IPv4
- MTU、Prefix、自动配置
- NAT 保活和超时
- 日志等级
- 会合重试延迟
- Auto wait for peer
- 有序的四种穿透模式及其开关

`auth_token` 为明文，请限制配置文件权限。

双 STUN 与 Adaptive NAT Punch 的完整状态机见 [nat-traversal.md](nat-traversal.md)，
最终服务器代理回退见 [ipv4-relay-fallback.md](ipv4-relay-fallback.md)。

## 自动等待

启用 `Auto wait for peer` 后，TUI 会在以下状态自动以空 `target_peer_id` 启动引擎：

- 程序启动
- 手动断开
- 对端超时
- 打洞或网络错误

程序启动时会立即执行首次自动等待；会合超时、其他错误或手动断开后，按 `rendezvous_retry_delay_seconds` 延迟重试。该值位于 Settings → Rendezvous，默认 5 秒，可配置 1–3600 秒。退出 TUI 时会设置 `exiting`，确保停止引擎后不会再次注册。

## 统计实现

- 包数和累计字节直接读取 `TunnelStats` 原子计数器
- 速度每 1 秒按字节差值除以实际时间计算
- 速度与延迟历史每 1 秒采样，最多保留最近 60 秒
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
- 方向键：切换 Tab、日志等级和客户端列表；Settings 页左右方向键在两列之间切换
- `Enter` / `Space`：按钮、复选框和单位切换
- `Ctrl+C`：在 Log 页复制（有选区复制选区，无选区复制全部），其他页面退出程序
- 鼠标：FTXUI 支持的终端中可直接点击
- Quit：安全停止隧道并退出
