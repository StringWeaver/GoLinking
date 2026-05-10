# Architecture: WinRT VPN Bridge with sing-box core

本文档记录了如何将跨平台的 Go 代理核心 (`sing-box`) 集成到 Windows WinRT VPN 架构中，实现免驱动（抛弃 `wintun`）的桌面 VPN 代理。

## 1. 核心架构设计

整个应用运行在**前端 App 进程**内，采用 libbox 主控 + VpnBridge.dll 桥接的模式：

### 层级 1: 前端应用 (UI & 引擎主控)
- **形式**: Flutter / C# / XAML 构建的应用程序（打包为 MSIX 或独立 EXE）。
- **职责**: 
  - 提供用户配置界面。
  - 通过 Dart FFI 直接加载 `libbox.dll`，完全掌控引擎生命周期。
  - 调用 `libbox` 的 `Setup`、`StartOrReloadService`、`CloseService` 管理代理服务。
  - 通过 gRPC `CommandClient` 获取日志、状态、分组、连接等实时信息。
  - 向 Windows 注册 VPN 进程内后台任务 (In-Process Background Task)。

### 层级 2: Go 代理核心引擎 (libbox.dll)
- **形式**: 使用 Go `-buildmode=c-shared` 编译的 C 导出共享库，由前端 App 直接加载。
- **职责**: 
  - 包含完整的 `sing-box` 引擎：路由、代理、DNS、协议解析、加解密。
  - 包含 `sing-tun` 的 `winRTVpn` 实现（通过 `with_winrt_vpn` 构建标签启用）。
  - `winRTVpn` 通过 `purego` 动态加载 `VpnBridge.dll`，注册 Go 回调函数指针。
  - 通过 `syscall.NewCallback` 将 Go 出站回调函数转为 C 函数指针，注册到 C++ 侧。
  - 通过 `purego` 调用 `VpnBridge.dll` 导出的 `VpnChannel_InjectPacket` 函数注入入站数据包。
  - 引擎内部直接处理出站数据包，无需跨进程通信。

### 层级 3: C++/WinRT 桥接层 (VpnBridge.dll)
- **形式**: 使用 MSVC 编译的标准 C++/WinRT 动态链接库，由 libbox 通过 purego 加载。
- **职责**: 
  - 实现微软要求的 `IVpnPlugIn` COM 接口。
  - **不再加载 libbox.dll**——Go 侧通过 `syscall.NewCallback` 传入函数指针，C++ 侧直接调用。
  - **核心 Hack (异步数据泵)**: 由于微软原生的 `Decapsulate` 事件驱动模型与现代代理引擎的 Mux/并发连接池冲突，我们通过建立一个 Fake Socket 骗过系统，然后直接暴露 `GetVpnReceivePacketBuffer` 和 `AppendVpnReceivePacketBuffer` 接口，把 WinRT 复杂的对象封装成简单的纯 C 函数指针。
  - 导出 `VpnBridge_RegisterPlugin` 供 Go 注册回调函数指针。
  - 导出 `VpnBridge_InitCOM` 供 Go 初始化 COM Apartment。
  - 导出 `VpnChannel_InjectPacket` 供 Go purego 调用注入入站数据包。

---

## 2. 单进程架构（前端 App 主控）

```
前端 App 进程:
  ├── Flutter UI (Dart FFI)
  │     └── 直接调用 libbox: Setup / StartOrReloadService / CloseService / gRPC
  │
  ├── libbox.dll (Go c-shared, 前端 App 加载)
  │     ├── sing-box 完整引擎（路由、代理、DNS...）
  │     ├── CommandServer (gRPC, 进程内)
  │     └── sing-tun (winRTVpn, purego 零 CGO)
  │           ├── purego → LoadLibrary("VpnBridge.dll")
  │           ├── syscall.NewCallback(goOnEncapsulate) → 注册到 C++
  │           ├── Read()  ← Go 回调写入 packetChan
  │           └── Write() → purego → VpnChannel_InjectPacket
  │
  └── VpnBridge.dll (C++/WinRT, MSVC 编译, 由 Go purego 加载)
        ├── IVpnPlugIn 实现 (Connect/Disconnect/Encapsulate/Decapsulate)
        ├── IBackgroundTask::Run → VpnChannel::ProcessEventAsync
        ├── Encapsulate() → 直接调用 Go 传来的 onEncapsulate 函数指针
        └── 导出: VpnBridge_RegisterPlugin / VpnBridge_InitCOM / VpnChannel_InjectPacket

  单进程闭环，前端完全控制，无需跨进程通信 ✅
```

