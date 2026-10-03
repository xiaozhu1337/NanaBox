#include "pch.h"

#include "MainWindow.h"
#include "ExitConfirmationPage.h"
#include "ReloadConfirmationPage.h"

#include <Mile.Xaml.h>

#include <ShlObj.h>

#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>

#include "Utils.h"

#include <map>

#include "NanaBoxResources.h"

#include "VmmsCertificate.h"

namespace winrt
{
    using Windows::UI::Xaml::Hosting::DesktopWindowXamlSource;
}

void NanaBox::MainWindow::InitializeVirtualMachine()
{
    this->m_VirtualMachinePaused = false;
    std::string ConfigurationFileContent = ::ReadAllTextFromUtf8TextFile(
        this->m_ConfigurationFilePath);

    this->m_Configuration = NanaBox::DeserializeConfiguration(
        ConfigurationFileContent);

    winrt::hstring HcsVmId = winrt::to_hstring(this->m_Configuration.Name);

    {
        bool VirtualMachineExisted = true;
        try
        {
            winrt::make_self<NanaBox::ComputeSystem>(HcsVmId);
        }
        catch (...)
        {
            VirtualMachineExisted = false;
        }

        if (VirtualMachineExisted)
        {
            winrt::throw_hresult(HCS_E_SYSTEM_ALREADY_EXISTS);
        }
    }

    // Recovery may change disk paths; never apply it while another HCS VM exists.
    NanaBox::SnapshotStore(this->m_ConfigurationFilePath).Recover();
    ConfigurationFileContent = ::ReadAllTextFromUtf8TextFile(this->m_ConfigurationFilePath);
    this->m_Configuration = NanaBox::DeserializeConfiguration(ConfigurationFileContent);

    for (NanaBox::ScsiDeviceConfiguration& ScsiDevice
        : this->m_Configuration.ScsiDevices)
    {
        NanaBox::ValidateScsiDeviceConfiguration(HcsVmId, ScsiDevice);
    }

    if (this->m_Configuration.GuestStateFile.empty())
    {
        this->m_Configuration.GuestStateFile =
            this->m_Configuration.Name + ".vmgs";
    }
    {
        std::wstring GuestStateFile = ::GetAbsolutePath(Mile::ToWideString(
            CP_UTF8, this->m_Configuration.GuestStateFile));
        if (!::PathFileExistsW(GuestStateFile.c_str()))
        {
            winrt::check_hresult(::HcsCreateEmptyGuestStateFile(
                GuestStateFile.c_str()));
        }

        winrt::check_hresult(::HcsGrantVmAccess(
            HcsVmId.c_str(),
            GuestStateFile.c_str()));
    }

    if (this->m_Configuration.RuntimeStateFile.empty())
    {
        this->m_Configuration.RuntimeStateFile =
            this->m_Configuration.Name + ".vmrs";
    }
    {
        std::wstring RuntimeStateFile = ::GetAbsolutePath(Mile::ToWideString(
            CP_UTF8, this->m_Configuration.RuntimeStateFile));
        if (!::PathFileExistsW(RuntimeStateFile.c_str()))
        {
            winrt::check_hresult(::HcsCreateEmptyRuntimeStateFile(
                RuntimeStateFile.c_str()));
        }

        winrt::check_hresult(::HcsGrantVmAccess(
            HcsVmId.c_str(),
            RuntimeStateFile.c_str()));
    }

    if (!this->m_Configuration.SaveStateFile.empty())
    {
        std::wstring SaveStateFile = ::GetAbsolutePath(Mile::ToWideString(
            CP_UTF8, this->m_Configuration.SaveStateFile));
        if (::PathFileExistsW(SaveStateFile.c_str()))
        {
            winrt::check_hresult(::HcsGrantVmAccess(
                HcsVmId.c_str(),
                SaveStateFile.c_str()));
        }
    }

    if (!this->m_Configuration.NetworkAdapters.empty())
    {
        for (NanaBox::NetworkAdapterConfiguration& NetworkAdapter
            : this->m_Configuration.NetworkAdapters)
        {
            NanaBox::ComputeNetworkDeleteEndpoint(NetworkAdapter);
            if (NetworkAdapter.Connected)
            {
                try
                {
                    NanaBox::ComputeNetworkCreateEndpoint(
                        this->m_Configuration.Name,
                        NetworkAdapter);
                }
                catch (...)
                {

                }
            }
        }
    }

    this->m_VirtualMachine = winrt::make_self<NanaBox::ComputeSystem>(
        HcsVmId,
        winrt::to_hstring(
            NanaBox::MakeHcsConfiguration(this->m_Configuration)));

    this->m_VirtualMachine->SystemExited.add([this](
        winrt::hstring const& EventData)
    {
        UNREFERENCED_PARAMETER(EventData);

        if (this->m_SnapshotOperation.load()) return;

        if (this->m_VirtualMachineRestarting)
        {
            this->m_VirtualMachineRestarting = false;
            this->m_RdpClient->Disconnect();
            return;
        }

        this->m_VirtualMachineRunning = false;
        ::SleepEx(200, FALSE);
        this->PostMessageW(WM_CLOSE);
    });

    /*this->m_VirtualMachine->SystemRdpEnhancedModeStateChanged.add([this]()
    {
        this->m_EnableEnhancedMode = !this->m_EnableEnhancedMode;
    });*/

    this->m_VirtualMachine->Start();

    NanaBox::ComputeSystemUpdateGpu(
        this->m_VirtualMachine,
        this->m_Configuration.Gpu);

    this->m_VirtualMachineRunning = true;

    if (!this->m_Configuration.SaveStateFile.empty())
    {
        std::wstring SaveStateFile = ::GetAbsolutePath(Mile::ToWideString(
            CP_UTF8, this->m_Configuration.SaveStateFile));

        ::MileDeleteFileIgnoreReadonlyAttribute(SaveStateFile.c_str());

        this->m_Configuration.SaveStateFile.clear();
    }

    ConfigurationFileContent =
        NanaBox::SerializeConfiguration(this->m_Configuration);
    ::WriteAllTextToUtf8TextFile(
        this->m_ConfigurationFilePath,
        ConfigurationFileContent);

    nlohmann::json Properties = nlohmann::json::parse(
        winrt::to_string(this->m_VirtualMachine->GetProperties()));
    this->m_VirtualMachineGuid = Properties["RuntimeId"];

    this->m_WindowTitle = Mile::ToWideString(
        CP_UTF8,
        Mile::FormatString(
            "%s - NanaBox",
            this->m_Configuration.Name.c_str()));
    this->SetWindowTextW(this->m_WindowTitle.c_str());
}

