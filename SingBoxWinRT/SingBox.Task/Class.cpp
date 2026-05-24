#include "pch.h"
#include "Class.h"
#include "Class.g.cpp"

#include <winrt/Windows.Networking.Vpn.h>
#include <winrt/Windows.Networking.Sockets.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>

// Global callbacks from Go (purego will bind to these exports)
extern "C" {
    typedef uintptr_t (*OnEncapsulateCallback)(const uint8_t* data, size_t size);
    __declspec(dllexport) void VpnBridge_RegisterPlugin(OnEncapsulateCallback cb);
    __declspec(dllexport) void VpnBridge_InitCOM();
    __declspec(dllexport) uintptr_t VpnChannel_InjectPacket(const uint8_t* data, size_t size);
}

typedef char* (*LibboxStart_t)(const char*, const char*);
typedef char* (*LibboxStop_t)();

static OnEncapsulateCallback g_onEncapsulate = nullptr;
static winrt::Windows::Networking::Vpn::VpnChannel g_channel{ nullptr };

extern "C" __declspec(dllexport) void VpnBridge_RegisterPlugin(OnEncapsulateCallback cb) {
    g_onEncapsulate = cb;
}

extern "C" __declspec(dllexport) void VpnBridge_InitCOM() {
    winrt::init_apartment();
}

extern "C" __declspec(dllexport) uintptr_t VpnChannel_InjectPacket(const uint8_t* data, size_t size) {
    if (!g_channel) return 0;
    try {
        auto outBuffer = g_channel.GetVpnReceivePacketBuffer();
        auto outBuf = outBuffer.Buffer();
        if (memcpy_s(outBuf.data(), outBuf.Capacity(), data, size) == 0) {
            outBuf.Length(static_cast<uint32_t>(size));
            g_channel.AppendVpnReceivePacketBuffer(outBuffer);
            g_channel.FlushVpnReceivePacketBuffers();
            return 1;
        }
    } catch (...) {}
    return 0;
}

namespace winrt::SingBox_Task::implementation
{
    struct VpnPlugin : winrt::implements<VpnPlugin, winrt::Windows::Networking::Vpn::IVpnPlugIn>
    {
        HMODULE hLibbox = nullptr;
        LibboxStart_t pLibboxStart = nullptr;
        LibboxStop_t pLibboxStop = nullptr;

        void Connect(winrt::Windows::Networking::Vpn::VpnChannel const& channel)
        {
            g_channel = channel;
            try {
                using namespace winrt::Windows::Networking;
                using namespace winrt::Windows::Networking::Sockets;
                using namespace winrt::Windows::Networking::Vpn;

                DatagramSocket transport{};
                channel.AssociateTransport(transport, nullptr);
                const auto localhost = HostName{ L"127.0.0.1" };
                transport.BindEndpointAsync(localhost, L"").get();
                transport.ConnectAsync(localhost, transport.Information().LocalPort()).get();

                VpnRouteAssignment routeScope{};
                routeScope.ExcludeLocalSubnets(true);
                routeScope.Ipv4InclusionRoutes(std::vector{
                    VpnRoute(HostName{ L"0.0.0.0" }, 1),
                    VpnRoute(HostName{ L"128.0.0.0" }, 1)
                });

                VpnDomainNameAssignment dnsAssignment{};

                // Start libbox BEFORE starting the channel transport, because libbox might need to register the callback
                if (LoadLibbox()) {
                    auto folder = Windows::Storage::ApplicationData::Current().LocalFolder();
                    std::string folderPath = winrt::to_string(folder.Path());
                    
                    std::string config = R"({"log":{"level":"info"},"inbounds":[{"type":"tun","tag":"tun-in","interface_name":"singtun","inet4_address":"172.19.0.1/30","auto_route":true,"strict_route":false}],"outbounds":[{"type":"direct","tag":"direct"}],"experimental":{"clash_api":{"external_controller":"127.0.0.1:9090","external_ui":"ui"}}})";
                    char* err = pLibboxStart(config.c_str(), folderPath.c_str());
                    if (err) {
                        channel.TerminateConnection(winrt::to_hstring(err));
                        return;
                    }
                } else {
                    channel.TerminateConnection(L"Failed to load libbox.dll");
                    return;
                }

                channel.StartWithMainTransport(
                    std::vector{ HostName{ L"192.168.3.1" } },
                    std::vector<HostName>{},
                    nullptr,
                    routeScope,
                    dnsAssignment,
                    1500,
                    1512,
                    false,
                    transport
                );
            } catch (std::exception const& ex) {
                channel.TerminateConnection(winrt::to_hstring(ex.what()));
            } catch (winrt::hresult_error const& ex) {
                channel.TerminateConnection(ex.message());
            } catch (...) {
                channel.TerminateConnection(L"Unknown error in Connect");
            }
        }

        void Disconnect(winrt::Windows::Networking::Vpn::VpnChannel const& channel)
        {
            try {
                channel.Stop();
            } catch (...) {}
            g_channel = nullptr;

            if (pLibboxStop) {
                pLibboxStop();
            }
        }

        void GetKeepAlivePayload(winrt::Windows::Networking::Vpn::VpnChannel const&, winrt::Windows::Networking::Vpn::VpnPacketBuffer&)
        {
        }

        void Encapsulate(winrt::Windows::Networking::Vpn::VpnChannel const& channel, winrt::Windows::Networking::Vpn::VpnPacketBufferList const& packets, winrt::Windows::Networking::Vpn::VpnPacketBufferList const&)
        {
            if (!g_onEncapsulate) {
                uint32_t packetCount = packets.Size();
                while (packetCount-- > 0) {
                    packets.Append(packets.RemoveAtBegin());
                }
                return;
            }

            uint32_t packetCount = packets.Size();
            while (packetCount-- > 0) {
                auto packet = packets.RemoveAtBegin();
                auto buf = packet.Buffer();
                g_onEncapsulate(buf.data(), buf.Length());
                packets.Append(packet);
            }
        }

        void Decapsulate(winrt::Windows::Networking::Vpn::VpnChannel const&, winrt::Windows::Networking::Vpn::VpnPacketBuffer const&, winrt::Windows::Networking::Vpn::VpnPacketBufferList const&, winrt::Windows::Networking::Vpn::VpnPacketBufferList const&)
        {
        }

    private:
        bool LoadLibbox() {
            if (pLibboxStart) return true;
            hLibbox = LoadPackagedLibrary(L"libbox.dll", 0);
            if (!hLibbox) return false;
            pLibboxStart = (LibboxStart_t)GetProcAddress(hLibbox, "LibboxStart");
            pLibboxStop = (LibboxStop_t)GetProcAddress(hLibbox, "LibboxStop");
            return pLibboxStart != nullptr;
        }
    };

    void Class::Run(Windows::ApplicationModel::Background::IBackgroundTaskInstance const& taskInstance)
    {
        auto plugin = winrt::make<VpnPlugin>();
        Windows::Networking::Vpn::VpnChannel::ProcessEventAsync(plugin, taskInstance.TriggerDetails());
    }
}
