# GoLinking

验证 Go 编译的 DLL（`c-shared`）能否被 MSVC 编译的 C++ 程序正确链接和调用——包括传递 C++ 函数指针到 Go、在 Go 中回调 C++、以及在 C++ 回调中使用 C++/WinRT 的最坏情况。

## 核心问题

| 问题 | 结论 |
|------|------|
| Go `c-shared` DLL 能否被 MSVC C++ 链接？ | ✅ 可以。Go 生成 `.dll` + `.h`，通过 `dumpbin` + `lib.exe` 生成 MSVC 导入库 `.lib` |
| C++ 函数指针能否传给 Go 并回调？ | ✅ 可以。cgo 不允许直接调用 C 函数指针，需写 C 桥接函数 |
| Go 调 C++ 时 CRT 是否变成 MinGW 的？ | ⚠️ Go 内部用 MinGW CRT，但回调进入 C++ 后回到 MSVC CRT，两者互不干扰 |
| C++ 回调中使用 C++/WinRT 会不会因不同 UCRT 出问题？ | ✅ 不会。UCRT 是 Windows 10+ 的系统组件，进程内只有一份，Go/MinGW 和 MSVC 共享同一个 UCRT |
| IPropertySet / IMap 泛型集合在 Go 回调链路中是否正常？ | ✅ 正常。`Insert`/`Lookup`/`Remove`/`HasKey` + `box_value`/`unbox_value` 全部通过 |

## 项目结构

```
GoLinking/
├── golib/
│   ├── go.mod          # Go module
│   └── golib.go        # Go 库：导出函数 + C 函数指针桥接
├── cpp/
│   └── main.cpp        # MSVC C++ 主程序：WinRT 回调测试
├── build.ps1           # 一键构建脚本（PowerShell）
├── compile_cpp.bat     # MSVC 编译辅助脚本
└── README.md
```

## 构建与运行

### 前置条件

- **Go** 1.21+（需支持 `c-shared` 构建模式）
- **Visual Studio** 2022+（需 MSVC C++ 编译器和 Windows SDK）
- **C++/WinRT**（Windows SDK 10.0.19041+ 自带）
- **PowerShell** 5.1+

### 一键构建

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

脚本自动完成以下步骤：

1. 加载 MSVC 环境变量（`vcvarsall.bat x64`）
2. 编译 Go `c-shared` DLL → `build/golib.dll` + `build/golib.h`
3. 用 `dumpbin` + `lib.exe` 从 DLL 生成 MSVC 导入库 → `build/golib.lib`
4. 用 MSVC 编译 C++ 代码（`/std:c++20`，链接 `WindowsApp.lib`）→ `build/test_winrt.exe`
5. 运行测试（10 秒超时保护）

### 手动构建

```powershell
# 1. 编译 Go DLL
cd golib
go build -buildmode=c-shared -o ../build/golib.dll .

# 2. 生成 MSVC 导入库
cd ..\build
dumpbin /exports golib.dll > exports.txt
# 从 exports.txt 提取符号名，编写 golib.def
lib.exe /def:golib.def /out:golib.lib /machine:x64

# 3. MSVC 编译 C++（需要设置 vcvarsall 环境）
cl.exe /EHsc /std:c++20 /I"build" main.cpp build/golib.lib /link WindowsApp.lib /OUT:test_winrt.exe
```

## 测试用例

| # | 测试 | 说明 |
|---|------|------|
| 1 | 基本 Go 函数 | `Add()`, `Greet()` — 验证基础链接 |
| 2 | C++ 函数指针 → Go 回调 | `CallCallback(Multiply, 7, 8)` — 验证函数指针传递 |
| 3 | WinRT 系统版本 | `AnalyticsInfo::VersionInfo()` — 静态属性读取 |
| 4 | WinRT 电源状态 | `PowerManager::EnergySaverStatus()` 等多个属性 |
| 5 | WinRT 网络信息 | `NetworkInformation::GetInternetConnectionProfile()` |
| 6 | WinRT IPropertySet/IMap | `ValueSet` 的 `Insert`/`Lookup`/`Remove`/`HasKey` + `box_value`/`unbox_value` — 泛型集合最坏情况 |
| 7 | HSTRING 压力测试 | 100 次 `hstring` 创建与拼接 |
| 8 | Go → C++/WinRT 字符串 | Go 发送字符串，C++ 做 `hstring` 往返转换 |

