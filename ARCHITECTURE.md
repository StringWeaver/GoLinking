# Architecture: WinRT VPN Plugin with sing-box core

本文档记录了如何将跨平台的 Go 代理核心 (`sing-box`) 集成到 Windows WinRT VPN 架构中，实现免驱动（抛弃 `wintun`）的桌面 VPN 代理。

## 1. 核心架构设计

整个应用的数据流和控制流被切分为三层：

### 层级 1: 前端应用 (UI & 系统注册)
- **形式**: Flutter / C# / XAML 构建的应用程序（打包为 MSIX 或独立 EXE）。
- **职责**: 
  - 提供用户配置界面。
  - 向 Windows 注册 VPN 插件 (VPN Background Task)。
  - 调用 `windows.networking.vpn` API 发起连接，将系统流量引导至 VpnPlugin。

### 层级 2: C++/WinRT 桥接层 (VpnPlugin.dll)
- **形式**: 使用 MSVC 编译的标准 C++/WinRT 动态链接库。
- **职责**: 
  - 实现微软要求的 `IVpnPlugIn` COM 接口。
  - 充当 Windows 系统 VPN 栈和 Go 核心引擎之间的桥梁。
  - **核心 Hack (异步数据泵)**: 由于微软原生的 `Decapsulate` 事件驱动模型与现代代理引擎的 Mux/并发连接池冲突，我们通过建立一个 Fake Socket 骗过系统，然后直接暴露 `GetVpnReceivePacketBuffer` 和 `AppendVpnReceivePacketBuffer` 接口，把 WinRT 复杂的对象封装成简单的纯 C 函数指针。
  - 在 `Connect()` 中通过 `LoadLibrary("libbox.dll")` 加载完整的 sing-box 引擎，调用其导出函数启动代理服务。
  - 导出 `VpnChannel_InjectPacket` 纯 C 函数供 Go 侧 purego 回调注入入站数据包。

### 层级 3: 代理核心引擎 (libbox.dll)
- **形式**: 使用 Go `-buildmode=c-shared` 编译的 C 导出共享库。
- **职责**: 
  - 包含完整的 `sing-box` 引擎：路由、代理、DNS、协议解析、加解密。
  - 包含 `sing-tun` 的 `winRTVpn` 实现（通过 `with_winrt_vpn` 构建标签启用）。
  - 导出 `BoxWinRT_OnEncapsulate` 函数供 VpnPlugin 的 `Encapsulate` 回调调用，将出站 IP 报文注入 sing-box 引擎。
  - 通过 `purego` 动态调用 VpnPlugin.dll 导出的 `VpnChannel_InjectPacket` 函数，将入站数据包注入系统 VPN 通道。
  - 引擎内部直接处理出站数据包，无需跨进程通信。

---

## 2. 单进程架构

```
系统 VPN 进程:
  VpnPlugin.dll
    └── LoadLibrary("libbox.dll")   ← 直接加载完整引擎
          ├── sing-box 完整引擎（路由、代理、DNS...）
          ├── sing-tun 的 winRTVpn 实现（purego，零 CGO）
          ├── //export BoxWinRT_OnEncapsulate  ← VpnPlugin::Encapsulate 的回调入口
          └── purego → VpnChannel_InjectPacket ← 入站包注入

  单进程闭环，无需跨进程通信 ✅
```

### 核心收益

1. **无需跨进程通信**: sing-box 引擎和 TUN 设备在同一个进程里，出站包直接交给引擎处理，入站包通过 purego 回调注入系统——与 Wintun 模式下 libbox 内部直接用 `NativeTun` 是同一个思路，只是 TUN 实现从 Wintun 换成了 WinRT VPN。
2. **复用 libbox 已有的服务管理**: `libbox.dll` 本身已导出 `Setup`、`StartOrReloadService`、`CloseService` 等函数，VpnPlugin 直接调用即可，无需重写。

---

## 3. CGO 边界：sing-tun 保持 purego

### CGO 使用情况

| 位置 | CGO 用途 | 是否必须 |
|---|---|---|
| `libbox.dll` 整体 | `-buildmode=c-shared` | ✅ Go 编译器强制要求 |
| `libbox/winrt_vpn.go` | `import "C"` + `//export BoxWinRT_OnEncapsulate` | ✅ 出站回调入口（libbox 反正要 CGO，零额外成本） |
| `sing-tun/tun_windows_winrt.go` | 纯 Go，无 CGO | — |
| `sing-tun/tun_windows_winrt_purego.go` | purego 调用 C++ 导出函数 | — |

