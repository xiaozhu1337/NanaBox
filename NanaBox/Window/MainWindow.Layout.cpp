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

void NanaBox::MainWindow::OnTimer(
    UINT_PTR nIDEvent)
{
    if (nIDEvent == NanaBox::MainWindowTimerEvents::SyncDisplaySettings)
    {
        if (this->m_RdpClientMode != RdpClientMode::EnhancedVideoSyncedSession)
        {
            return;
        }

        UINT64 CurrentCheckPoint = ::GetTickCount64();
        if (this->m_SyncDisplaySettingsCheckPoint > CurrentCheckPoint)
        {
            return;
        }
        else
        {
            this->m_SyncDisplaySettingsCheckPoint = CurrentCheckPoint + 200;
        }

        CSize DisplayResolution = this->m_RecommendedDisplayResolution;
        if (this->m_DisplayResolution == DisplayResolution)
        {
            return;
        }

        try
        {
            this->m_RdpClient->UpdateSessionDisplaySettings(
                DisplayResolution.cx,
                DisplayResolution.cy,
                DisplayResolution.cx,
                DisplayResolution.cy,
                0,
                this->m_RecommendedZoomLevel,
                100);

            this->m_DisplayResolution = DisplayResolution;
        }
        catch (...)
        {

        }
    }
}

void NanaBox::MainWindow::OnSize(
    UINT nType,
    CSize size)
{
    UNREFERENCED_PARAMETER(nType);
    UNREFERENCED_PARAMETER(size);

    if (nType == SIZE_MINIMIZED)
    {
        return;
    }

    winrt::DesktopWindowXamlSource XamlSource = nullptr;
    winrt::copy_from_abi(
        XamlSource,
        ::GetPropW(this->m_hWnd, L"XamlWindowSource"));
    if (!XamlSource)
    {
        return;
    }

    UINT DpiValue = ::GetDpiForWindow(this->m_hWnd);

    this->m_RecommendedZoomLevel = ::MulDiv(
        100,
        DpiValue,
        USER_DEFAULT_SCREEN_DPI);

    int MainWindowControlScaledHeight = ::MulDiv(
        this->m_RecommendedMainWindowControlHeight,
        DpiValue,
        USER_DEFAULT_SCREEN_DPI);

    MARGINS Margins = {};
    Margins.cyTopHeight = MainWindowControlScaledHeight;
    ::DwmExtendFrameIntoClientArea(this->m_hWnd, &Margins);

    RECT RdpClientRect;
    winrt::check_bool(this->GetClientRect(&RdpClientRect));
    RdpClientRect.top += MainWindowControlScaledHeight;

    this->m_RdpClientWindow.SetWindowPos(
        nullptr,
        &RdpClientRect,
        SWP_NOZORDER | SWP_NOACTIVATE);

    winrt::com_ptr<IDesktopWindowXamlSourceNative> XamlSourceNative =
        XamlSource.as<IDesktopWindowXamlSourceNative>();

    HWND XamlWindowHandle = nullptr;
    winrt::check_hresult(
        XamlSourceNative->get_WindowHandle(&XamlWindowHandle));
    ::SetWindowPos(
        XamlWindowHandle,
        nullptr,
        0,
        0,
        RdpClientRect.right - RdpClientRect.left,
        MainWindowControlScaledHeight,
        SWP_SHOWWINDOW);

    this->m_RecommendedDisplayResolution = CSize(
        RdpClientRect.right - RdpClientRect.left,
        RdpClientRect.bottom - RdpClientRect.top);

    if (this->m_RdpClient->Connected() &&
        RdpClientMode::BasicSession == this->m_RdpClientMode)
    {
        VARIANT RawZoomLevel;
        RawZoomLevel.vt = VT_UI4;
        RawZoomLevel.uintVal =
            this->m_Configuration.VideoMonitor.DisableBasicSessionDpiScaling
            ? 100
            : this->m_RecommendedZoomLevel;
        this->m_RdpClient->Property(
            L"ZoomLevel",
            RawZoomLevel);
    }
}

void NanaBox::MainWindow::OnSetFocus(
    ATL::CWindow wndOld)
{
    UNREFERENCED_PARAMETER(wndOld);

    this->m_RdpClientWindow.SetFocus();
}