## 重要发现：COM Apartment 生命周期

> **永远不要在每次回调中调用 `init_apartment` / `uninit_apartment`。**

- `uninit_apartment()` 调用 `CoUninitialize()`，销毁 COM Apartment
- C++/WinRT 内部有静态缓存（activation factories 等），`uninit` 后这些缓存变成悬垂指针
- 再次 `init_apartment` 重建 Apartment，但残留缓存仍被引用，导致 `ACCESS_VIOLATION`
- 即使是纯 C++（不涉及 Go）也会在第二次 `init`/`uninit` 循环时崩溃
- **正确做法：** `init_apartment` 只在程序启动时调用一次，`uninit_apartment` 仅在进程退出时调用（或完全不调用）

## 关键技术细节

### cgo 函数指针桥接

cgo 不允许直接调用 C 函数指针，必须在 C 注释块中写桥接函数：

```go
/*
typedef int (*BinOpFunc)(int a, int b);
static int call_binop(BinOpFunc fn, int a, int b) {
    return fn(a, b);  // 桥接：Go 调用此函数，此函数调用回调
}
*/
import "C"

//export CallCallback
func CallCallback(fn C.BinOpFunc, a C.int, b C.int) C.int {
    return C.call_binop(fn, a, b)
}
```

### MSVC 导入库生成

Go `c-shared` 产生的 `.lib` 是 MinGW 格式，MSVC 无法直接使用。解决方法：

1. `dumpbin /exports golib.dll` 提取导出符号
2. 生成 `.def` 定义文件
3. `lib.exe /def:golib.def /out:golib.lib /machine:x64` 生成 MSVC 格式导入库

### UCRT 共享

Windows 10+ 的 UCRT（Universal C Runtime）是系统组件，以 DLL 形式存在（`ucrtbase.dll`）。Go/MinGW 和 MSVC 在同一进程中共享同一份 UCRT，不会出现 CRT 冲突。

---

## 进阶：WinRT VPN 的 Fake Socket + 绕过 Decapsulate Hack

本仓库的 `cgo_winrt` 实验只验证了"Go ↔ C++/WinRT 能互调"。真正把这些技术用到 **Windows WinRT VPN（`windows.networking.vpn`）** 上时，会遇到微软原生 `IVpnPlugIn` 事件驱动模型与现代代理引擎（Mux/并发连接池）冲突的问题。下面记录一套**已验证可行**的 hack 方案，完整实现见 `Maple/Maple.Task/VpnPlugin.cpp`。

### 问题背景

微软的 `IVpnPlugIn` 设计假设你有一个**真实的外层隧道传输**（TLS/UDP 到 VPN 服务器）：

- `Encapsulate(channel, packets, encapulatedPackets)`：系统把出站 IP 包交给你，你**封装后写回 `encapulatedPackets`**，系统发到 transport
- `Decapsulate(channel, encapBuffer, decapsulatedPackets, controlPacketsToSend)`：系统从 transport 收到封装包，你**拆解后写回 `decapsulatedPackets`**

但代理引擎（sing-box/leaf 这类）的工作模型是：
- 出站包直接进用户态 TCP/UDP 栈，不需要"封装到外层传输"
- 入站包由代理连接异步产生，没有"系统递给我一个封装包让我拆"的同步时机

硬塞进 `Encapsulate`/`Decapsulate` 的同步模型会阻塞 COM 线程，且无法表达代理引擎的异步 Mux 语义。

### Hack 核心：Fake Socket + 主动注入

**思路**：用一个自连接的 `DatagramSocket` 骗过 `StartWithMainTransport` 的"必须给一个已连接 transport"的检查，然后**完全绕过 `Decapsulate`**，入站包由代理引擎异步产生后主动调用 `VpnChannel` 的 buffer API 注入。

#### 1. Fake Socket：自连接的 UDP 满足 transport 检查