**核心原则**：`libbox.dll` 使用 `-buildmode=c-shared` 本身就强制 `CGO_ENABLED=1`，在它里面新增 `//export` 函数是零额外成本的。而 `sing-tun` 作为独立库，其 `winRTVpn` 实现完全基于 purego，不引入任何 CGO 依赖，可被任何 Go 项目（无论是否启用 CGO）引用。

### 出站回调链路（C++ → Go）

VpnPlugin 的 `Encapsulate` 需要把出站包传递给 Go 侧。由于 `libbox.dll` 本身就是 `c-shared` DLL，它的 `//export` 函数天然可被 C++ 通过 `GetProcAddress` 直接调用：

```go
// sing-box/experimental/libbox/winrt_vpn.go
//go:build windows && with_winrt_vpn

package libbox

import "C"
import (
    "unsafe"
    "github.com/sagernet/sing-tun"
)

//export BoxWinRT_OnEncapsulate
func BoxWinRT_OnEncapsulate(data *C.uint8_t, size C.size_t) {
    packet := unsafe.Slice((*byte)(unsafe.Pointer(data)), int(size))
    packetCopy := make([]byte, len(packet))
    copy(packetCopy, packet)
    tun.OnEncapsulate(packetCopy)
}
```

```cpp
// VpnPlugin.cpp - Encapsulate 中调用
void VpnPlugin::Encapsulate(...) {
    auto encapFunc = (BoxWinRTEncapFunc)GetProcAddress(m_goModule, "BoxWinRT_OnEncapsulate");
    encapFunc(buffer.data(), buffer.Length());
}
```

### 入站注入链路（Go → C++）

Go 侧通过 purego 动态调用 C++ 导出函数，无需 CGO：

```go
// sing-tun/tun_windows_winrt_purego.go
func injectPacketToWinRT(packet []byte) bool {
    ret, _, _ := purego.SyscallN(vpnChannelInjectPacket, uintptr(unsafe.Pointer(dataPtr)), uintptr(len(packet)))
    return ret != 0
}
```

```cpp
// VpnPlugin.cpp - 导出给 Go 调用
extern "C" __declspec(dllexport) bool VpnChannel_InjectPacket(const uint8_t* data, size_t size) {
    return VpnPlugin::s_instance->InjectReceivePacket(data, size);
}
```

---

## 4. 为什么选择这套架构？

1. **彻底免驱 (No Wintun)**: 利用 `windows.networking.vpn` 规范，应用本身即是 VPN 提供者，不需要请求管理员权限安装第三方内核驱动，也天然支持 UWP/MSIX 的按应用分流 (App Split Routing)。
2. **规避 WinRT 限制**: 现代 WinRT API 充满了面向对象的泛型（如 `IMap<String, IInspectable>`），纯 Go 的 `syscall` 和 `go-ole` 极难模拟。让 C++ (MSVC) 去处理这些脏活，让 Go 只处理干净的纯 C 标量和指针，是最优雅的解法。
3. **最小化 CGO 影响范围**: `libbox.dll` 使用 `c-shared` 编译本身就需要 CGO（这是 Go 的 `-buildmode=c-shared` 的强制要求），出站回调的 `//export` 函数也放在 libbox 里，不增加额外 CGO 成本。`sing-tun` 保持 purego，可独立于 CGO 使用。VpnPlugin.dll 和 libbox.dll 在运行时通过标准的 `LoadLibrary` + `GetProcAddress` + `purego` 连接，两者编译互不依赖。
4. **兼容沙盒与包身份**: 将业务逻辑全部隔离在 DLL 内，完美契合 UWP 的 AppContainer 严格沙盒要求。对于打包为 MSIX 的 WinUI3 桌面应用，虽然可能不是完全的沙盒环境，但只要具有**包身份 (Package Identity)**，依然能够合法注册和拉起 VPN 后台任务。

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
[VpnPlugin::Encapsulate(channel, packets, encapsulatedPackets)]
       │
       │ 遍历每个出站包，通过 GetProcAddress 调用 libbox 导出函数
       ▼
[libbox.dll: BoxWinRT_OnEncapsulate(data, size)]
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
[VpnPlugin::InjectReceivePacket(data, size)]
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
[Windows 系统 VPN 框架]
       │
       │ (1. 用户点击连接, 系统拉起 VPN 后台任务)
       ▼
