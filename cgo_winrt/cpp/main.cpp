// Go DLL + MSVC C++/WinRT Callback Test
// Tests: C++ callback using C++/WinRT called from Go via c-shared DLL
//
// [IMPORTANT] init_apartment / uninit_apartment usage:
//   NEVER call init/uninit_apartment per callback. It causes ACCESS_VIOLATION (0xC0000005).
//   Root cause: uninit_apartment calls CoUninitialize which destroys the COM apartment.
//   C++/WinRT has internal static caches (activation factories etc.) that become dangling
//   after uninit. The next init_apartment re-creates the apartment but the stale caches
//   are still referenced, leading to crashes. Even pure C++ (no Go) crashes on the 2nd cycle.
//   Correct approach: init_apartment ONCE at program start, never uninit (or only at exit).

#include <iostream>
#include <cstdlib>
#include <string>
#include <memory>

// C++/WinRT headers
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/Windows.System.Power.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Storage.h>

#pragma comment(lib, "windowsapp.lib")

// Forward declarations of Go exported functions
extern "C" {
    int Add(int a, int b);
    char* Greet(const char* name);
    int CallCallback(int (*fn)(int, int), int a, int b);
    char* CallWinRTCallback(char* (*fn)());
    void CallWinRTVoidCallback(void (*fn)(const char*));
}