```cpp
// Maple/Maple.Task/VpnPlugin.cpp :: ConnectCore
const auto localhost = HostName{ L"127.0.0.1" };
DatagramSocket transport{};
channel.AssociateTransport(transport, nullptr);
transport.BindEndpointAsync(localhost, L"").get();
// 关键：让 socket 连到自己，这样 StartWithMainTransport 认为它"已连接"
transport.ConnectAsync(localhost, transport.Information().LocalPort()).get();
```

`BindEndpointAsync` 绑到 127.0.0.1 随机端口，再 `ConnectAsync` 到自己的端口。系统看到 transport 处于"已连接"状态就放行了，实际上没有任何真实流量经过这个 socket。

#### 2. 路由：用两个 /1 覆盖整个 IPv4 空间

```cpp
VpnRouteAssignment routeScope{};
routeScope.ExcludeLocalSubnets(true);
routeScope.Ipv4InclusionRoutes(std::vector{
    VpnRoute(HostName{ L"0.0.0.0" }, 1),
    VpnRoute(HostName{ L"128.0.0.0" }, 1),
});
routeScope.Ipv6InclusionRoutes(std::vector{
    VpnRoute(HostName{L"::"}, 1),
    VpnRoute(HostName{L"8000::"}, 1)
});
```

不能直接写 `0.0.0.0/0`（即使绑了接口也会绕回环）。用 `0.0.0.0/1` + `128.0.0.0/1` 两个 /1 路由覆盖整个 IPv4 地址空间，这是 TUN 类工具的经典技巧。IPv6 同理用 `::/1` + `8000::/1`。

#### 3. 出站：Encapsulate 只做搬运，不写 encapulatedPackets

```cpp
// Maple/Maple.Task/VpnPlugin.cpp :: Encapsulate
void VpnPlugin::Encapsulate(VpnChannel const&, VpnPacketBufferList const& packets, VpnPacketBufferList const&)
{
    auto packetCount = packets.Size();
    while (packetCount-- > 0) {
        const auto packet = packets.RemoveAtBegin();
        const auto buffer = packet.Buffer();
        // 直接把原始 IP 包交给代理引擎的 netstack，不做任何封装
        netstack_send(m_netStackHandle, buffer.data(), static_cast<size_t>(buffer.Length()));
        packets.Append(packet);  // 立即归还 buffer 给系统池
    }
}
```

关键点：
- **第二个参数 `encapulatedPackets` 完全不写**——我们不往 fake transport 发任何东西
- `packets.Append(packet)` 立即归还 packet buffer，避免系统池耗尽
- `netstack_send` 内部会 `memcpy` 数据（如果代理引擎在另一个线程异步处理），所以归还后不会有悬垂指针问题

#### 4. 入站：绕过 Decapsulate，主动注入

```cpp
// Maple/Maple.Task/VpnPlugin.cpp :: cb (netstack 回调)
extern "C" {
    typedef void(__cdecl* netstack_cb)(uint8_t*, size_t, void*);
    void cb(uint8_t* data, size_t size, void* channelAbi) {
        // channelAbi 是 Connect 时通过 winrt::detach_abi 取出的 VpnChannel 原始指针
        VpnChannel channel{ nullptr };
        winrt::attach_abi(channel, channelAbi);
        try {
            const auto outBuffer = channel.GetVpnReceivePacketBuffer();
            const auto outBuf = outBuffer.Buffer();
            if (memcpy_s(outBuf.data(), outBuf.Capacity(), data, size) == 0) {
                outBuf.Length(static_cast<uint32_t>(size));
                channel.AppendVpnReceivePacketBuffer(outBuffer);
                channel.FlushVpnReceivePacketBuffers();
            }
        }
        catch (...) {}
        winrt::detach_abi(channel);  // 不要让局部变量析构释放 ABI
    }
}
```

`Decapsulate` 留空：

```cpp
void VpnPlugin::Decapsulate(VpnChannel const&, VpnPacketBuffer const&, VpnPacketBufferList const&, VpnPacketBufferList const&)
{
    // 入站包现在通过 cb() 主动注入，Decapsulate 不再是入站入口
}
```

