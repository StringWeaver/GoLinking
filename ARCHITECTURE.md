# Architecture: WinRT VPN Bridge with sing-box core (UWP App)

本文档记录了如何将跨平台的 Go 代理核心 (`sing-box`) 集成到 Windows WinRT VPN 架构中，实现基于纯 C++ / CMake 构建的免驱动（抛弃 `wintun`）UWP 桌面 VPN 代理。

## 1. 核心架构设计

整个应用运行在**前端 App 进程 (AppContainer 沙盒)**内，采用纯 C++ 宿主加载 libbox 主控 + VpnBridge.dll 桥接的模式：

### 层级 1: 前端应用 (UI & 引擎主控)
- **形式**: 纯 C++ / WinRT 构建的 UWP 应用程序 (CoreWindow)，通过 CMake 编译生成。
- **UI 极简设计**: 
  - 彻底放弃脆弱的 CMake XAML 编译。应用本身是一个极简的 UWP CoreWindow（无界面背景）。
  - 在启动并连接 VPN 后，利用 `Launcher::LaunchUriAsync` 自动唤起系统外置浏览器（Edge/Chrome），跳转至 `http://127.0.0.1:9090/ui` 展示 Web 控制台。
- **职责**: 
  - 通过 `LoadPackagedLibrary` 动态加载 `libbox.dll`。
  - 获取 `Setup`、`StartOrReloadService`、`CloseService` 等 C 导出函数指针，完全掌控引擎生命周期。
  - 负责处理 UWP 沙盒路径获取，并将 `LocalFolder` 作为工作目录传给 Go 引擎。
  - 调用 `VpnManagementAgent` 向 Windows 注册并触发 VPN 进程内后台任务 (In-Process Background Task)。

### 层级 2: Go 代理核心引擎 (libbox.dll)
- **形式**: 使用 Go `-buildmode=c-shared` 编译的 C 导出共享库。
- **职责**: 
  - 包含完整的 `sing-box` 引擎：路由、代理、DNS、协议解析、加解密。
  - 开启 `experimental.clash_api.external_ui`，为 WebView2 提供内嵌控制台支持。
  - 包含 `sing-tun` 的 `winRTVpn` 实现（通过 `with_winrt_vpn` 构建标签启用）。
  - `winRTVpn` 通过 `purego` 动态加载 `VpnBridge.dll`，注册 Go 回调函数指针。
  - 通过 `syscall.NewCallback` 将 Go 出站回调函数转为 C 函数指针，注册到 C++ 侧。
  - 通过 `purego` 调用 `VpnBridge.dll` 导出的 `VpnChannel_InjectPacket` 函数注入入站数据包。

### 层级 3: C++/WinRT 桥接层 (VpnBridge.dll)
- **形式**: 使用 MSVC 编译的标准 C++/WinRT 动态链接库，被系统注入并由 Go purego 加载。
- **职责**: 
  - 实现微软要求的 `IVpnPlugIn` COM 接口。
  - Go 侧通过 `syscall.NewCallback` 传入函数指针，C++ 侧直接调用，实现零延迟数据收发。
  - **核心 Hack (异步数据泵)**: 由于微软原生的 `Decapsulate` 事件驱动模型与现代代理引擎的 Mux/并发连接池冲突，我们通过建立一个 Fake Socket 骗过系统，然后直接暴露 `GetVpnReceivePacketBuffer` 和 `AppendVpnReceivePacketBuffer` 接口。
  - 导出 `VpnBridge_RegisterPlugin`、`VpnBridge_InitCOM`、`VpnChannel_InjectPacket` 供 Go purego 调用。

---

## 2. 单进程架构（纯 C++ UWP 主控）

