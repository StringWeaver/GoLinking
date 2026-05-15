#include "pch.h"
#include <string>
#include <stdio.h>

using namespace winrt;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::UI::Core;
using namespace Windows::Networking::Vpn;
using namespace Windows::System;

typedef char* (*LibboxStart_t)(const char*, const char*);
typedef char* (*LibboxStop_t)();

struct App : implements<App, IFrameworkViewSource, IFrameworkView>
{
    HMODULE hLibbox = nullptr;
    LibboxStart_t pLibboxStart = nullptr;
    LibboxStop_t pLibboxStop = nullptr;
    bool vpnTriggered = false;

    void Log(const std::string& msg) {
        try {
            auto folder = Windows::Storage::ApplicationData::Current().LocalFolder();
            std::wstring path = std::wstring(folder.Path()) + L"\\debug.log";
            FILE* f = _wfopen(path.c_str(), L"a");
            if (f) {
                fprintf(f, "%s\n", msg.c_str());
                fclose(f);
            }
        } catch(...) {}
    }

    IFrameworkView CreateView()
    {
        return *this;
    }

    void Initialize(CoreApplicationView const&)
    {
    }

    void Load(hstring const&)
    {
    }

    void Uninitialize()
    {
    }

    void Run()
    {
        CoreWindow window = CoreWindow::GetForCurrentThread();
        window.Activate();

        CoreDispatcher dispatcher = window.Dispatcher();
        dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessUntilQuit);
    }

    void SetWindow(CoreWindow const& window)
    {
        Log("SetWindow called.");
        if (LoadLibbox()) {
            Log("LoadLibbox returned true.");
            auto folder = Windows::Storage::ApplicationData::Current().LocalFolder();
            std::string folderPath = winrt::to_string(folder.Path());
            
            std::wstring stderrPath = std::wstring(folder.Path()) + L"\\stderr.log";
            _wfreopen(stderrPath.c_str(), L"w", stderr);
            _wfreopen(stderrPath.c_str(), L"w", stdout);

            std::string config = R"({"log":{"level":"info"},"inbounds":[{"type":"tun","tag":"tun-in","interface_name":"singtun","inet4_address":"172.19.0.1/30","auto_route":true,"strict_route":false}],"outbounds":[{"type":"direct","tag":"direct"}]})";
            
            Log("Calling pLibboxStart...");
            char* err = pLibboxStart(config.c_str(), folderPath.c_str());
            if (!err) {
                Log("pLibboxStart success. Connecting VPN...");
                ConnectVpnAsync();
                Log("Launching URI...");
                Launcher::LaunchUriAsync(Windows::Foundation::Uri(L"http://127.0.0.1:9090"));
            } else {
                Log(std::string("pLibboxStart error: ") + err);
            }
        } else {
            Log("LoadLibbox returned false. Cannot load libbox.dll!");
            DWORD err = GetLastError();
            char errStr[256];
            sprintf_s(errStr, "GetLastError: %lu", err);
            Log(errStr);
        }

        window.Closed({ this, &App::OnClosed });
    }

    bool LoadLibbox() {
        if (pLibboxStart) return true;
        hLibbox = LoadPackagedLibrary(L"libbox.dll", 0);
        if (!hLibbox) return false;
        pLibboxStart = (LibboxStart_t)GetProcAddress(hLibbox, "LibboxStart");
        pLibboxStop = (LibboxStop_t)GetProcAddress(hLibbox, "LibboxStop");
        return pLibboxStart != nullptr;
    }

    winrt::Windows::Foundation::IAsyncAction ConnectVpnAsync() {
        VpnManagementAgent agent;
        auto profiles = co_await agent.GetProfilesAsync();
        VpnPlugInProfile profile = nullptr;
        for (auto p : profiles) {
            if (p.ProfileName() == L"SingTun") {
                profile = p.try_as<VpnPlugInProfile>();
                break;
            }
        }

        if (!profile) {
            profile = VpnPlugInProfile();
            profile.ProfileName(L"SingTun");
            profile.VpnPluginPackageFamilyName(Windows::ApplicationModel::Package::Current().Id().FamilyName());
            profile.ServerUris().Append(Windows::Foundation::Uri(L"http://127.0.0.1"));
            profile.CustomConfiguration(L"");
            co_await agent.AddProfileFromObjectAsync(profile);
        }

        auto status = co_await agent.ConnectProfileAsync(profile);
        if (status == VpnManagementErrorStatus::Ok) {
            vpnTriggered = true;
        }
    }

    winrt::Windows::Foundation::IAsyncAction DisconnectVpnAsync() {
        VpnManagementAgent agent;
        auto profiles = co_await agent.GetProfilesAsync();
        for (auto p : profiles) {
            if (p.ProfileName() == L"SingTun") {
                co_await agent.DisconnectProfileAsync(p);
                break;
            }
        }
    }

    void OnClosed(CoreWindow const&, CoreWindowEventArgs const&)
    {
        if (pLibboxStop) pLibboxStop();
        if (vpnTriggered) {
            DisconnectVpnAsync();
            vpnTriggered = false;
        }
    }
};

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    CoreApplication::Run(make<App>());
}