### 核心收益

1. **前端完全掌控引擎生命周期**: Setup / StartOrReloadService / CloseService 全由前端调用，无需 IPC，配置管理简单直接。
2. **无需跨进程通信**: sing-box 引擎和 TUN 设备在同一个进程里，出站包直接交给引擎处理，入站包通过 purego 回调注入系统。
3. **与 Wintun (Karing) 模式一致**: 前端 App 加载 libbox 的方式完全相同，WinRT VPN 只是 TUN 实现的不同后端。
4. **不再需要 `//export`**: 用 `syscall.NewCallback` 替代 `//export BoxWinRT_OnEncapsulate`，sing-tun 保持 purego。

---

## 3. Go ↔ C++ 接口：syscall.NewCallback + purego

### 架构原则

| 原则 | 说明 |
|---|---|
| **Go 侧主控** | libbox 通过 purego 加载 VpnBridge.dll，而非 VpnBridge 加载 libbox |
| **函数指针替代 //export** | 用 `syscall.NewCallback` 将 Go 函数转为 C 函数指针，注册到 C++ |
| **零 CGO 额外成本** | sing-tun 完全 purego，无需 `//export`，不引入任何 CGO 依赖 |
| **纯 C 接口边界** | Go 和 C++ 之间只传递纯 C 标量（指针 + 长度），无 WinRT 类型跨越 DLL 边界 |

### CGO 使用情况

| 位置 | CGO 用途 | 是否必须 |
|---|---|---|
| `libbox.dll` 整体 | `-buildmode=c-shared` | ✅ Go 编译器强制要求 |
| `sing-tun/tun_windows_winrt.go` | 纯 Go，无 CGO | — |

**核心原则**：`libbox.dll` 使用 `-buildmode=c-shared` 本身就强制 `CGO_ENABLED=1`，但 WinRT VPN 功能不需要任何额外的 `//export` 函数。`sing-tun` 作为独立库，其 `winRTVpn` 实现完全基于 purego + `syscall.NewCallback`，不引入任何 CGO 依赖，可被任何 Go 项目（无论是否启用 CGO）引用。

### 出站回调链路（C++ → Go）

Go 侧通过 `syscall.NewCallback` 将函数转为 C 函数指针，注册到 VpnBridge.dll：

```go
// sing-tun/tun_windows_winrt.go

// Go 回调：C++ 的 Encapsulate 直接调用此函数
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

```cpp
// VpnBridge.cpp - Encapsulate 中直接调用 Go 传来的函数指针
typedef uintptr_t (*OnEncapsulateCallback)(const uint8_t* data, size_t size);
static OnEncapsulateCallback g_onEncapsulate = nullptr;