```text
前端 App 进程 (UWP App.exe, AppContainer 沙盒):
  ├── UI 层 (极简 / 无头化)
  │     └── 启动时自动唤起外部浏览器 (Edge) -> http://127.0.0.1:9090/ui
  │
  ├── C++ 主控逻辑 (CoreWindow / IFrameworkView)
  │     └── LoadPackagedLibrary("libbox.dll") -> Setup / Start / Close
  │
  ├── libbox.dll (Go c-shared)
  │     ├── sing-box 完整引擎（路由、代理、DNS...）
  │     ├── clash_api (提供本地控制台和 REST API)
  │     └── sing-tun (winRTVpn, purego 零 CGO)
  │           ├── purego → LoadLibrary("VpnBridge.dll")
  │           ├── syscall.NewCallback(goOnEncapsulate) → 注册到 C++
  │           ├── Read()  ← Go 回调写入 packetChan
  │           └── Write() → purego → VpnChannel_InjectPacket
  │
  └── VpnBridge.dll (C++/WinRT 桥接层, 系统注入后台任务)
        ├── IVpnPlugIn 实现 (Connect/Disconnect/Encapsulate/Decapsulate)
        ├── IBackgroundTask::Run → VpnChannel::ProcessEventAsync
        ├── Encapsulate() → 直接调用 Go 传来的 onEncapsulate 函数指针
        └── 导出: VpnBridge_RegisterPlugin / VpnBridge_InitCOM / VpnChannel_InjectPacket

  单进程闭环，完全基于 C/C++ 生态，免提权，无需跨进程通信 ✅
```

### 核心收益

1. **彻底免驱与免提权**: 利用 `windows.networking.vpn` 规范，摒弃了 `wintun`，不再需要管理员权限，不会触发 UAC 弹窗。
2. **极简的前端开发**: UWP 彻底抛弃了 XAML 和 GUI 负担，变为一个隐形的“启动器”。所有的节点测速、配置修改均由自动弹出的 Web 界面完成。
3. **闭环在 C/C++ 生态中**: 相比于 Flutter/Dart FFI，C++ UWP 加载 `libbox.dll` 的 C 导出函数是原生的、最自然的方式。
4. **无需跨进程通信**: sing-box 引擎和 TUN 设备在同一个进程里，出站包直接通过内存指针投递，入站包通过 `purego` 注入系统，性能损耗极低。

---

## 3. Go ↔ C++ 接口：syscall.NewCallback + purego

### 架构原则

| 原则 | 说明 |
|---|---|
| **Go 侧主控** | libbox 通过 purego 加载 VpnBridge.dll，而非 VpnBridge 加载 libbox |
| **函数指针替代 //export** | 用 `syscall.NewCallback` 将 Go 函数转为 C 函数指针，注册到 C++ |
| **零 CGO 额外成本** | sing-tun 完全 purego，无需 `//export`，不引入任何 CGO 依赖 |
| **纯 C 接口边界** | Go 和 C++ 之间只传递纯 C 标量（指针 + 长度），无 WinRT 类型跨越 DLL 边界 |

### 出站回调链路（C++ → Go）

```go
// sing-tun/tun_windows_winrt.go
func goOnEncapsulate(data *byte, size uintptr) uintptr {
    packet := unsafe.Slice(data, int(size))
    packetCopy := make([]byte, len(packet))
    copy(packetCopy, packet)
    OnEncapsulate(packetCopy)  // → winRTVpn.packetChan
    return 0
}
// 注册时：
cb := syscall.NewCallback(goOnEncapsulate)
vpnBridgeRegisterPlugin(cb)
```

### 入站注入链路（Go → C++）

```go
// sing-tun/tun_windows_winrt.go
func injectPacketToWinRT(packet []byte) bool {
    ret, _, _ := purego.SyscallN(vpnChannelInjectPacket,
        uintptr(unsafe.Pointer(dataPtr)), uintptr(len(packet)))
    return ret != 0
}
```

### VpnBridge.dll 导出接口总览