三个关键 API：
- `GetVpnReceivePacketBuffer()`：从系统池申请一个接收缓冲
- `AppendVpnReceivePacketBuffer(buf)`：把填好的缓冲追加到接收队列
- `FlushVpnReceivePacketBuffers()`：刷新队列，通知系统有入站包到达

#### 5. VpnChannel 的跨线程传递：detach_abi / attach_abi

`VpnChannel` 是 WinRT 对象，有引用计数，不能直接把 C++ 对象指针传给 C 回调。正确做法是用 `winrt::detach_abi` 取出原始 ABI 指针（同时增加引用计数），传给 C 侧的 `void*` context，回调里再用 `winrt::attach_abi` 重新包装成 C++ 对象：

```cpp
// Connect 时：取出 ABI，传给 netstack 作为 context
const auto channelAbi = winrt::detach_abi(m_channel);  // 引用计数 +1
m_netStackHandle = netstack_register(cb, channelAbi);

// 释放时：netstack_release 返回 context，需要释放 ABI 引用
auto context = netstack_release(netStackHandle);
IInspectable obj{};
winrt::attach_abi(obj, context);  // obj 析构时自动 Release
```

### C 侧 netstack 接口约定

Maple 的 `leaf.h` 定义了代理引擎（Leaf，Rust 编译）和 C++ VPN 插件之间的纯 C 接口：

```c
// Maple/Maple.Task/leaf.h
typedef void NetStackHandle;
typedef int32_t NetStackSendResult;

// 注册入站回调，返回 netstack 句柄。context 会原样传回回调。
NetStackHandle* netstack_register(void on_receive(uint8_t*, size_t, void*), void* context);

// 出站：把 IP 包交给代理引擎
NetStackSendResult netstack_send(NetStackHandle*, uint8_t*, size_t);

// 释放：返回当初传入的 context（调用方负责释放 ABI 引用）
void* netstack_release(NetStackHandle* ptr);
```

这套接口和 `cgo_winrt` 实验验证的"Go 导出函数 + C 函数指针回调"模式完全对应——只是把 Go 换成了 Rust（Leaf），桥接逻辑一致。

### 完整数据流

```
出站 (OS → 代理引擎):
  系统 → VpnPlugin::Encapsulate → netstack_send → Leaf netstack → 代理出站

入站 (代理引擎 → OS):
  Leaf 代理连接收到数据 → netstack 回调 cb() →
    GetVpnReceivePacketBuffer + memcpy + AppendVpnReceivePacketBuffer + FlushVpnReceivePacketBuffers
    → 系统协议栈 → 应用层
```

### 为什么这套 Hack 成立

1. **Fake Socket 不会被系统检查流量**：`StartWithMainTransport` 只在启动时检查 transport 是否已连接，运行时不校验是否有真实流量
2. **`Encapsulate` 不写 `encapulatedPackets` 不会断开通道**：系统只是没收到外层封装包，但 VPN 接口对 OS 仍然有效，出站包已经被 `Encapsulate` 拿走交给代理引擎了
3. **`Decapsulate` 可以完全留空**：入站包的来源是代理引擎的异步连接，不是外层 transport，所以入站走主动注入而非事件回调是正确的
4. **`AppendVpnReceivePacketBuffer` 可以在任意线程调用**：`VpnChannel` 的 buffer API 是线程安全的，`FlushVpnReceivePacketBuffers` 负责通知系统

### 已知限制

- **KeepAlive**：`GetKeepAlivePayload` 留空，长连接空闲时系统可能不会主动发 keep-alive。如果系统有 keep-alive 超时，需要在 `GetKeepAlivePayload` 里填一个最小 IP 包
- **Fake Socket 的端口占用**：自连接 UDP 会占用一个 127.0.0.1 端口，多实例时需注意
- **`ExcludeLocalSubnets(true)`**：必须排除本地子网，否则访问局域网设备会绕回 VPN 接口

---

## 进阶：UWP VPN broker 进程模型 vs iOS Extension 进程模型

上面的 hack 解决了"如何把代理引擎塞进 WinRT VPN"的**数据流**问题。但如果要把 mihomo（Go）或 Leaf（Rust）这样的代理引擎**完整集成**到 UWP VPN 体系中，还必须理解 broker 进程与 iOS Extension 进程的**进程模型差异**——这决定了代理引擎运行时的生命周期、线程模型和能力边界。

