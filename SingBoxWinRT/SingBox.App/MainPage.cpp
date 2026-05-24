#include "pch.h"
#include "MainPage.h"
#include "MainPage.g.cpp"

#include <winrt/Windows.Networking.Vpn.h>
#include <winrt/Windows.ApplicationModel.h>

using namespace winrt;
using namespace Windows::UI::Xaml;
using namespace Windows::Networking::Vpn;

namespace winrt::SingBox_App::implementation
{
    int32_t MainPage::MyProperty()
    {
        throw hresult_not_implemented();
    }

    void MainPage::MyProperty(int32_t /* value */)
    {
        throw hresult_not_implemented();
    }
    
    winrt::Windows::Foundation::IAsyncAction MainPage::OnNavigatedTo(winrt::Windows::UI::Xaml::Navigation::NavigationEventArgs const& /*e*/)
    {
        VpnManagementAgent agent;
        auto profiles = co_await agent.GetProfilesAsync();
        VpnPlugInProfile profile = nullptr;
        
        for (auto p : profiles) {
            if (p.ProfileName() == L"SingBoxWinRT") {
                profile = p.try_as<VpnPlugInProfile>();
                break;
            }
        }

        if (!profile) {
            profile = VpnPlugInProfile();
            profile.ProfileName(L"SingBoxWinRT");
            profile.VpnPluginPackageFamilyName(Windows::ApplicationModel::Package::Current().Id().FamilyName());
            profile.ServerUris().Append(Windows::Foundation::Uri(L"http://127.0.0.1"));
            profile.CustomConfiguration(L"");
            
            auto addStatus = co_await agent.AddProfileFromObjectAsync(profile);
            if (addStatus != VpnManagementErrorStatus::Ok) {
                // If it fails on Win11, user has to add it manually in settings.
                co_return;
            }
        }
        
        // Optionally connect automatically
        // co_await agent.ConnectProfileAsync(profile);
    }
}