| 导出函数 | 调用方 | 说明 |
|---|---|---|
| `VpnBridge_InitCOM()` | Go purego | 初始化 COM Apartment (multi_threaded) |
| `VpnBridge_RegisterPlugin(onEncapsulate)` | Go purego | 注册 Go 出站回调函数指针 |
| `VpnChannel_InjectPacket(data, size)` | Go purego | 注注入站数据包到 VPN 通道 |

---

## 4. UWP 沙盒 (AppContainer) 专属避坑指南

由于应用运行在严苛的 UWP 沙盒内，有两项重大的环境差异需要处理：

1. **文件系统隔离**:
   - `libbox.dll` **不能**在进程当前目录或系统目录写入日志或数据库（如 `cache.db`）。
   - **解决**: 在 UWP C++ 侧调用 `Windows::Storage::ApplicationData::Current().LocalFolder().Path()` 获取沙盒应用数据路径，然后通过 `libbox.dll` 的 `Setup` 函数（或启动参数）传入，让 Go 引擎以此为基础路径进行读写。
2. **本地回环网络限制 (Loopback Exemption)**:
   - UWP 沙盒默认禁止访问 `127.0.0.1`（即无法访问本地 WebView2 的 Web 控制台）。
   - **解决**: 
     - 必须在 `AppxManifest.xml` 中勾选 `privateNetworkClientServer` Capability。
     - 开发阶段可能需要运行 `CheckNetIsolation LoopbackExempt -a -n="<AppContainerName>"`。如果需要代理应用自身的流量，也需要利用 Windows API 豁免网络隔离。

---

## 5. 数据流与启动时序

### 完整连接时序

```text
[前端 UWP App 启动]
       │
       │ (1. C++ 通过 LoadPackagedLibrary 加载 libbox.dll)
       │ (2. 获取 LocalFolder 并通过 Setup 传给 libbox)
       │ (3. UWP WebView2 访问 127.0.0.1:9090/ui 展示配置面板)
       │ (4. purego 加载 VpnBridge.dll)
       │ (5. 调用 VpnBridge_InitCOM() 初始化 COM)
       │ (6. syscall.NewCallback(goOnEncapsulate) 生成 C 函数指针)
       │ (7. 调用 VpnBridge_RegisterPlugin(cb) 注册出站回调)
       │ (8. 调用 libbox.Start 启动引擎)
       ▼
[sing-box 引擎启动]
       │
       │ (9. 引擎创建 winRTVpn 实例作为 TUN 设备)
       │ (10. 用户点击 UWP 界面上的连接开关)
       │ (11. UWP 调用 VpnManagementAgent 触发连接)
       ▼
[VpnBridge: VpnPlugin::Connect(channel)]
       │
       │ (12. 系统将 VpnBridge.dll 拉起到 UWP 进程内)
       │ (13. channel.StartWithMainTransport 启动 VPN 通道)
       ▼
[引擎正常工作，通过指针回调收发数据]
```

---

## 6. 构建步骤指南

构建过程现涵盖三个部分，全链路依赖 CMake：

### 阶段一：构建 Go 代理核心 (libbox.dll)

因为 `-buildmode=c-shared` 要求入口为 `main` 包，我们需要创建一个封装用的 `go_dll` 目录（包含 `main.go` 并配置 `go.mod` 替换本地代码），在此目录下运行构建。必须包含 `with_winrt_vpn` 标签：
```powershell
cd go_dll
$env:CGO_ENABLED="1"
go build -buildmode=c-shared -v -trimpath -tags "with_gvisor,with_quic,with_wireguard,with_utls,with_purego,with_clash_api,with_winrt_vpn,badlinkname,tfogo_checklinkname0" -ldflags "-X github.com/sagernet/sing-box/constant.Version=1.13.0 -s -w -buildid= -checklinkname=0" -o ../build/libbox.dll .
```

### 阶段二：构建 C++/WinRT 桥接层 (VpnBridge.dll)

