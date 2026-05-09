# Purego + C++/WinRT (Zero CGO!)

这个示例项目演示如何 **完全不使用 CGO**，用 purego 实现和原始 GoLinking 完全相同的 WinRT 调用效果！**甚至包括回调函数：Go 实现逻辑，传给 C++/WinRT 反向调用！**

## 核心区别

| 特性 | 原始 GoLinking (CGO) | Purego 方案 (Zero CGO) |
|------|---------------------|------------------------|
| CGO_ENABLED | 必须 = 1 | 必须 = 0 |
| 角色 | Go 是 DLL，被 C++ 主程序加载 | Go 是主程序，用 purego 加载 C++ DLL |
| 构建模式 | `-buildmode=c-shared` | 普通 `go build` |
| 依赖 C 编译器 | 是 | 否，纯 Go 编译 |

## 项目结构

```
purego_winrt/
├── README.md
├── build.ps1           # 一键构建脚本
├── cpp_dll/
│   ├── dllmain.cpp      # DLL 入口点
│   └── winrt_exports.cpp # 导出所有 WinRT 函数 + Go 回调接收
└── gomain/
    ├── go.mod
    └── main.go          # 纯 Go 主程序 + 完整回调演示
```

## 前置条件

- **Go** 1.21+
- **Visual Studio 2022+**（带 MSVC 和 Windows SDK，用于编译 C++ DLL）
- **PowerShell 5.1+**

## 一键构建运行

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

脚本自动完成：
1. 加载 MSVC 环境变量
2. 编译 C++ DLL（包含所有 C++/WinRT 代码）
3. 设置 `CGO_ENABLED=0` 纯 Go 编译 Go 主程序
4. 自动运行所有测试用例

## 实现的测试用例

| 编号 | 测试 | 说明 |
|------|------|------|
| 1 | Add | 基础整数加法 |
| 2 | Greet | 字符串处理 |
| 3 | Multiply | 乘法运算 |
| 4 | WinRT System Version | 读取系统版本 |
| 5 | WinRT Power Status | 电源/电池状态 |
| 6 | WinRT Network Info | 网络连接信息 |
| 7 | WinRT ValueSet/IMap | 泛型集合操作（最坏情况） |
| 8 | WinRT HSTRING Stress | 100 次 HSTRING 压力测试 |
| 9 | ProcessMessage | Go → C++/WinRT 字符串往返 |
| 10 | Callback 1 | Go 实现 Square 函数 → C++ 反向调用 |
| 11 | Callback 2 | C++ 多次调用 Go 字符串回调 |
| 12 | Callback 3 | WinRT 先获取数据 → 直接传给 Go 回调 |

## 回调实现原理（Zero CGO）

如果你需要 C++/WinRT 反向调用 Go 实现的回调函数，用标准库的 `syscall.NewCallback()` 就可以！完全不需要 CGO：

```go
// 1. 定义回调类型，和 C++ 侧的函数指针签名 1:1 完全对齐
// 注意：对于 syscall.NewCallback，即使原 C++ 函数返回 void，Go 的回调也必须返回 uintptr
type GoBinaryOp func(a int, b int) int
type GoStringCallback func(msg *byte) uintptr

// 2. 在 Go 中实现你的业务逻辑
var goSquare GoBinaryOp = func(a int, b int) int {
    fmt.Printf("[GO CALLBACK EXECUTED!] a=%d, b=%d\n", a, b)
    return a * a
}

// 3. 用 syscall.NewCallback() 把 Go 函数转换成可跨边界调用的 uintptr
cbSquare := syscall.NewCallback(goSquare)

// 4. 把这个函数指针传给 C++ DLL，它就可以直接反向调用了！
result := callGoBinaryOp(cbSquare, 12, 0)
```

**为什么这完全不需要 CGO？**
- Go 标准库的 `syscall.NewCallback` 在 Windows 上已经为你处理了 x64 调用约定
- 它自动生成符合 Windows ABI 的跳板代码，让 C/C++ 可以像调用普通原生函数一样调用 Go
- 全程没有任何 `import "C"`

## 关键技术点

1. **完全无 CGO**：`CGO_ENABLED=0`，没有任何 `import "C"`
2. **标准库加载动态库**：用 Go 标准库 `syscall.LoadLibrary` 加载 MSVC 编译的 DLL，并用 `purego.RegisterLibFunc` 绑定函数
3. **C++/WinRT 在 DLL 里**：所有复杂的 WinRT 逻辑完全在 C++ 侧实现
4. **COM Apartment 初始化一次**：DLL 导出 `WinRT_Init()`，在进程启动时调用一次
5. **回调完全支持**：用 Go 标准库 `syscall.NewCallback` 轻松把 Go 函数转成原生指针（注意：传递给 `NewCallback` 的函数必须有一个 `uintptr` 大小的返回值）

## 为什么这比 CGO 方案更好？

- ✅ Go 部分编译极其简单，零跨平台痛苦
- ✅ 纯 Go 二进制，无 MinGW CRT 依赖
- ✅ C++/WinRT 完全由 MSVC 编译器原生支持
- ✅ 两者互不干扰，各司其职
- ✅ 回调也完美支持，Go 写业务逻辑，WinRT 只负责系统交互