### 核心差异：谁拥有进程

| | iOS Extension | Android VpnService | UWP broker + IVpnPlugIn DLL |
|---|---|---|---|
| **进程谁拥有** | Extension 代码（自己有 `main()`） | App 自己的进程 | 系统 broker 代码（DLL 无 `main()`） |
| **主循环谁驱动** | Extension 自己的 `NSRunLoop`/`CFRunLoop` | App 的 Looper/MessageQueue | 系统的 COM 消息循环 |
| **VPN 事件到达方式** | run loop 分发到 delegate 回调 | Service 生命周期 + 回调 | COM 虚函数调用 `IVpnPlugIn` 方法 |
| **代理引擎在哪跑** | Extension 进程内（Go runtime 线程） | App 进程内（Go runtime 线程） | broker 进程内（Go runtime 线程，作为 guest） |
| **能否控制进程退出** | ✅ 能（退出 main 即退进程） | ✅ 能 | ❌ 不能（Go 退出只停 runtime，broker 仍活着） |
| **进程崩了谁重启** | 系统 `nesmartcl` | 系统 ActivityManager | 系统 broker 管理器 |

### iOS Extension：代理引擎是进程的"主人"

iOS 的 `NEPacketTunnelProvider` 必须实现为 **App Extension**，而 App Extension 在 iOS 上的定义就是**由系统拉起的独立进程**：

```
iOS Extension 进程:
  main() → 属于 Extension 代码
    └→ NSRunLoop ← Extension 自己拥有和驱动
    └→ nesmartcl 通过 XPC 发来事件 → run loop 分发
    └→ Extension 代码掌控进程生命周期
    └→ mihomo Go runtime 在"自己的地盘"上运行
```

iOS 强制 Extension 为独立进程的原因：

1. **后台存活**：iOS 对后台 App 极其激进——切后台几秒即 suspend。如果 VPN 跑在 App 进程里，App 被 suspend 就断了。Extension 进程由系统 `nesmartcl` 守护，有独立后台执行权限，不跟随容器 App 生命周期。
2. **权限隔离**：普通 App 沙箱没有操作网络栈的权限。`networkextension` entitlement 只赋予 Extension 进程，不赋予容器 App 进程。VPN 和 App 在不同进程 = 不同沙箱 = 不同权限。
3. **崩溃隔离**：VPN 代码 crash 不影响 App UI，App crash 不影响 VPN 隧道。

### UWP broker：代理引擎是进程的"客人"

UWP 的 VPN 系统由系统拉起一个 **broker 进程**，加载注册的 `IVpnPlugIn` 实现（一个 DLL）。broker 进程的 `main()` 是系统代码，COM 消息循环由系统驱动，DLL 只是系统在需要时通过虚函数表调用的一个对象：

```
UWP broker 进程:
  main() → 属于系统 broker 代码（不可见、不可控）
    └→ COM 消息循环 ← 系统拥有和驱动
    └→ 系统在消息循环中调用 IVpnPlugIn 的方法
    └→ DLL 是被调用的 guest，不掌控进程
    └→ mihomo Go runtime 在"别人的地盘"上运行
```

broker 主线程大致这样运行：

```
COM 消息循环 {
    "VPN 需要连接"       → 调用 IVpnPlugIn::Connect()
    "出站 IP 包到达"     → 调用 IVpnPlugIn::Encapsulate()
    "transport 数据到达" → 调用 IVpnPlugIn::Decapsulate()
    "VPN 需要断开"       → 调用 IVpnPlugIn::Disconnect()
    定时器到期           → 调用 IVpnPlugIn::GetKeepAlivePayload()
}
```

DLL 的每个方法都是**被调者**。broker 调完就期望返回，它要继续处理消息循环。**不能在 `Encapsulate` 里阻塞等代理引擎处理完**——那会卡死 broker 的消息循环。

### Go runtime 作为 guest 的线程模型

虽然 DLL 没有主循环，但 Go c-shared 模式加载后，Go runtime 会创建自己的 OS 线程。这些线程独立于 broker 的 COM 线程运行：