void VpnPlugin::Encapsulate(...) {
    if (!g_onEncapsulate) { /* drain and drop */ return; }
    while (packetCount-- > 0) {
        auto packet = packets.RemoveAtBegin();
        auto buffer = packet.Buffer();
        g_onEncapsulate(buffer.data(), static_cast<size_t>(buffer.Length()));
        packets.Append(packet);
    }
}
```

### 入站注入链路（Go → C++）

Go 侧通过 purego 动态调用 C++ 导出函数，无需 CGO：

```go
// sing-tun/tun_windows_winrt.go
func injectPacketToWinRT(packet []byte) bool {
    ret, _, _ := purego.SyscallN(vpnChannelInjectPacket,
        uintptr(unsafe.Pointer(dataPtr)), uintptr(len(packet)))
    return ret != 0
}
```

```cpp
// VpnBridge.cpp - 导出给 Go 调用
extern "C" __declspec(dllexport) bool VpnChannel_InjectPacket(const uint8_t* data, size_t size) {
    if (VpnPlugin::s_instance) {
        return VpnPlugin::s_instance->InjectReceivePacket(data, size);
    }
    return false;
}
```

### VpnBridge.dll 导出接口总览

| 导出函数 | 调用方 | 说明 |
|---|---|---|
| `VpnBridge_InitCOM()` | Go purego | 初始化 COM Apartment (multi_threaded) |
| `VpnBridge_RegisterPlugin(onEncapsulate)` | Go purego | 注册 Go 出站回调函数指针 |
| `VpnChannel_InjectPacket(data, size)` | Go purego | 注注入站数据包到 VPN 通道 |

---

## 4. 为什么选择这套架构？

1. **彻底免驱 (No Wintun)**: 利用 `windows.networking.vpn` 规范，应用本身即是 VPN 提供者，不需要请求管理员权限安装第三方内核驱动，也天然支持 UWP/MSIX 的按应用分流 (App Split Routing)。
2. **规避 WinRT 限制**: 现代 WinRT API 充满了面向对象的泛型（如 `IMap<String, IInspectable>`），纯 Go 的 `syscall` 和 `go-ole` 极难模拟。让 C++ (MSVC) 去处理这些脏活，让 Go 只处理干净的纯 C 标量和指针，是最优雅的解法。
3. **前端掌控引擎**: 与 Karing (Wintun) 模式一致，前端 App 直接加载 libbox.dll，无需 IPC 传递配置，引擎生命周期管理简单直接。
4. **syscall.NewCallback 替代 //export**: Go 标准库的 `syscall.NewCallback` 在 Windows 上自动处理 x64 调用约定，生成符合 Windows ABI 的跳板代码，让 C++ 可以像调用普通原生函数一样调用 Go。全程没有任何 `import "C"`，不需要 `//export`。
5. **兼容沙盒与包身份**: 将业务逻辑全部隔离在 DLL 内，完美契合 UWP 的 AppContainer 严格沙盒要求。对于打包为 MSIX 的 WinUI3 桌面应用，虽然可能不是完全的沙盒环境，但只要具有**包身份 (Package Identity)**，依然能够合法注册和拉起 VPN 后台任务。
6. **进程内后台任务**: VPN 后台任务注册为 In-Process 模式，VpnBridge.dll 中的 `IBackgroundTask::Run` 在前端 App 进程内被调用，Go runtime 已在进程内，函数指针有效。

---

## 5. 数据流时序图

### 出站方向（OS → VPN → sing-box → 远程服务器）

```text
[Windows 应用程序发送网络请求]
       │
       ▼
[Windows VPN 栈拦截出站包]
       │
       ▼
[VpnBridge: VpnPlugin::Encapsulate(channel, packets, ...)]
       │
       │ 遍历每个出站包，直接调用 Go 注册的函数指针
       ▼
[Go: goOnEncapsulate(data, size)]
       │
       │ tun.OnEncapsulate(packet)
       │   → winRTVpn.packetChan <- packet
       │   → Stack.tunLoop goroutine 被唤醒
       ▼
[sing-box 引擎处理：路由决策 → 协议封装 → 加密 → 发送到远程服务器]
       │
       ▼
[远程服务器]
```

### 入站方向（远程服务器 → sing-box → VPN → OS）

```text
[远程服务器返回响应]
       │
       ▼
[libbox.dll: sing-box 引擎解包得到入站 IP 报文]
       │
       │ winRTVpn.Write(packet)
       │   → injectPacketToWinRT(packet)
       │     → purego.SyscallN(VpnChannel_InjectPacket, data, size)
       ▼
[VpnBridge: VpnPlugin::InjectReceivePacket(data, size)]
       │
       │ channel.GetVpnReceivePacketBuffer()  → 获取系统缓冲区
       │ memcpy_s 拷贝数据                    → 填充报文内容
       │ channel.AppendVpnReceivePacketBuffer → 提交给系统
       │ channel.FlushVpnReceivePacketBuffers → 立即注入
       ▼
[Windows VPN 栈将入站包投递给目标应用程序]
```