void NanaBox::MainWindow::TryReloadVirtualMachine()
{
    std::string ConfigurationFileContent =
        ::ReadAllTextFromUtf8TextFile(this->m_ConfigurationFilePath);

    NanaBox::VirtualMachineConfiguration Configuration =
        NanaBox::DeserializeConfiguration(ConfigurationFileContent);

    if (this->m_Configuration.MemorySize != Configuration.MemorySize)
    {
        try
        {
            NanaBox::ComputeSystemUpdateMemorySize(
                this->m_VirtualMachine,
                Configuration.MemorySize);
            this->m_Configuration.MemorySize =
                Configuration.MemorySize;
        }
        catch (...)
        {

        }
    }

    if (0 != ::_stricmp(
        this->m_Configuration.ComPorts.ComPort1.c_str(),
        Configuration.ComPorts.ComPort1.c_str()))
    {
        try
        {
            if (this->m_Configuration.ComPorts.ComPort1.empty())
            {
                NanaBox::ComputeSystemAddComPort(
                    this->m_VirtualMachine,
                    0,
                    Configuration.ComPorts.ComPort1);
            }
            else if (Configuration.ComPorts.ComPort1.empty())
            {
                NanaBox::ComputeSystemRemoveComPort(
                    this->m_VirtualMachine,
                    0,
                    Configuration.ComPorts.ComPort1);
            }
            else
            {
                NanaBox::ComputeSystemUpdateComPort(
                    this->m_VirtualMachine,
                    0,
                    Configuration.ComPorts.ComPort1);
            }

            this->m_Configuration.ComPorts.ComPort1 =
                Configuration.ComPorts.ComPort1;
        }
        catch (...)
        {

        }
    }

    if (0 != ::_stricmp(
        this->m_Configuration.ComPorts.ComPort2.c_str(),
        Configuration.ComPorts.ComPort2.c_str()))
    {
        try
        {
            if (this->m_Configuration.ComPorts.ComPort2.empty())
            {
                NanaBox::ComputeSystemAddComPort(
                    this->m_VirtualMachine,
                    1,
                    Configuration.ComPorts.ComPort2);
            }
            else if (Configuration.ComPorts.ComPort2.empty())
            {
                NanaBox::ComputeSystemRemoveComPort(
                    this->m_VirtualMachine,
                    1,
                    Configuration.ComPorts.ComPort2);
            }
            else
            {
                NanaBox::ComputeSystemUpdateComPort(
                    this->m_VirtualMachine,
                    1,
                    Configuration.ComPorts.ComPort2);
            }

            this->m_Configuration.ComPorts.ComPort2 =
                Configuration.ComPorts.ComPort2;
        }
        catch (...)
        {

        }
    }

    try
    {
        NanaBox::ComputeSystemUpdateGpu(
            this->m_VirtualMachine,
            Configuration.Gpu);
        this->m_Configuration.Gpu = Configuration.Gpu;
    }
    catch (...)
    {

    }

    {
        using NetworkAdapterMapType =
            std::map<std::string, NanaBox::NetworkAdapterConfiguration>;

        NetworkAdapterMapType PreviousList;
        NetworkAdapterMapType CurrentList;
        std::vector<NanaBox::NetworkAdapterConfiguration> KeepList;
        std::vector<NanaBox::NetworkAdapterConfiguration> AddList;
        std::vector<NanaBox::NetworkAdapterConfiguration> RemoveList;
        std::vector<NanaBox::NetworkAdapterConfiguration> FinalList;

        for (NanaBox::NetworkAdapterConfiguration& Previous
            : this->m_Configuration.NetworkAdapters)
        {
            PreviousList.insert(std::pair(Previous.EndpointId, Previous));
        }

        for (NanaBox::NetworkAdapterConfiguration& Current
            : Configuration.NetworkAdapters)
        {
            CurrentList.insert(std::pair(Current.EndpointId, Current));
        }

        for (NanaBox::NetworkAdapterConfiguration& Previous
            : this->m_Configuration.NetworkAdapters)
        {
            NetworkAdapterMapType::iterator Current =
                CurrentList.find(Previous.EndpointId);

            if (CurrentList.end() == Current)
            {
                RemoveList.push_back(Previous);
            }
            else
            {
                if (Previous.Connected != Current->second.Connected ||
                    0 != ::_stricmp(
                        Previous.MacAddress.c_str(),
                        Current->second.MacAddress.c_str()))
                {
                    if (Previous.Connected)
                    {
                        RemoveList.push_back(Previous);
                    }
                    AddList.push_back(Current->second);
                }
                else
                {
                    KeepList.push_back(Previous);
                }
            }
        }

        for (NanaBox::NetworkAdapterConfiguration& Current
            : Configuration.NetworkAdapters)
        {
            if (PreviousList.end() == PreviousList.find(Current.EndpointId))
            {
                AddList.push_back(Current);
            }
        }

        for (NanaBox::NetworkAdapterConfiguration& Current : KeepList)
        {
            FinalList.push_back(Current);
        }

        for (NanaBox::NetworkAdapterConfiguration& Current : RemoveList)
        {
            try
            {
                NanaBox::ComputeSystemRemoveNetworkAdapter(
                    this->m_VirtualMachine,
                    Current);
                NanaBox::ComputeNetworkDeleteEndpoint(Current);
            }
            catch (...)
            {
                FinalList.push_back(Current);
            }
        }

        for (NanaBox::NetworkAdapterConfiguration& Current : AddList)
        {
            try
            {
                NanaBox::ComputeNetworkDeleteEndpoint(Current);
                if (Current.Connected)
                {
                    NanaBox::ComputeNetworkCreateEndpoint(
                        this->m_Configuration.Name,
                        Current);
                    NanaBox::ComputeSystemAddNetworkAdapter(
                        this->m_VirtualMachine,
                        Current);
                }
                FinalList.push_back(Current);
            }
            catch (...)
            {

            }
        }

        this->m_Configuration.NetworkAdapters = FinalList;
    }

    {
        std::size_t PreviousCount = this->m_Configuration.ScsiDevices.size();
        std::size_t NewCount = Configuration.ScsiDevices.size();
        if (PreviousCount <= NewCount)
        {
            for (std::uint32_t i = 0; i < NewCount; ++i)
            {
                NanaBox::ScsiDeviceConfiguration& Current =
                    Configuration.ScsiDevices[i];

                if (i < PreviousCount)
                {
                    NanaBox::ScsiDeviceConfiguration& Previous =
                        this->m_Configuration.ScsiDevices[i];

                    if (Previous.Type == Current.Type)
                    {
                        if (0 != ::_stricmp(
                            Previous.Path.c_str(),
                            Current.Path.c_str()))
                        {
                            try
                            {
                                NanaBox::ComputeSystemUpdateScsiDevice(
                                    this->m_VirtualMachine,
                                    i,
                                    Current);
                                Previous.Path = Current.Path;
                            }
                            catch (...)
                            {

                            }
                        }
                    }
                }
                else
                {
                    try
                    {
                        NanaBox::ValidateScsiDeviceConfiguration(
                            winrt::to_hstring(this->m_Configuration.Name),
                            Current);

                        NanaBox::ComputeSystemAddScsiDevice(
                            this->m_VirtualMachine,
                            i,
                            Current);
                        this->m_Configuration.ScsiDevices.push_back(Current);
                    }
                    catch (...)
                    {

                    }
                }
            }
        }
    }

    try
    {
        // Validate changes with a temporary client; the connected client cannot
        // apply them until reinitialized after disconnection.
        winrt::com_ptr<NanaBox::RdpClient> TemporaryRdpClient =
            winrt::make_self<NanaBox::RdpClient>();

        try
        {
            NanaBox::RemoteDesktopUpdateKeyboardConfiguration(
                TemporaryRdpClient,
                Configuration.Keyboard);
            this->m_Configuration.Keyboard =
                Configuration.Keyboard;
        }
        catch (...)
        {

        }

        try
        {
            NanaBox::RemoteDesktopUpdateEnhancedSessionConfiguration(
                TemporaryRdpClient,
                Configuration.EnhancedSession);
            this->m_Configuration.EnhancedSession =
                Configuration.EnhancedSession;
        }
        catch (...)
        {

        }
    }
    catch (...)
    {

    }

    {
        this->m_Configuration.VideoMonitor.DisableBasicSessionDpiScaling =
            Configuration.VideoMonitor.DisableBasicSessionDpiScaling;
    }

    ConfigurationFileContent =
        NanaBox::SerializeConfiguration(this->m_Configuration);
    ::WriteAllTextToUtf8TextFile(
        this->m_ConfigurationFilePath,
        ConfigurationFileContent);

    this->m_NeedRdpClientModeChange = true;
    this->m_RdpClient->Disconnect();
}
