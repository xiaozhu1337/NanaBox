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

void NanaBox::MainWindow::RdpClientOnRemoteDesktopSizeChange(
    _In_ LONG Width,
    _In_ LONG Height)
{
    if (this->m_RdpClientMode == RdpClientMode::BasicSession)
    {
        this->m_DisplayResolution = CSize(Width, Height);

        if (this->IsZoomed())
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
            return;
        }

        UINT DpiValue = ::GetDpiForWindow(this->m_hWnd);

        RECT WindowRect;
        winrt::check_bool(this->GetWindowRect(&WindowRect));
        {
            RECT ClientRect;
            winrt::check_bool(this->GetClientRect(&ClientRect));

            // Reserve space for the non-client area.

            WindowRect.right -= ClientRect.right;
            WindowRect.bottom -= ClientRect.bottom;

            // Reserve space for the XAML toolbar window.

            WindowRect.bottom += ::MulDiv(
                this->m_RecommendedMainWindowControlHeight,
                DpiValue,
                USER_DEFAULT_SCREEN_DPI);

            // Reserve space for the RDP client window.

            if (this->m_Configuration.VideoMonitor.DisableBasicSessionDpiScaling)
            {
                WindowRect.right += this->m_DisplayResolution.cx;
                WindowRect.bottom += this->m_DisplayResolution.cy;
            }
            else
            {
                WindowRect.right += ::MulDiv(
                    this->m_DisplayResolution.cx,
                    DpiValue,
                    USER_DEFAULT_SCREEN_DPI);
                WindowRect.bottom += ::MulDiv(
                    this->m_DisplayResolution.cy,
                    DpiValue,
                    USER_DEFAULT_SCREEN_DPI);
            }
        }

        this->SetWindowPos(
            nullptr,
            &WindowRect,
            SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void NanaBox::MainWindow::RdpClientOnLoginComplete()
{
    this->m_VirtualMachineNeverConnected = true;
    if (this->m_RdpClientMode == RdpClientMode::EnhancedSession)
    {
        this->m_RdpClientMode = RdpClientMode::EnhancedVideoSyncedSession;
        this->m_DisplayResolution = CSize();
    }
}

void NanaBox::MainWindow::RdpClientOnDisconnected(
    _In_ LONG DisconnectReason)
{
    this->m_MouseCaptureMode = false;
    ::ClipCursor(nullptr);

    if (this->m_RdpNamedPipe) this->m_RdpNamedPipe->TerminateInstance();
    this->m_RdpNamedPipe = nullptr;

    if (this->m_SnapshotOperation.load()) return;

    if (this->m_RdpClientMode == RdpClientMode::EnhancedVideoSyncedSession)
    {
        this->m_RdpClientMode = RdpClientMode::EnhancedSession;
    }

    if (this->m_VirtualMachineRunning)
    {
        // I have not found the actual meaning of DisconnectReason even in the
        // reverse engineering view of mstscax.dll. The only thing I know is that
        // it will return the internal error description string when using the
        // GetErrorDescription method of the RDP client.
        if (DisconnectReason == 4 && !this->m_VirtualMachineNeverConnected)
        {
            if (!this->m_VmmsCertificateCreationAttempted)
            {
                this->m_VmmsCertificateCreationAttempted = true;
                if (::CreateVmmsCertificate())
                {
                    try
                    {
                        this->RdpClientConnect();
                        return;
                    }
                    catch (...)
                    {

                    }
                }
            }

            ::ShowMessageDialog(
                nullptr,
                Mile::WinRT::GetLocalizedString(
                    L"Messages/VirtualMachineConnectionFailedInstructionText"),
                Mile::WinRT::GetLocalizedString(
                    L"Messages/VirtualMachineConnectionFailedContentText"));

            this->m_VirtualMachine->Terminate();
        }
        else if (this->m_NeedRdpClientModeChange)
        {
            try
            {
                this->RdpClientUninitialize();
                this->RdpClientInitialize();

                // Ensure the UI layout will be refreshed via sending WM_SIZE
                // message directly. It's OK because we don't use wParam and
                // lParam in WM_SIZE message.
                this->SendMessageW(WM_SIZE);
            }
            catch (...)
            {

            }

            this->m_NeedRdpClientModeChange = false;
        }
        else
        {
            try
            {
                this->RdpClientConnect();
            }
            catch (...)
            {

            }
        }
    }
}

void NanaBox::MainWindow::RdpClientOnRequestGoFullScreen()
{
    HMONITOR MonitorHandle = ::MonitorFromWindow(
        this->m_hWnd,
        MONITOR_DEFAULTTONULL);
    winrt::check_pointer(MonitorHandle);
    MONITORINFOEX MonitorInfo;
    MonitorInfo.cbSize = sizeof(MONITORINFOEX);
    winrt::check_bool(::GetMonitorInfoW(
        MonitorHandle,
        &MonitorInfo));
    DEVMODE DevMode;
    DevMode.dmSize = sizeof(DEVMODE);
    winrt::check_bool(::EnumDisplaySettingsW(
        MonitorInfo.szDevice,
        ENUM_CURRENT_SETTINGS,
        &DevMode));

    winrt::check_bool(this->GetWindowRect(
        &this->m_RememberedMainWindowRect));
    this->m_RememberedMainWindowStyle =
        this->GetWindowLongPtrW(GWL_STYLE);

    RECT FullScreenRect;
    FullScreenRect.left = 0;
    FullScreenRect.top = 0;
    FullScreenRect.right = DevMode.dmPelsWidth;
    FullScreenRect.bottom = DevMode.dmPelsHeight;

    // Resize RDP client window and hide the XAML control window by calculating
    // in OnSize method automatically.
    this->m_RecommendedMainWindowControlHeight = 0;

    this->SetWindowLongPtrW(
        GWL_STYLE,
        WS_VISIBLE | WS_POPUP);
    // SetWindowPos will send WM_SIZE, RDP Zooming will be re-enabled in Basic
    // Session Mode.
    this->SetWindowPos(
        HWND_TOP,
        &FullScreenRect,
        SWP_FRAMECHANGED | SWP_NOACTIVATE);
}

void NanaBox::MainWindow::RdpClientOnRequestLeaveFullScreen()
{
    this->m_RecommendedMainWindowControlHeight =
        this->m_MainWindowControlHeight;
    this->SetWindowLongPtrW(
        GWL_STYLE,
        this->m_RememberedMainWindowStyle);
    this->SetWindowPos(
        nullptr,
        &this->m_RememberedMainWindowRect,
        SWP_NOACTIVATE);
}

void NanaBox::MainWindow::RdpClientOnRequestContainerMinimize()
{
    this->ShowWindow(SW_MINIMIZE);
}

void NanaBox::MainWindow::RdpClientOnConfirmClose(
    _Out_ VARIANT_BOOL* pfAllowClose)
{
    // Set it FALSE because we don't want to close the connection before making
    // decisions in the close confirmation dialog.
    *pfAllowClose = VARIANT_FALSE;
    this->PostMessageW(WM_CLOSE);
}

void NanaBox::MainWindow::RdpClientOnFocusReleased(
    _In_ INT Direction)
{
    UNREFERENCED_PARAMETER(Direction);

    this->m_MouseCaptureMode = !this->m_MouseCaptureMode;

    if (this->m_MouseCaptureMode)
    {
        RECT WindowRect;
        if (this->m_RdpClientWindow.GetWindowRect(&WindowRect))
        {
            ::ClipCursor(&WindowRect);
        }
    }
    else
    {
        ::ClipCursor(nullptr);
    }
}