// ==================== Helper ====================
static char* StrDup(const std::string& s) {
    char* p = (char*)malloc(s.size() + 1);
    memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

// ==================== Basic callbacks ====================

static int Multiply(int a, int b) {
    std::cout << "  [C++ Multiply] a=" << a << ", b=" << b << std::endl;
    return a * b;
}

// ==================== WinRT callbacks ====================
// NOTE: We init the COM apartment ONCE in main() and keep it alive.
// Calling init/uninit_apartment per callback causes COM state issues.

// Callback 1: Get system version
static char* GetWinRTVersionInfo() {
    std::cout << "  [C++/WinRT] GetWinRTVersionInfo entering..." << std::endl;
    try {
        auto versionInfo = winrt::Windows::System::Profile::AnalyticsInfo::VersionInfo();
        auto deviceFamily = winrt::Windows::System::Profile::AnalyticsInfo::DeviceForm();
        std::string result = "DeviceFamily=" + winrt::to_string(deviceFamily);
        if (versionInfo) {
            result += ", Version=" + winrt::to_string(versionInfo.DeviceFamilyVersion());
        }
        result += " [OK]";
        std::cout << "  [C++/WinRT] GetWinRTVersionInfo success" << std::endl;
        return StrDup(result);
    }
    catch (const winrt::hresult_error& ex) {
        return StrDup("ERROR: " + winrt::to_string(ex.message()));
    }
}

// Callback 2: Get power status
static char* GetWinRTPowerStatus() {
    std::cout << "  [C++/WinRT] GetWinRTPowerStatus entering..." << std::endl;
    try {
        auto es = winrt::Windows::System::Power::PowerManager::EnergySaverStatus();
        auto bs = winrt::Windows::System::Power::PowerManager::BatteryStatus();
        auto ps = winrt::Windows::System::Power::PowerManager::PowerSupplyStatus();
        std::string result = "EnergySaver=" + std::to_string((int)es)
                           + ", Battery=" + std::to_string((int)bs)
                           + ", PowerSupply=" + std::to_string((int)ps) + " [OK]";
        std::cout << "  [C++/WinRT] GetWinRTPowerStatus success" << std::endl;
        return StrDup(result);
    }
    catch (const winrt::hresult_error& ex) {
        return StrDup("ERROR: " + winrt::to_string(ex.message()));
    }
}

// Callback 3: Get network info
static char* GetWinRTNetworkInfo() {
    std::cout << "  [C++/WinRT] GetWinRTNetworkInfo entering..." << std::endl;
    try {
        auto profile = winrt::Windows::Networking::Connectivity::NetworkInformation::GetInternetConnectionProfile();
        std::string result;
        if (profile) {
            result = "Network: " + winrt::to_string(profile.ProfileName())
                   + ", Level=" + std::to_string((int)profile.GetNetworkConnectivityLevel());
        } else {
            result = "No internet profile";
        }
        result += " [OK]";
        std::cout << "  [C++/WinRT] GetWinRTNetworkInfo success" << std::endl;
        return StrDup(result);
    }
    catch (const winrt::hresult_error& ex) {
        return StrDup("ERROR: " + winrt::to_string(ex.message()));
    }
}

// Callback 4: IPropertySet / IMap collection stress test
// This is the real worst-case: IMap<hstring, IInspectable> with
// Insert/Lookup/Remove, box_value/unbox_value, and HasKey.
// Using ValueSet which implements IPropertySet and can be created standalone
// (ApplicationData::Current() requires UWP package identity).
static char* GetWinRTAppDataInfo() {
    std::cout << "  [C++/WinRT] GetWinRTAppDataInfo (ValueSet/IMap) entering..." << std::endl;
    try {
        // ValueSet implements IPropertySet -> IMap<hstring, IInspectable>
        winrt::Windows::Foundation::Collections::ValueSet vs;

        // Insert: IMap<hstring, IInspectable>::Insert
        winrt::hstring key1 = L"TestKey1";
        winrt::hstring key2 = L"TestKey2";
        winrt::hstring key3 = L"TestKey3";
        vs.Insert(key1, winrt::box_value(L"Hello from C++/WinRT!"));
        vs.Insert(key2, winrt::box_value(42));
        vs.Insert(key3, winrt::box_value(3.14));

        // Lookup: IMap<hstring, IInspectable>::Lookup + unbox_value
        auto val1 = vs.Lookup(key1);
        auto str1 = winrt::unbox_value<winrt::hstring>(val1);
        auto val2 = vs.Lookup(key2);
        auto int2 = winrt::unbox_value<int32_t>(val2);
        auto val3 = vs.Lookup(key3);
        auto dbl3 = winrt::unbox_value<double>(val3);

        std::string result = "Insert+Lookup OK: " + winrt::to_string(str1)
                           + ", " + std::to_string(int2)
                           + ", " + std::to_string(dbl3);

        // HasKey
        result += ", HasKey=" + std::string(vs.HasKey(key1) ? "true" : "false");

        // Remove: IMap<hstring, IInspectable>::Remove
        vs.Remove(key1);
        result += ", Remove OK";
        result += ", HasKey=" + std::string(vs.HasKey(key1) ? "true" : "false") + " [OK]";

        std::cout << "  [C++/WinRT] GetWinRTAppDataInfo success" << std::endl;
        return StrDup(result);
    }
    catch (const winrt::hresult_error& ex) {
        return StrDup("ERROR: " + winrt::to_string(ex.message()));
    }
}

// Callback 5: HSTRING stress test
static char* GetWinRTStringStress() {
    std::cout << "  [C++/WinRT] GetWinRTStringStress entering..." << std::endl;
    try {
        int count = 0;
        for (int i = 0; i < 100; i++) {
            winrt::hstring hs = winrt::to_hstring("Stress_") + winrt::to_hstring(i);
            count += (int)hs.size();
        }
        std::string result = "100 HSTRINGs stress test OK (total chars: " + std::to_string(count) + ")";
        std::cout << "  [C++/WinRT] GetWinRTStringStress success" << std::endl;
        return StrDup(result);
    }
    catch (const winrt::hresult_error& ex) {
        return StrDup("ERROR: " + winrt::to_string(ex.message()));
    }
}

// Callback 6: void callback - Go sends string to C++/WinRT
static void WinRTProcessMessage(const char* message) {
    std::cout << "  [C++/WinRT] WinRTProcessMessage: " << message << std::endl;
    try {
        winrt::hstring hs = winrt::to_hstring(message);
        std::string roundtrip = winrt::to_string(hs);
        std::cout << "  [C++/WinRT] HSTRING round-trip OK: " << roundtrip << std::endl;
    }
    catch (const winrt::hresult_error& ex) {
        std::cout << "  [C++/WinRT] Exception: " << winrt::to_string(ex.message()) << std::endl;
    }
}

// ==================== Main ====================

int main() {
    std::cout << "======================================================" << std::endl;
    std::cout << " Go DLL + MSVC C++/WinRT Callback Test (Worst Case)" << std::endl;
    std::cout << "======================================================" << std::endl;
    std::cout << std::endl;

    // Init COM apartment ONCE for the entire process
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    std::cout << "[Setup] WinRT apartment initialized (MTA)" << std::endl;
    std::cout << std::endl;

    // ---- Test 1: Basic Go functions ----
    std::cout << "--- Test 1: Basic Go functions ---" << std::endl;
    int r = Add(10, 20);
    std::cout << "Add(10, 20) = " << r << std::endl;
    char* g = Greet("WinRT");
    std::cout << "Greet(\"WinRT\") = " << g << std::endl;
    std::free(g);
    std::cout << std::endl;

    // ---- Test 2: Basic callback ----
    std::cout << "--- Test 2: Basic callback via Go ---" << std::endl;
    r = CallCallback(Multiply, 7, 8);
    std::cout << "CallCallback(Multiply, 7, 8) = " << r << std::endl;
    std::cout << std::endl;

    // ---- Test 3: WinRT - System Version ----
    std::cout << "--- Test 3: WinRT callback - System Version ---" << std::endl;
    char* info = CallWinRTCallback(GetWinRTVersionInfo);
    std::cout << "Result: " << info << std::endl;
    std::free(info);
    std::cout << std::endl;

    // ---- Test 4: WinRT - Power Status ----
    std::cout << "--- Test 4: WinRT callback - Power Status ---" << std::endl;
    info = CallWinRTCallback(GetWinRTPowerStatus);
    std::cout << "Result: " << info << std::endl;
    std::free(info);
    std::cout << std::endl;

    // ---- Test 5: WinRT - Network Info ----
    std::cout << "--- Test 5: WinRT callback - Network Info ---" << std::endl;
    info = CallWinRTCallback(GetWinRTNetworkInfo);
    std::cout << "Result: " << info << std::endl;
    std::free(info);
    std::cout << std::endl;

    // ---- Test 6: WinRT - AppData (IPropertySet) ----
    std::cout << "--- Test 6: WinRT callback - AppData (IPropertySet) ---" << std::endl;
    info = CallWinRTCallback(GetWinRTAppDataInfo);
    std::cout << "Result: " << info << std::endl;
    std::free(info);
    std::cout << std::endl;

    // ---- Test 7: WinRT - HSTRING Stress ----
    std::cout << "--- Test 7: WinRT callback - HSTRING Stress ---" << std::endl;
    info = CallWinRTCallback(GetWinRTStringStress);
    std::cout << "Result: " << info << std::endl;
    std::free(info);
    std::cout << std::endl;

    // ---- Test 8: WinRT - Go -> C++/WinRT string ----
    std::cout << "--- Test 8: WinRT void callback - Go -> C++/WinRT ---" << std::endl;
    CallWinRTVoidCallback(WinRTProcessMessage);
    std::cout << std::endl;

    // Cleanup
    winrt::uninit_apartment();

    std::cout << "======================================================" << std::endl;
    std::cout << " ALL TESTS COMPLETED! " << std::endl;
    std::cout << "======================================================" << std::endl;
    return 0;
}