### 完整连接时序

```text
[前端 App 启动]
       │
       │ (1. Flutter 通过 Dart FFI 加载 libbox.dll)
       │ (2. 调用 libbox.Setup() 初始化引擎环境)
       │ (3. purego 加载 VpnBridge.dll)
       │ (4. 调用 VpnBridge_InitCOM() 初始化 COM Apartment)
       │ (5. syscall.NewCallback(goOnEncapsulate) 生成 C 函数指针)
       │ (6. 调用 VpnBridge_RegisterPlugin(cb) 注册出站回调)
       │ (7. 调用 libbox.StartOrReloadService(config) 启动引擎)
       ▼
[sing-box 引擎启动]
       │
       │ (8. 引擎创建 winRTVpn 实例作为 TUN 设备)
       │ (9. purego 解析 VpnChannel_InjectPacket 准备入站注入)
       │ (10. 用户点击连接, 系统 VPN 框架激活进程内后台任务)
       ▼
[VpnBridge: VpnPlugin::Connect(channel)]
       │
       │ (11. AssociateTransport 绑定 Fake Socket)
       │ (12. 配置路由 (VpnRouteAssignment) 和 DNS)
       │ (13. channel.StartWithMainTransport 启动 VPN 通道)
       │     注意: 不再 LoadLibrary libbox，引擎已经在运行
       ▼
[引擎正常工作]
       │
       │ ┌─── 出站循环 ───────────────────────────────────────────┐
       │ │ Windows → VpnBridge::Encapsulate                     │
       │ │   → g_onEncapsulate (Go 函数指针)                    │
       │ │   → tun.OnEncapsulate → winRTVpn.packetChan          │
       │ │   → Stack.tunLoop → sing-box 代理引擎 → 远程服务器   │
       │ └───────────────────────────────────────────────────────┘
       │
       │ ┌─── 入站循环 ───────────────────────────────────────────┐
       │ │ 远程服务器 → sing-box 解包 → winRTVpn.Write           │
       │ │   → purego → VpnChannel_InjectPacket                  │
       │ │   → VpnBridge::InjectReceivePacket → Windows 系统    │
       │ └───────────────────────────────────────────────────────┘
       ▼
[前端 App 关闭 / 用户断开连接]
       │
       │ (14. 前端调用 libbox.CloseService() 关闭引擎)
       │ (15. winRTVpn.Close() 清理资源)
       │ (16. VpnBridge: VpnPlugin::Disconnect → channel.Stop())
       ▼
[清理完成]
```

---

## 6. 构建步骤指南

构建过程分为两个独立阶段，两个 DLL 编译互不依赖：

### 阶段一：构建 Go 代理核心 (libbox.dll)

1. 进入 `sing-box` 目录。
2. 确保开启了对应的编译 Tag，**必须包含 `with_winrt_vpn`** 以启用 WinRT VPN TUN 实现。
3. 使用 `c-shared` 编译（`-buildmode=c-shared` 强制要求 `CGO_ENABLED=1`）：
   ```powershell
   # 设置必要的编译标记
   $TAGS = "with_gvisor,with_quic,with_wireguard,with_utls,with_purego,with_clash_api,with_winrt_vpn,badlinkname,tfogo_checklinkname0"

   # 构建成 C 共享库
   go build -buildmode=c-shared -v -trimpath `
     -tags $TAGS `
     -ldflags "-X github.com/sagernet/sing-box/constant.Version=$VERSION -s -w -buildid= -checklinkname=0" `
     -o ../build/libbox.dll `
     ./experimental/libbox
   ```

### 阶段二：构建 C++/WinRT 桥接层 (VpnBridge.dll)

