# Architecture: UWP/WinUI3 VPN App with sing-box core (Zero CGO)

本文档记录了如何将跨平台的 Go 代理核心 (`sing-box`) 优雅地集成到 Windows 现代 VPN 架构中，实现免驱动（抛弃 `wintun`）的桌面 VPN 代理。

## 1. 核心架构设计

在传统的跨语言集成方案中，通常会使用 CGO 强行将 C++ 和 Go 代码混合编译。但在我们的架构中，我们采用 **双 DLL 解耦 (Zero CGO) + 纯指针桥接** 的“降维打击”方案。

整个应用的数据流和控制流被切分为三层：

### 层级 1: 现代前端 (UI & 系统注册)
- **形式**: C# / XAML 构建的 UWP 或 WinUI3 (打包为 MSIX) 应用程序。
- **职责**: 
  - 提供用户配置界面。
  - 向 Windows 注册一个后台 VPN 任务 (VPN Background Task)。
  - 调用 `windows.networking.vpn` API 发起连接，将系统流量引导至我们的插件中。

### 层级 2: C++/WinRT 中转站 (VpnPlugin.dll)
- **形式**: 使用 MSVC 编译的标准 C++ 动态链接库。
- **职责**: 
  - 实现微软要求的 `IVpnPlugIn` COM 接口。
  - 充当系统虚拟网卡和 Go 核心之间的桥梁。
  - **核心 Hack (异步数据泵)**: 由于微软原生的 `Decapsulate` 事件驱动模型与现代代理引擎的自带 Mux/并发连接池冲突，我们通过建立一个 Fake Socket（例如连到 127.0.0.1）骗过系统，然后直接暴露 `GetVpnReceivePacketBuffer` 和 `AppendVpnReceivePacketBuffer` 接口，把 WinRT 复杂的对象封装成简单的纯 C 函数指针（如 `WriteToVpnChannel(uint8_t* data, size_t size)`）。

### 层级 3: 代理核心引擎 (libbox.dll)
- **形式**: 使用 Go `-buildmode=c-shared` 编译的标准 C 导出库。
- **职责**: 
  - 基于 `sing-box` (或带有 `sing-tun` 功能) 处理所有的核心网络路由、协议解析、加解密。
  - 通过 `syscall.NewCallback` 或 `purego` 注册从 C++ 端传来的回调函数指针。
  - 当收到出站请求时，把拦截下来的 IP 报文交给内部协议栈处理。
  - 当协议栈解出入站报文时，直接调用 C++ 给的回调指针，将干净的 `[]byte` 灌入虚拟网卡。

---

## 2. 为什么选择这套架构？

1. **彻底免驱 (No Wintun)**: 利用 `windows.networking.vpn` 规范，应用本身即是 VPN 提供者，不需要请求管理员权限安装第三方内核驱动，也天然支持 UWP/MSIX 的按应用分流 (App Split Routing)。
2. **规避 WinRT 限制**: 现代 WinRT API 充满了面向对象的泛型（如 `IMap<String, IInspectable>`），纯 Go 的 `syscall` 和 `go-ole` 极难模拟。让 C++ (MSVC) 去处理这些脏活，让 Go 只处理干净的纯 C 标量和指针，是最优雅的解法。
3. **零 CGO 污染**: Go 核心使用 `c-shared` 独立编译，C++ 插件独立编译，两者在运行时通过标准的 `LoadLibrary` 和 `GetProcAddress` 连接，完全不需要配置复杂的 C/C++ 交叉编译工具链。
4. **兼容沙盒与包身份**: 将业务逻辑全部隔离在 DLL 内，完美契合 UWP 的 AppContainer 严格沙盒要求。对于打包为 MSIX 的 WinUI3 桌面应用，虽然可能不是完全的沙盒环境，但只要具有**包身份 (Package Identity)**，依然能够合法注册和拉起 VPN 后台任务。

---

## 3. 跨语言交互时序图

```text
[Windows 系统 VPN 框架]
       │
       │ (1. 点击连接, 系统拉起后台任务)
       ▼
[C++ DLL: VpnPlugin::Connect]
       │
       │ (2. AssociateTransport 绑定假 Socket)
       │ (3. LoadLibrary("libbox.dll"))
       │ (4. Register Callback (Go -> C++))
       ▼
[Go DLL: libbox.dll (sing-box)]
       │
       │ (5. 代理引擎启动，拦截出站流量)
       │ (6. 处理网络收发)
       │
       │ (7. 引擎产生了解包后的入站 IP 报文)
       ▼
[C++ DLL: Go 调用的注入回调]
       │
       │ (8. channel.GetVpnReceivePacketBuffer())
       │ (9. memcpy_s 拷贝 C 内存)
       │ (10. channel.AppendVpnReceivePacketBuffer())
       ▼
[Windows 系统 VPN 框架: 数据包注入虚拟网卡]
```

---

## 4. 构建步骤指南

为了保持项目的清晰与解耦，构建过程严格分为两个独立阶段：

### 阶段一：构建 Go 代理核心 (libbox.dll)

1. 进入 `sing-box` 目录或你的外壳包装目录。
2. 确保开启了对应的编译 Tag（参考 `sing-box/experimental/libbox/ffi.json` 中关于 windows 的配置，例如 `with_quic`, `badlinkname`, `with_purego` 等）。
3. 禁用 CGO，使用普通构建模式（如果使用 purego 拦截底层请求）或者使用 `c-shared` 导出 C 接口：
   ```powershell
   # 设置必要的编译标记
   $TAGS = "with_gvisor,with_quic,with_purego,badlinkname,tfogo_checklinkname0"
   
   # 构建成 C 共享库
   go build -buildmode=c-shared -tags $TAGS -o ../build/libbox.dll ./experimental/libbox
   ```

### 阶段二：构建 C++/WinRT 中转站 (VpnPlugin.dll)

1. 使用 MSVC 编译 C++ 源码。
2. 代码中必须包含 `<winrt/Windows.Networking.Vpn.h>` 等现代 UWP 头文件。
3. 确保所有需要暴露给 Go 调用的函数（如注册回调、初始化环境）都被包裹在 `extern "C"` 和 `__declspec(dllexport)` 中，以避免 Name Mangling。
4. 编译命令示例 (或使用 MSBuild/CMake)：
   ```powershell
   cl.exe /EHsc /std:c++20 /LD VpnPlugin.cpp /link WindowsApp.lib /OUT:../build/VpnPlugin.dll /DLL
   ```

### 阶段三：前端打包 (UWP / WinUI3 MSIX)

1. 将生成的 `libbox.dll` 和 `VpnPlugin.dll` 放置于 UWP 或 WinUI3 应用的构建输出目录中。
2. 在 `AppxManifest.xml` 中声明 VPN 扩展：
   ```xml
   <Extensions>
       <uap3:Extension Category="windows.vpnClient" EntryPoint="YourApp.VpnBackgroundTask" />
   </Extensions>
   ```
3. 在前台代码中调用 `VpnManagementAgent.AddProfileFromObjectAsync` 注册 VPN 配置文件。