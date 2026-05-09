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

## License

MIT