1. 使用 MSVC 编译 C++ 源码。
2. 代码中必须包含 `<winrt/Windows.Networking.Vpn.h>` 等 WinRT 头文件。
3. 确保所有需要暴露给 Go 调用的函数都被包裹在 `extern "C"` 和 `__declspec(dllexport)` 中，以避免 Name Mangling。
4. 编译命令示例（或使用 CMake）：
   ```powershell
   cl.exe /EHsc /std:c++20 /LD VpnBridge.cpp module.cpp VpnTask.cpp /link WindowsApp.lib /OUT:../build/VpnBridge.dll /DLL
   ```

### 阶段三：前端打包

1. 将生成的 `libbox.dll` 和 `VpnBridge.dll` 放置于应用的构建输出目录中。
2. 注册 VPN 进程内后台任务，声明入口点为 `VpnBridge.dll` 中的 `VpnTask` 类。

---

## 7. 与 Wintun 模式的对比

|| | Wintun 模式 | WinRT VPN 模式 |
|---|---|---|
| **TUN 实现** | `NativeTun` (LoadLibrary wintun.dll) | `winRTVpn` (系统 VPN 栈回调) |
| **需要管理员权限** | ✅ 需要安装 Wintun 驱动 | ❌ 不需要，系统原生支持 |
| **sing-tun 需要 CGO** | ❌ (purego 调用 wintun.dll) | ❌ (purego + syscall.NewCallback) |
| **libbox 需要 CGO** | ✅ (c-shared 构建模式) | ✅ (c-shared 构建模式) |
| **libbox 需要额外 //export** | ❌ | ❌ (用 syscall.NewCallback 替代) |
| **进程模型** | 单进程 (App.exe 加载 libbox.dll) | 单进程 (App.exe 加载 libbox.dll → purego 加载 VpnBridge.dll) |
| **按应用分流** | 需要 WFP 防火墙规则 | 系统原生支持 (VpnRouteAssignment) |
| **额外 DLL** | wintun.dll (第三方) | VpnBridge.dll (自研 C++/WinRT) |
| **谁加载 libbox** | Flutter App (Dart FFI) | Flutter App (Dart FFI) — 一致！ |
| **出站入口** | Go goroutine 主动轮询 Ring Buffer | C++ 回调 → Go 函数指针 (syscall.NewCallback) |
| **入站注入** | 直接写 Ring Buffer | purego → VpnChannel_InjectPacket |

---

## 8. 关键技术要点

### syscall.NewCallback 的使用限制

`syscall.NewCallback` 要求回调函数**必须有 `uintptr` 返回值**，即使原 C++ 函数返回 void：

```go
// ✅ 正确：返回 uintptr
func goOnEncapsulate(data *byte, size uintptr) uintptr {
    // ... 处理数据 ...
    return 0
}

// ❌ 错误：无返回值
func goOnEncapsulate(data *byte, size uintptr) {
    // ... 处理数据 ...
}
```

### COM Apartment 初始化

VpnBridge.dll 中的 WinRT 操作需要 COM Apartment。必须在 Go 进程中调用 `VpnBridge_InitCOM()` 一次性初始化，使用 **multi_threaded** 模式以适配 Go 的 goroutine 调度：

```cpp
// VpnBridge.cpp
__declspec(dllexport) void VpnBridge_InitCOM() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
}
```

⚠️ **永远不要在每次回调中反复 init/uninit COM Apartment**。`uninit_apartment()` 调用 `CoUninitialize()`，会销毁 C++/WinRT 的静态缓存（activation factories 等），导致后续调用崩溃。

### 进程内后台任务 (In-Process Background Task)

VPN 后台任务必须注册为**进程内模式**，确保 `VpnBridge.dll` 中的 `IBackgroundTask::Run` 在前端 App 进程内被调用。此时 Go runtime 已在进程内运行，`syscall.NewCallback` 生成的函数指针有效。

如果使用进程外后台任务 (Out-of-process)，系统会在 `backgroundTaskHost.exe` 中加载 VpnBridge.dll，此时没有 Go runtime，函数指针无效——这在新架构中不可行。
