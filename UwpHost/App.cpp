#include "pch.h"
#include <string>

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
        if (LoadLibbox()) {
            auto folder = Windows::Storage::ApplicationData::Current().LocalFolder();
            std::string folderPath = winrt::to_string(folder.Path());
            
            std::string config = R"({"log":{"level":"info"},"inbounds":[{"type":"tun","tag":"tun-in","interface_name":"singtun","inet4_address":"172.19.0.1/30","auto_route":true,"strict_route":false}],"outbounds":[{"type":"direct","tag":"direct"}]})";
            
            char* err = pLibboxStart(config.c_str(), folderPath.c_str());
            if (!err) {
                ConnectVpnAsync();
                Launcher::LaunchUriAsync(Windows::Foundation::Uri(L"http://127.0.0.1:9090"));
            }
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