#include <iostream>
#include <cstdlib>
#include <string>
#include <memory>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/Windows.System.Power.h>
#include <winrt/Windows.Networking.Connectivity.h>

#pragma comment(lib, "windowsapp.lib")

static bool g_winrt_initialized = false;

static char* StrDup(const std::string& s) {
    char* p = (char*)malloc(s.size() + 1);
    memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

extern "C" {

__declspec(dllexport) void WinRT_Init() {
    if (!g_winrt_initialized) {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        g_winrt_initialized = true;
    }
}

__declspec(dllexport) void WinRT_Uninit() {
    if (g_winrt_initialized) {
        winrt::uninit_apartment();
        g_winrt_initialized = false;
    }
}

__declspec(dllexport) int Add(int a, int b) {
    return a + b;
}

__declspec(dllexport) char* Greet(const char* name) {
    std::string greeting = "Hello, " + std::string(name) + "! From C++/WinRT.";
    return StrDup(greeting);
}

__declspec(dllexport) int Multiply(int a, int b) {
    return a * b;
}

__declspec(dllexport) char* GetWinRTVersionInfo() {
    try {
        auto versionInfo = winrt::Windows::System::Profile::AnalyticsInfo::VersionInfo();
        auto deviceFamily = winrt::Windows::System::Profile::AnalyticsInfo::DeviceForm();
        std::string result = "DeviceFamily=" + winrt::to_string(deviceFamily);
        if (versionInfo) {
            result += ", Version=" + winrt::to_string(versionInfo.DeviceFamilyVersion());
        }
        result += " [OK]";
        return StrDup(result);
    }
    catch (const winrt::hresult_error& ex) {
        return StrDup("ERROR: " + winrt::to_string(ex.message()));
    }
}

__declspec(dllexport) char* GetWinRTPowerStatus() {
    try {
        auto es = winrt::Windows::System::Power::PowerManager::EnergySaverStatus();
        auto bs = winrt::Windows::System::Power::PowerManager::BatteryStatus();
        auto ps = winrt::Windows::System::Power::PowerManager::PowerSupplyStatus();
        std::string result = "EnergySaver=" + std::to_string((int)es)
            + ", Battery=" + std::to_string((int)bs)
            + ", PowerSupply=" + std::to_string((int)ps) + " [OK]";
        return StrDup(result);
    }
    catch (const winrt::hresult_error& ex) {
        return StrDup("ERROR: " + winrt::to_string(ex.message()));
    }
}

__declspec(dllexport) char* GetWinRTNetworkInfo() {
    try {
        auto profile = winrt::Windows::Networking::Connectivity::NetworkInformation::GetInternetConnectionProfile();
        std::string result;
        if (profile) {
            result = "Network: " + winrt::to_string(profile.ProfileName())
                + ", Level=" + std::to_string((int)profile.GetNetworkConnectivityLevel());
        }
        else {
            result = "No internet profile";
        }
        result += " [OK]";
        return StrDup(result);
    }
    catch (const winrt::hresult_error& ex) {
        return StrDup("ERROR: " + winrt::to_string(ex.message()));
    }
}

__declspec(dllexport) char* GetWinRTAppDataInfo() {
    try {
        winrt::Windows::Foundation::Collections::ValueSet vs;
        winrt::hstring key1 = L"TestKey1";
        winrt::hstring key2 = L"TestKey2";
        winrt::hstring key3 = L"TestKey3";
        vs.Insert(key1, winrt::box_value(L"Hello from C++/WinRT!"));
        vs.Insert(key2, winrt::box_value(42));
        vs.Insert(key3, winrt::box_value(3.14));
        auto val1 = vs.Lookup(key1);
        auto str1 = winrt::unbox_value<winrt::hstring>(val1);
        auto val2 = vs.Lookup(key2);
        auto int2 = winrt::unbox_value<int32_t>(val2);
        auto val3 = vs.Lookup(key3);
        auto dbl3 = winrt::unbox_value<double>(val3);
        std::string result = "Insert+Lookup OK: " + winrt::to_string(str1)
            + ", " + std::to_string(int2)
            + ", " + std::to_string(dbl3);
        result += ", HasKey=" + std::string(vs.HasKey(key1) ? "true" : "false");
        vs.Remove(key1);
        result += ", Remove OK";
        result += ", HasKey=" + std::string(vs.HasKey(key1) ? "true" : "false") + " [OK]";
        return StrDup(result);
    }
    catch (const winrt::hresult_error& ex) {
        return StrDup("ERROR: " + winrt::to_string(ex.message()));
    }
}

__declspec(dllexport) char* GetWinRTStringStress() {
    try {
        int count = 0;
        for (int i = 0; i < 100; i++) {
            winrt::hstring hs = winrt::to_hstring("Stress_") + winrt::to_hstring(i);
            count += (int)hs.size();
        }
        std::string result = "100 HSTRINGs stress test OK (total chars: " + std::to_string(count) + ")";
        return StrDup(result);
    }
    catch (const winrt::hresult_error& ex) {
        return StrDup("ERROR: " + winrt::to_string(ex.message()));
    }
}

__declspec(dllexport) void ProcessMessage(const char* message) {
    try {
        winrt::hstring hs = winrt::to_hstring(message);
        std::string roundtrip = winrt::to_string(hs);
        std::cout << "  [C++/WinRT] ProcessMessage: " << roundtrip << std::endl;
    }
    catch (const winrt::hresult_error& ex) {
        std::cout << "  [C++/WinRT] Exception: " << winrt::to_string(ex.message()) << std::endl;
    }
}

__declspec(dllexport) void FreeString(char* p) {
    if (p) {
        free(p);
    }
}

typedef int (*GoBinaryOp)(int a, int b);
typedef void (*GoStringCallback)(const char* msg);

__declspec(dllexport) int CallGoBinaryOp(GoBinaryOp fn, int a, int b) {
    std::cout << "  [C++/WinRT] About to call Go callback: BinaryOp(" << a << ", " << b << ")" << std::endl;
    int result = fn(a, b);
    std::cout << "  [C++/WinRT] Go callback returned: " << result << std::endl;
    return result;
}

__declspec(dllexport) void CallGoStringCallbackMultipleTimes(GoStringCallback fn) {
    std::cout << "  [C++/WinRT] Starting multi-callback test ..." << std::endl;
    fn("Hello from C++/WinRT - 1st call");
    fn("Hello from C++/WinRT - 2nd call");
    fn("Hello from C++/WinRT - 3rd call");
    std::cout << "  [C++/WinRT] Multi-callback test completed" << std::endl;
}

__declspec(dllexport) char* WinRT_Then_Call_Go_Callback(GoStringCallback fn) {
    try {
        auto profile = winrt::Windows::Networking::Connectivity::NetworkInformation::GetInternetConnectionProfile();
        std::string netInfo;
        if (profile) {
            netInfo = "Network: " + winrt::to_string(profile.ProfileName());
        }
        else {
            netInfo = "No internet profile";
        }
        
        std::string msgToGo = "[From WinRT] NetworkInfo: " + netInfo;
        std::cout << "  [C++/WinRT] WinRT fetched data, now passing to Go callback" << std::endl;
        
        fn(msgToGo.c_str());
        
        std::string finalResult = "WinRT -> Go callback chain [OK]";
        return StrDup(finalResult);
    }
    catch (const winrt::hresult_error& ex) {
        return StrDup("ERROR: " + winrt::to_string(ex.message()));
    }
}

}
