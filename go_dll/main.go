package main

import "C"
import (
	"context"
	"fmt"
	"os"
	"strings"
	"sync"

	box "github.com/sagernet/sing-box"
	"github.com/sagernet/sing-box/include"
	"github.com/sagernet/sing-box/option"
	sjson "github.com/sagernet/sing/common/json"

	_ "github.com/anytls/sing-anytls"
)

var (
	instance *box.Box
	mu       sync.Mutex
	cancel   context.CancelFunc
)

//export LibboxHello
func LibboxHello() *C.char {
	return C.CString("Hello from Go Libbox (UWP Bridge)!")
}

//export LibboxStart
func LibboxStart(configJSON *C.char, workingDir *C.char) *C.char {
	mu.Lock()
	defer mu.Unlock()

	if instance != nil {
		return C.CString("service already running")
	}

	workDirStr := C.GoString(workingDir)
	if workDirStr != "" {
		if err := os.Chdir(workDirStr); err != nil {
			return C.CString(fmt.Sprintf("failed to set working dir to %s: %v", workDirStr, err))
		}
	}

	configStr := C.GoString(configJSON)

	ctx, cancelFunc := context.WithCancel(context.Background())
	cancel = cancelFunc
	ctx = include.Context(ctx)

	var options option.Options
	if err := sjson.UnmarshalContext(ctx, []byte(configStr), &options); err != nil {
		cancel()
		cancel = nil
		return C.CString(fmt.Sprintf("decode config error: %s", err))
	}

	var err error
	instance, err = box.New(box.Options{
		Context: ctx,
		Options: options,
	})
	if err != nil {
		cancel()
		cancel = nil
		return C.CString(fmt.Sprintf("create service error: %s", err))
	}

	if err := instance.Start(); err != nil {
		instance.Close()
		instance = nil
		cancel()
		cancel = nil
		return C.CString(fmt.Sprintf("start service error: %s", err))
	}

	return nil // Success
}

//export LibboxStop
func LibboxStop() *C.char {
	mu.Lock()
	defer mu.Unlock()

	if instance == nil {
		return C.CString("service not running")
	}

	if cancel != nil {
		cancel()
		cancel = nil
	}

	if err := instance.Close(); err != nil {
		if !strings.Contains(err.Error(), "service not running") {
			return C.CString(fmt.Sprintf("close service error: %s", err))
		}
	}

	instance = nil
	return nil
}

func main() {}