void NanaBox::MainWindow::OnActivate(
    UINT nState,
    BOOL bMinimized,
    ATL::CWindow wndOther)
{
    UNREFERENCED_PARAMETER(wndOther);

    if (bMinimized || nState == WA_INACTIVE)
    {
        return;
    }

    this->m_RdpClientWindow.SetFocus();
}

void NanaBox::MainWindow::OnClose()
{
    if (this->m_SnapshotOperation.load()) return;
    if (!this->m_VirtualMachineRunning)
    {
        this->DestroyWindow();
        return;
    }

    HWND WindowHandle = ::CreateXamlDialog(this->m_hWnd);
    if (!WindowHandle)
    {
        return;
    }

    winrt::NanaBox::ExitConfirmationPage Window =
        winrt::make<winrt::NanaBox::implementation::ExitConfirmationPage>(
            WindowHandle);
    ::ShowXamlDialog(
        WindowHandle,
        480,
        320,
        winrt::get_abi(Window),
        this->m_hWnd);

    switch (Window.Status())
    {
    case winrt::NanaBox::ExitConfirmationStatus::Suspend:
    {
        // Temporarily disable the GPU-PV settings because virtual machine
        // doen't support save the state when GPU-PV is enabled.
        NanaBox::GpuConfiguration Gpu;
        Gpu.AssignmentMode = NanaBox::GpuAssignmentMode::Disabled;
        NanaBox::ComputeSystemUpdateGpu(this->m_VirtualMachine, Gpu);

        this->m_VirtualMachine->Pause();

        if (this->m_Configuration.SaveStateFile.empty())
        {
            this->m_Configuration.SaveStateFile =
                this->m_Configuration.Name + ".SaveState.vmrs";
        }

        std::wstring SaveStateFile = ::GetAbsolutePath(Mile::ToWideString(
            CP_UTF8, this->m_Configuration.SaveStateFile));

        ::MileDeleteFileIgnoreReadonlyAttribute(SaveStateFile.c_str());

        nlohmann::json Options;
        Options["SaveType"] = "ToFile";
        Options["SaveStateFilePath"] = winrt::to_string(SaveStateFile.c_str());

        try
        {
            this->m_VirtualMachine->Save(winrt::to_hstring(Options.dump()));
        }
        catch (winrt::hresult_error const& ex)
        {
            ::MileDeleteFileIgnoreReadonlyAttribute(SaveStateFile.c_str());

            this->m_Configuration.SaveStateFile.clear();

            ::ShowErrorMessageDialog(ex);
        }

        if (this->m_Configuration.SaveStateFile.empty())
        {
            this->m_VirtualMachine->Resume();

            NanaBox::ComputeSystemUpdateGpu(
                this->m_VirtualMachine,
                this->m_Configuration.Gpu);
        }
        else
        {
            this->m_VirtualMachine->Terminate();

            std::string ConfigurationFileContent =
                NanaBox::SerializeConfiguration(this->m_Configuration);
            ::WriteAllTextToUtf8TextFile(
                this->m_ConfigurationFilePath,
                ConfigurationFileContent);
        }

        break;
    }
    case winrt::NanaBox::ExitConfirmationStatus::PowerOff:
    {
        try
        {
            this->m_VirtualMachine->Pause();
        }
        catch (...)
        {

        }
        this->m_VirtualMachine->Terminate();

        break;
    }
    default:
        break;
    }
}

void NanaBox::MainWindow::OnDestroy()
{
    // Just kill the process directly for reducing the Windows error reports.
    ::TerminateProcess(::GetCurrentProcess(), 0);
}

BOOL NanaBox::MainWindow::OnQueryEndSession(
    UINT nSource,
    UINT uLogOff)
{
    UNREFERENCED_PARAMETER(nSource);
    UNREFERENCED_PARAMETER(uLogOff);

    ::ShutdownBlockReasonCreate(
        this->m_hWnd,
        Mile::WinRT::GetLocalizedString(
            L"ExitConfirmationPage/GridTitleTextBlock/Text").c_str());

    // Notify the OS to prevent shut down because shut down automatically
    // without exiting NanaBox may corrupt the user's virtual machine.
    return FALSE;
}
