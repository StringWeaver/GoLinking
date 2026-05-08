package main

/*
#include <stdlib.h>

// C function pointer types
typedef int (*BinOpFunc)(int a, int b);
typedef void (*NotifyFunc)(const char* message);
typedef char* (*WinRTStrCallback)();
typedef void (*WinRTVoidCallback)(const char* message);

// Bridge functions: cgo does not allow calling C function pointers directly
static int call_binop(BinOpFunc fn, int a, int b) {
    return fn(a, b);
}

static void call_notify(NotifyFunc fn, const char* msg) {
    fn(msg);
}

static char* call_winrt_str(WinRTStrCallback fn) {
    return fn();
}

static void call_winrt_void(WinRTVoidCallback fn, const char* msg) {
    fn(msg);
}
*/
import "C"

import (
	"fmt"
	"unsafe"
)

//export Add
func Add(a C.int, b C.int) C.int {
	return a + b
}

//export Greet
func Greet(name *C.char) *C.char {
	greeting := "Hello, " + C.GoString(name) + "! From Go."
	return C.CString(greeting)
}

//export CallCallback
func CallCallback(fn C.BinOpFunc, a C.int, b C.int) C.int {
	return C.call_binop(fn, a, b)
}

//export CallNotify
func CallNotify(fn C.NotifyFunc) {
	msg := C.CString("Hello from Go callback!")
	defer C.free(unsafe.Pointer(msg))
	C.call_notify(fn, msg)
}

//export UseCallbackInGoroutine
func UseCallbackInGoroutine(fn C.BinOpFunc, a C.int, b C.int) C.int {
	ch := make(chan C.int)
	go func() {
		result := C.call_binop(fn, a, b)
		ch <- result
	}()
	return <-ch
}

//export RegisterAndCall
func RegisterAndCall(fn C.BinOpFunc) *C.char {
	results := []string{}
	for i := C.int(1); i <= 3; i++ {
		r := C.call_binop(fn, i, i*10)
		results = append(results, fmt.Sprintf("fn(%d, %d) = %d", i, i*10, r))
	}
	output := ""
	for i, s := range results {
		if i > 0 {
			output += "; "
		}
		output += s
	}
	return C.CString(output)
}

// CallWinRTCallback: Receives a C++ function pointer that uses C++/WinRT.
// Go calls it and returns the result string.
// The C++ callback may call RoInitialize, create HSTRINGs, activate WinRT objects, etc.
//
//export CallWinRTCallback
func CallWinRTCallback(fn C.WinRTStrCallback) *C.char {
	// Call the C++/WinRT callback directly from this goroutine.
	// The callback handles its own COM apartment initialization.
	result := C.call_winrt_str(fn)
	return result
}

// CallWinRTVoidCallback: Receives a C++ void callback that uses C++/WinRT.
// Go sends a message string to it.
//
//export CallWinRTVoidCallback
func CallWinRTVoidCallback(fn C.WinRTVoidCallback) {
	msg := C.CString("Message from Go to C++/WinRT!")
	defer C.free(unsafe.Pointer(msg))
	C.call_winrt_void(fn, msg)
}

func main() {}
