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

NanaBox::MainWindow::MainWindow(
    std::wstring const& ConfigurationFilePath) :
    m_ConfigurationFilePath(ConfigurationFilePath)
{

}

int NanaBox::MainWindow::OnCreate(
    LPCREATESTRUCT lpCreateStruct)
{
    UNREFERENCED_PARAMETER(lpCreateStruct);

    if (this->DefWindowProcW() != 0)
    {
        return -1;
    }

    this->m_ApplicationIcon.LoadIconW(
        MAKEINTRESOURCE(IDI_NANABOX),
        256,
        256,
        LR_SHARED);

    this->SetIcon(this->m_ApplicationIcon, TRUE);
    this->SetIcon(this->m_ApplicationIcon, FALSE);

    this->m_MainWindowControl =
        winrt::make<winrt::NanaBox::implementation::MainWindowControl>(
            this->m_hWnd);

    if (FAILED(::MileXamlSetXamlContentForContentWindow(
        this->m_hWnd,
        winrt::get_abi(this->m_MainWindowControl))))
    {
        return -1;
    }

    try
    {
        this->InitializeVirtualMachine();

        winrt::check_hresult(::RDPBASE_CreateInstance(
            nullptr,
            CLSID_RDPRuntimeSTAContext,
            IID_IRDPENCPlatformContext,
            this->m_PlatformContext.put_void()));
        winrt::check_hresult(this->m_PlatformContext->InitializeInstance());
        this->m_RdpNamedPipeCallbacks.attach(
            new NanaBox::RdpNamedPipeCallbacks(this));

        this->RdpClientInitialize();
    }
    catch (...)
    {
        ::ShowErrorMessageDialog(Mile::WinRT::ToHResultError());
        return -1;
    }

    return 0;
}

void NanaBox::MainWindow::OnCommand(
    UINT uNotifyCode,
    int nID,
    ATL::CWindow wndCtl)
{
    UNREFERENCED_PARAMETER(uNotifyCode);
    UNREFERENCED_PARAMETER(wndCtl);

    if (this->m_SnapshotOperation.load()) return;

    switch (nID)
    {
    case NanaBox::MainWindowCommands::Snapshots:
    {
        this->ShowSnapshots();
        break;
    }
    case NanaBox::MainWindowCommands::EnterBasicSession:
    {
        this->m_RdpClientMode = RdpClientMode::BasicSession;
        this->m_NeedRdpClientModeChange = true;

        this->m_RdpClient->Disconnect();

        break;
    }
    case NanaBox::MainWindowCommands::EnterEnhancedSession:
    {
        this->m_RdpClientMode = RdpClientMode::EnhancedSession;
        this->m_NeedRdpClientModeChange = true;

        this->m_RdpClient->Disconnect();

        break;
    }
    case NanaBox::MainWindowCommands::EnterFullScreen:
    {
        // If ZoomLevel doesn't equal 100, Basic Session Mode can't enter full
        // screen.
        if (this->m_RdpClientMode == NanaBox::RdpClientMode::BasicSession)
        {
            this->m_RecommendedZoomLevel = 100;
            VARIANT RawZoomLevel;
            RawZoomLevel.vt = VT_UI4;
            RawZoomLevel.uintVal = this->m_RecommendedZoomLevel;
            this->m_RdpClient->Property(
                L"ZoomLevel",
                RawZoomLevel);
        }

        this->m_RdpClient->FullScreen(true);

        break;
    }
    case NanaBox::MainWindowCommands::PauseVirtualMachine:
    {
        this->m_VirtualMachine->Pause();
        this->m_VirtualMachinePaused = true;

        break;
    }
    case NanaBox::MainWindowCommands::ResumeVirtualMachine:
    {
        this->m_VirtualMachine->Resume();
        this->m_VirtualMachinePaused = false;

        break;
    }
    case NanaBox::MainWindowCommands::RestartVirtualMachine:
    {
        this->m_VirtualMachineRestarting = true;
        this->m_VirtualMachine->Terminate();
        this->m_VirtualMachine = nullptr;

        this->InitializeVirtualMachine();

        break;
    }
    case NanaBox::MainWindowCommands::VirtualMachineSettings:
    {
        winrt::handle(Mile::CreateThread([=]()
        {
            winrt::hstring SettingsInstructionText =
                Mile::WinRT::GetLocalizedString(
                    L"MainWindow/SettingsInstructionText");
            winrt::hstring SettingsContentText =
                Mile::WinRT::GetLocalizedString(
                    L"MainWindow/SettingsContentText");

            ::ShowMessageDialog(
                this->m_hWnd,
                SettingsInstructionText.c_str(),
                SettingsContentText.c_str());

            OPENASINFO OpenAsInfo = {};
            OpenAsInfo.pcszFile = this->m_ConfigurationFilePath.c_str();
            OpenAsInfo.oaifInFlags = OAIF_EXEC;
            ::SHOpenWithDialog(this->m_hWnd, &OpenAsInfo);
        }));

        break;
    }
    case NanaBox::MainWindowCommands::ReloadVirtualMachineSettings:
    {
        HWND WindowHandle = ::CreateXamlDialog(this->m_hWnd);
        if (!WindowHandle)
        {
            return;
        }

        winrt::NanaBox::ReloadConfirmationPage Window =
            winrt::make<winrt::NanaBox::implementation::ReloadConfirmationPage>(
                WindowHandle);
        ::ShowXamlDialog(
            WindowHandle,
            480,
            320,
            winrt::get_abi(Window),
            this->m_hWnd);

        if (winrt::NanaBox::ReloadConfirmationStatus::Reload == Window.Status())
        {
            try
            {
                this->TryReloadVirtualMachine();
            }
            catch (...)
            {

            }
        }

        break;
    }
    case NanaBox::MainWindowCommands::CreateVirtualHardDisk:
    {
        ::ShowNewVirtualHardDiskDialog(this->m_hWnd);

        break;
    }
    case NanaBox::MainWindowCommands::ResizeVirtualHardDisk:
    {
        ::ShowResizeVirtualHardDiskDialog(this->m_hWnd);

        break;
    }
    case NanaBox::MainWindowCommands::CompactVirtualHardDisk:
    {
        ::ShowCompactVirtualHardDiskDialog(this->m_hWnd);

        break;
    }
    case NanaBox::MainWindowCommands::Documentation:
    {
        ::LaunchDocumentation();

        break;
    }
    case NanaBox::MainWindowCommands::About:
    {
        ::ShowAboutDialog(this->m_hWnd);

        break;
    }
    default:
        break;
    }
}