在 `CMakeLists.txt` 中单独配置：
```cmake
add_library(VpnBridge SHARED VpnBridge.cpp module.g.cpp VpnTask.cpp ...)
# 使用自定义的 cppwinrt.exe 和 midlrt.exe 命令解决 CMake VS_WINRT_COMPONENT 的坑
```

### 阶段三：构建 UWP 宿主应用 (App.exe)

在工程外层 `CMakeLists.txt` 中开启 UWP 构建：
```cmake
add_executable(UwpHost WIN32 App.cpp AppxManifest.xml ...)
set_property(TARGET UwpHost PROPERTY VS_WINRT_COMPONENT FALSE)
```
- 为了避开 CMake 编译 UWP XAML 的严重路径 Bug，UWP 宿主已被设计为**纯 CoreWindow (无 XAML)**。
- 编译完成后，将 `libbox.dll`、`VpnBridge.dll` 和 `UwpHost.exe` 拷贝至 `AppxLayout` 目录下，并使用 `Add-AppxPackage -Register` 进行开发者注册和免打包调试。

---

## 7. 调试与日志分析 (UWP AppContainer)

由于沙盒环境隔离，UWP 应用**无法**将日志输出到控制台或普通的相对路径下。如果你在开发时遇到无端闪退或连接失败，请按以下步骤进行排查：

### 1. 查找崩溃与系统拦截 (Event Viewer)
如果应用点击后毫无反应甚至闪退，通常是 COM 初始化、架构不匹配或 Go `panic()` 引发的 abort。
打开 PowerShell 运行：
```powershell
# 查看应用程序崩溃记录
Get-WinEvent -FilterHashtable @{LogName='Application'; ProviderName='Application Error'} -MaxEvents 3

# 查看 UWP 生命周期的拦截或权限报错
Get-WinEvent -LogName "Microsoft-Windows-TWinUI/Operational" -MaxEvents 3 | Format-List -Property Message
```

### 2. 查阅本地运行日志 (LocalState)
我们在 UWP 的 `App.cpp` 中获取了沙盒本地存储路径，并将 C++ 和 Go (libbox) 的运行日志重定向到了该目录中：
```powershell
# UWP 应用存储沙盒路径
$env:LOCALAPPDATA\Packages\SingTun.UwpHost_gbfdrcbt587gt\LocalState

# 查看 C++ 主控和 DLL 加载流程的日志
cat $env:LOCALAPPDATA\Packages\SingTun.UwpHost_gbfdrcbt587gt\LocalState\debug.log

# 查看 Go 引擎内部发生 panic 或 os.Stderr 的崩溃输出
cat $env:LOCALAPPDATA\Packages\SingTun.UwpHost_gbfdrcbt587gt\LocalState\stderr.log
```

---

## 8. 与 Wintun 模式的对比

| | Wintun 模式 (旧架构) | WinRT VPN 模式 (UWP C++ 架构) |
|---|---|---|
| **TUN 实现** | `NativeTun` (wintun.dll) | `winRTVpn` (windows.networking.vpn) |
| **需要管理员权限** | ✅ 需要（UAC 弹窗提权） | ❌ 不需要，纯沙盒模式 |
| **前端程序** | Tauri / Rust (或者 Flutter) | 纯 C++ UWP CoreWindow (无头) |
| **通信架构** | UI 进程 --(命名管道)--> 后台特权服务 | 纯单进程，UI 直接 LoadPackagedLibrary |
| **GUI 渲染** | 重客户端 (Tauri/Flutter 渲染) | 轻量壳 (仅唤起系统浏览器显示 WebUI) |
| **出站入口** | Go 主动轮询 Ring Buffer | C++ 回调 → Go 函数指针 |
| **入站注入** | 直接写 Ring Buffer | purego → VpnChannel_InjectPacket |
| **开发体验** | 涉及 IPC、特权服务、双端打包 | 极简 CMake 构建出闭环产物 |