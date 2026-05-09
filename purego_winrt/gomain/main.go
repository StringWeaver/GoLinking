package main

import (
	"fmt"
	"os"
	"path/filepath"
	"syscall"
	"unsafe"

	"github.com/ebitengine/purego"
)

// 回调函数类型定义 - 和 C++ 侧完全对应
type GoBinaryOp func(a int, b int) int
type GoStringCallback func(msg *byte) uintptr

func main() {
	fmt.Println("======================================================")
	fmt.Println(" Purego + MSVC C++/WinRT Test (Zero CGO!)")
	fmt.Println(" Including Callbacks: Go defines function, C++ calls back!")
	fmt.Println("======================================================")
	fmt.Println()

	exePath, err := os.Executable()
	if err != nil {
		fmt.Printf("[ERROR] Cannot get exe path: %v\n", err)
		return
	}
	buildDir := filepath.Dir(exePath)
	dllPath := filepath.Join(buildDir, "winrt_dll.dll")

	fmt.Printf("[Setup] Loading DLL from: %s\n", dllPath)
	fmt.Println()

	lib, err := syscall.LoadLibrary(dllPath)
	if err != nil {
		fmt.Printf("[ERROR] Failed to load DLL: %v\n", err)
		return
	}
	defer syscall.FreeLibrary(lib)

	fmt.Println("[Setup] DLL loaded successfully!")
	fmt.Println()

	var (
		winrtInit                    func()
		winrtUninit                  func()
		add                          func(a int, b int) int
		greet                        func(name *byte) *byte
		multiply                     func(a int, b int) int
		getWinRTVersionInfo          func() *byte
		getWinRTPowerStatus          func() *byte
		getWinRTNetworkInfo          func() *byte
		getWinRTAppDataInfo          func() *byte
		getWinRTStringStress         func() *byte
		processMessage               func(msg *byte)
		freeString                   func(p *byte)
		callGoBinaryOp               func(fn uintptr, a int, b int) int
		callGoStringCallbackMultiple func(fn uintptr)
		winRTThenCallGoCallback      func(fn uintptr) *byte
	)

	purego.RegisterLibFunc(&winrtInit, uintptr(lib), "WinRT_Init")
	purego.RegisterLibFunc(&winrtUninit, uintptr(lib), "WinRT_Uninit")
	purego.RegisterLibFunc(&add, uintptr(lib), "Add")
	purego.RegisterLibFunc(&greet, uintptr(lib), "Greet")
	purego.RegisterLibFunc(&multiply, uintptr(lib), "Multiply")
	purego.RegisterLibFunc(&getWinRTVersionInfo, uintptr(lib), "GetWinRTVersionInfo")
	purego.RegisterLibFunc(&getWinRTPowerStatus, uintptr(lib), "GetWinRTPowerStatus")
	purego.RegisterLibFunc(&getWinRTNetworkInfo, uintptr(lib), "GetWinRTNetworkInfo")
	purego.RegisterLibFunc(&getWinRTAppDataInfo, uintptr(lib), "GetWinRTAppDataInfo")
	purego.RegisterLibFunc(&getWinRTStringStress, uintptr(lib), "GetWinRTStringStress")
	purego.RegisterLibFunc(&processMessage, uintptr(lib), "ProcessMessage")
	purego.RegisterLibFunc(&freeString, uintptr(lib), "FreeString")
	purego.RegisterLibFunc(&callGoBinaryOp, uintptr(lib), "CallGoBinaryOp")
	purego.RegisterLibFunc(&callGoStringCallbackMultiple, uintptr(lib), "CallGoStringCallbackMultipleTimes")
	purego.RegisterLibFunc(&winRTThenCallGoCallback, uintptr(lib), "WinRT_Then_Call_Go_Callback")

	fmt.Println("[Setup] WinRT_Init() - Initialize COM Apartment once")
	winrtInit()
	fmt.Println()

	fmt.Println("--- Test 1: Basic function Add ---")
	r1 := add(10, 20)
	fmt.Printf("  Add(10, 20) = %d\n", r1)
	fmt.Println()

	fmt.Println("--- Test 2: Basic function Greet ---")
	name := CString("Purego")
	greetingPtr := greet(name)
	greeting := GoString(greetingPtr)
	freeString(greetingPtr)
	fmt.Printf("  Greet(\"Purego\") = %s\n", greeting)
	fmt.Println()

	fmt.Println("--- Test 3: Basic Multiply ---")
	r3 := multiply(7, 8)
	fmt.Printf("  Multiply(7, 8) = %d\n", r3)
	fmt.Println()

	fmt.Println("--- Test 4: WinRT - System Version ---")
	verPtr := getWinRTVersionInfo()
	verStr := GoString(verPtr)
	freeString(verPtr)
	fmt.Printf("  Result: %s\n", verStr)
	fmt.Println()

	fmt.Println("--- Test 5: WinRT - Power Status ---")
	powerPtr := getWinRTPowerStatus()
	powerStr := GoString(powerPtr)
	freeString(powerPtr)
	fmt.Printf("  Result: %s\n", powerStr)
	fmt.Println()

	fmt.Println("--- Test 6: WinRT - Network Info ---")
	netPtr := getWinRTNetworkInfo()
	netStr := GoString(netPtr)
	freeString(netPtr)
	fmt.Printf("  Result: %s\n", netStr)
	fmt.Println()

	fmt.Println("--- Test 7: WinRT - ValueSet/IMap Collection ---")
	appPtr := getWinRTAppDataInfo()
	appStr := GoString(appPtr)
	freeString(appPtr)
	fmt.Printf("  Result: %s\n", appStr)
	fmt.Println()

	fmt.Println("--- Test 8: WinRT - HSTRING Stress Test ---")
	stressPtr := getWinRTStringStress()
	stressStr := GoString(stressPtr)
	freeString(stressPtr)
	fmt.Printf("  Result: %s\n", stressStr)
	fmt.Println()

	fmt.Println("--- Test 9: ProcessMessage (Go -> C++/WinRT string) ---")
	msg := CString("Message from Purego Go!")
	processMessage(msg)
	fmt.Println()

	fmt.Println("=== CALLBACK FEATURE: Go function passed to C++/WinRT! ===")
	fmt.Println()

	fmt.Println("--- Test 10: Callback 1 - Go implements Square(a*a) ---")
	var goSquare GoBinaryOp = func(a int, b int) int {
		fmt.Printf("    [GO CALLBACK EXECUTED!] Square: a=%d, b=%d\n", a, b)
		return a * a
	}
	cbSquare := syscall.NewCallback(goSquare)
	result10 := callGoBinaryOp(cbSquare, 12, 0)
	fmt.Printf("  Final Result: 12 * 12 = %d\n", result10)
	fmt.Println()

	fmt.Println("--- Test 11: Callback 2 - Multiple string callbacks from C++ ---")
	var callCount int
	var goPrintMessage GoStringCallback = func(msgPtr *byte) uintptr {
		s := GoString(msgPtr)
		callCount++
		fmt.Printf("    [GO CALLBACK #%d RECEIVED!] Message: %s\n", callCount, s)
		return 0
	}
	cbPrint := syscall.NewCallback(goPrintMessage)
	callGoStringCallbackMultiple(cbPrint)
	fmt.Println()

	fmt.Println("--- Test 12: Callback 3 - WinRT fetches data THEN calls Go! ---")
	var lastGoCallback GoStringCallback = func(msgPtr *byte) uintptr {
		s := GoString(msgPtr)
		fmt.Printf("    [GO CALLBACK AFTER WINRT!] Got from WinRT: %s\n", s)
		return 0
	}
	cbLast := syscall.NewCallback(lastGoCallback)
	finalResultPtr := winRTThenCallGoCallback(cbLast)
	finalResultStr := GoString(finalResultPtr)
	freeString(finalResultPtr)
	fmt.Printf("  Overall chain result: %s\n", finalResultStr)
	fmt.Println()

	fmt.Println("[Cleanup] WinRT_Uninit()")
	winrtUninit()
	fmt.Println()

	fmt.Println("======================================================")
	fmt.Println(" ALL TESTS COMPLETED! (Zero CGO + Callbacks!) ")
	fmt.Println("======================================================")
}

func CString(s string) *byte {
	b := append([]byte(s), 0)
	return &b[0]
}

func GoString(p *byte) string {
	if p == nil {
		return ""
	}
	var b []byte
	for ptr := uintptr(unsafe.Pointer(p)); ; ptr++ {
		c := *(*byte)(unsafe.Pointer(ptr))
		if c == 0 {
			break
		}
		b = append(b, c)
	}
	return string(b)
}