[VpnPlugin::Connect(channel)]
       │
       │ (2. AssociateTransport 绑定 Fake Socket)
       │ (3. 配置路由 (VpnRouteAssignment) 和 DNS)
       │ (4. channel.StartWithMainTransport 启动 VPN 通道)
       │ (5. LoadLibrary("libbox.dll"))
       │ (6. GetProcAddress("BoxWinRT_OnEncapsulate") 等导出函数
       │ (7. 调用 libbox 导出的 Setup + StartOrReloadService 启动引擎)
       ▼
[libbox.dll (sing-box 引擎)]
       │
       │ (8. 引擎创建 winRTVpn 实例作为 TUN 设备)
       │ (9. purego init() 注册 VpnChannel_InjectPacket 回调)
       │ (10. 引擎进入正常工作状态)
       │
       │ ┌─── 出站循环 ───────────────────────────────────────────┐
       │ │ Windows → VpnPlugin::Encapsulate                      │
       │ │   → BoxWinRT_OnEncapsulate (libbox //export)          │
       │ │   → tun.OnEncapsulate → winRTVpn.packetChan           │
       │ │   → Stack.tunLoop → sing-box 代理引擎 → 远程服务器    │
       │ └───────────────────────────────────────────────────────┘
       │
       │ ┌─── 入站循环 ───────────────────────────────────────────┐
       │ │ 远程服务器 → sing-box 解包 → winRTVpn.Write            │
       │ │   → purego → VpnChannel_InjectPacket                  │
       │ │   → VpnPlugin::InjectReceivePacket → Windows 系统     │
       │ └───────────────────────────────────────────────────────┘
       ▼
[VpnPlugin::Disconnect(channel)]
       │
       │ (11. channel.Stop())
       │ (12. 调用 libbox 导出的 CloseService 关闭引擎)
       ▼
[清理完成]
```

---

## 6. 构建步骤指南

构建过程分为两个独立阶段，两个 DLL 编译互不依赖：

### 阶段一：构建 Go 代理核心 (libbox.dll)

1. 进入 `sing-box` 目录。
2. 确保开启了对应的编译 Tag，**必须包含 `with_winrt_vpn`** 以启用 WinRT VPN TUN 实现（参考 `sing-box/experimental/libbox/ffi.json` 中关于 windows 的配置）。
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

### 阶段二：构建 C++/WinRT 桥接层 (VpnPlugin.dll)

1. 使用 MSVC 编译 C++ 源码。
2. 代码中必须包含 `<winrt/Windows.Networking.Vpn.h>` 等 WinRT 头文件。
3. 确保所有需要暴露给 Go 调用的函数（如 `VpnChannel_InjectPacket`）都被包裹在 `extern "C"` 和 `__declspec(dllexport)` 中，以避免 Name Mangling。
4. 编译命令示例（或使用 CMake）：
   ```powershell
   cl.exe /EHsc /std:c++20 /LD VpnPlugin.cpp module.cpp VpnTask.cpp /link WindowsApp.lib /OUT:../build/VpnPlugin.dll /DLL
   ```

### 阶段三：前端打包

1. 将生成的 `libbox.dll` 和 `VpnPlugin.dll` 放置于应用的构建输出目录中。
2. 注册 VPN 插件配置文件，声明后台任务入口点。

---

## 7. 与 Wintun 模式的对比

| | Wintun 模式 | WinRT VPN 模式 |
|---|---|---|
| **TUN 实现** | `NativeTun` (LoadLibrary wintun.dll) | `winRTVpn` (系统 VPN 栈回调) |
| **需要管理员权限** | ✅ 需要安装 Wintun 驱动 | ❌ 不需要，系统原生支持 |
| **sing-tun 需要 CGO** | ❌ (purego 调用 wintun.dll) | ❌ (purego 调用 VpnPlugin.dll) |
| **libbox 需要 CGO** | ✅ (c-shared 构建模式) | ✅ (c-shared 构建模式 + `//export BoxWinRT_*`) |
| **进程模型** | 单进程 (Karing.exe 加载 libbox.dll) | 单进程 (系统 VPN 进程加载 VpnPlugin.dll → libbox.dll) |
| **按应用分流** | 需要 WFP 防火墙规则 | 系统原生支持 (VpnRouteAssignment) |
| **额外 DLL** | wintun.dll (第三方) | VpnPlugin.dll (自研 C++/WinRT) |
| **谁加载 libbox** | Flutter App (Dart FFI) | VpnPlugin.dll (C++ LoadLibrary) |
| **出站入口** | Go goroutine 主动轮询 Ring Buffer | C++ 回调 → `//export BoxWinRT_OnEncapsulate` |
| **入站注入** | 直接写 Ring Buffer | purego → `VpnChannel_InjectPacket` |