```
broker 主线程 (回调驱动):
  Connect()     → 调 Go 导出函数 → 初始化 mihomo → 立即返回
  Encapsulate() → 调 Go 导出函数 → 把 IP 包丢进 channel → 立即返回
  Decapsulate() → 空函数，直接返回

Go runtime 自己的线程 (后台独立运行):
  线程1: Go scheduler (M machine)
  线程2: Go GC、sysmon
  线程3: 网络轮询器 (IOCP)
  线程4: gVisor stack goroutine → 从 channel 读 IP 包 → 处理 → 代理出站
  线程5: 代理引擎 goroutine → 收到入站数据 → 调 C++ 回调 → VpnChannel 注入
```

这正是前面 Hack 方案的做法：

- **出站（被动入场）**：broker 调 `Encapsulate` → Go 导出函数把包丢进 channel → 立即返回 → Go 后台线程从 channel 取包处理
- **入站（主动注入）**：Go 后台线程的 goroutine 收到代理引擎数据 → 通过 cgo 调 C++ 回调 → C++ 调 `GetVpnReceivePacketBuffer` + `AppendVpnReceivePacketBuffer` + `FlushVpnReceivePacketBuffers`（线程安全 API）→ 通知系统有入站包

入站注入完全绕过了 broker 的回调模型，是 Go 线程主动推送。README 前文已说明：

> `AppendVpnReceivePacketBuffer` 可以在任意线程调用：`VpnChannel` 的 buffer API 是线程安全的，`FlushVpnReceivePacketBuffers` 负责通知系统

### Android CMFA 的参照

Android 的 ClashMetaForAndroid（CMFA）已经验证了"Go runtime 作为 guest 运行在系统拉起的进程内"这一模式：

```
Android:
  App 进程
    └→ VpnService 组件 (运行在 App 进程内)
         └→ libmihomo.so (Go c-shared)
              └→ 系统给一个 TUN fd → Go runtime 线程处理
              └→ EmbedMode = true (禁止 restart/upgrade)
```

UWP 的 broker + DLL 模式在架构上与此对应，只是：
- Android：进程是 App 自己的，Go `.so` 在 App 进程内
- UWP：进程是系统 broker 的，Go `.dll` 在 broker 进程内

mihomo 已有 `SetEmbedMode(true)` 的先例（`hub/route/patch_android.go`），UWP 场景只需类似的 build tag 做同样处理。

### 实际影响与风险

代理引擎作为 broker 进程的 guest 运行，有以下实际约束：

1. **进程生命周期不可控**：系统随时可能终止/重启 broker 进程。Go runtime 每次 DLL 加载都重新初始化，mihomo 状态（连接、配置）需持久化或从外部加载。mihomo 的 `/restart` API 在 embed mode 下已禁用，符合此约束。

2. **回调必须快速返回**：`Encapsulate`/`Decapsulate` 等方法运行在 broker 的 COM 线程上，不能阻塞。必须将工作交给后台线程（Go goroutine）异步处理，回调本身只做"丢包进 channel + 立即返回"。

3. **Go 信号处理冲突**：Go runtime 会安装 SEH/VEH 处理器，在非 Go 拥有的进程里可能与 broker 自身的异常处理冲突。可通过 `GODEBUG=asyncpreemptoff=1` 和 `GODEBUG=cgocheck=0` 缓解，需实测。

4. **API server 监听**：mihomo 的 RESTful API（`hub/route/server.go`）需要监听 TCP/Named Pipe。在 broker 进程的沙箱内可能受限，可考虑用 Named Pipe 或 Unix Socket（Windows 支持 AF_UNIX）替代。

5. **替代方案——进程外模型**：如果 broker 进程的沙箱/信号问题无法解决，可采用 IPC 方案：VpnPlugin.dll 只做 WinRT VPN 桥接（纯 C++/WinRT，不含 Go），通过共享内存 ring buffer 与独立的 mihomo.exe 进程交换 IP 包。UWP App 通过 `FullTrustProcessLauncher`（需要 `runFullTrust` capability）拉起 mihomo.exe，后者有完整 Win32 权限。此方案更接近 iOS 的"独立进程"模型，但代价是 IPC 开销和更复杂的进程管理。

