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

void NanaBox::MainWindow::RdpClientInitialize()
{
    this->m_RdpClient = winrt::make_self<NanaBox::RdpClient>();

    this->m_RdpClient->EnableEventsDispatcher();

    winrt::check_pointer(this->m_RdpClientWindow.Create(
        _T(SYSTEM_ATL_AX_WINDOW_CLASS_NAME),
        this->m_hWnd,
        this->m_RdpClientWindow.rcDefault,
        nullptr,
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN));

    winrt::check_hresult(::SystemAtlAxAttachControl(
        this->m_RdpClient->RawControl().get(),
        this->m_RdpClientWindow.m_hWnd,
        nullptr));

    this->m_RdpClient->ConnectionBarText(this->m_WindowTitle.c_str());

    this->m_RdpClient->EnableAutoReconnect(false);
    this->m_RdpClient->RelativeMouseMode(true);

    this->m_RdpClient->AuthenticationLevel(0);
    this->m_RdpClient->EnableCredSspSupport(false);
    this->m_RdpClient->NegotiateSecurityLayer(true);

    NanaBox::RemoteDesktopUpdateKeyboardConfiguration(
        this->m_RdpClient,
        this->m_Configuration.Keyboard);

    NanaBox::RemoteDesktopUpdateEnhancedSessionConfiguration(
        this->m_RdpClient,
        this->m_Configuration.EnhancedSession);

    this->m_RdpClient->ContainerHandledFullScreen(true);

    try
    {
        VARIANT Value;
        Value.vt = VT_BOOL;
        Value.boolVal = VARIANT_TRUE;
        this->m_RdpClient->Property(L"AllowAxToContainerEvents", Value);
    }
    catch (...)
    {

    }

    this->m_RdpClient->PerformanceFlags(
        TS_PERF_ENABLE_ENHANCED_GRAPHICS |
        TS_PERF_ENABLE_FONT_SMOOTHING |
        TS_PERF_ENABLE_DESKTOP_COMPOSITION);

    try
    {
        VARIANT Value;
        Value.vt = VT_BOOL;
        Value.boolVal = VARIANT_TRUE;
        this->m_RdpClient->Property(L"EnableHardwareMode", Value);
    }
    catch (...)
    {

    }

    try
    {
        VARIANT Value;
        Value.vt = VT_BOOL;
        Value.boolVal = VARIANT_TRUE;
        this->m_RdpClient->Property(L"EnableFrameBufferRedirection", Value);
    }
    catch (...)
    {

    }

    try
    {
        VARIANT Value;
        Value.vt = VT_BOOL;
        Value.boolVal = VARIANT_TRUE;
        this->m_RdpClient->Property(L"AllowRelativeMouseMode", Value);
    }
    catch (...)
    {

    }

    this->m_RdpClient->GrabFocusOnConnect(false);

    this->m_RdpClient->AllowPromptingForCredentials(false);

    this->m_RdpClient->OnRemoteDesktopSizeChange.add(
        { this, &NanaBox::MainWindow::RdpClientOnRemoteDesktopSizeChange });
    this->m_RdpClient->OnLoginComplete.add(
        { this, &NanaBox::MainWindow::RdpClientOnLoginComplete });
    this->m_RdpClient->OnDisconnected.add(
        { this, &NanaBox::MainWindow::RdpClientOnDisconnected });
    this->m_RdpClient->OnRequestGoFullScreen.add(
        { this, &NanaBox::MainWindow::RdpClientOnRequestGoFullScreen });
    this->m_RdpClient->OnRequestLeaveFullScreen.add(
        { this, &NanaBox::MainWindow::RdpClientOnRequestLeaveFullScreen });
    this->m_RdpClient->OnRequestContainerMinimize.add(
        { this, &NanaBox::MainWindow::RdpClientOnRequestContainerMinimize });
    this->m_RdpClient->OnConfirmClose.add(
        { this, &NanaBox::MainWindow::RdpClientOnConfirmClose });
    this->m_RdpClient->OnFocusReleased.add(
        { this, &NanaBox::MainWindow::RdpClientOnFocusReleased });

    this->SetTimer(
        NanaBox::MainWindowTimerEvents::SyncDisplaySettings,
        200);

    this->m_RdpClient->MinInputSendInterval(20);

    if (this->m_RdpClientMode == RdpClientMode::EnhancedSession)
    {
        this->m_RdpClient->DesktopWidth(
            this->m_RecommendedDisplayResolution.cx);
        this->m_RdpClient->DesktopHeight(
            this->m_RecommendedDisplayResolution.cy);

        try
        {
            VARIANT Value;
            Value.vt = VT_UI4;
            Value.uintVal = ::MulDiv(
                100,
                ::GetDpiForWindow(this->m_hWnd),
                USER_DEFAULT_SCREEN_DPI);
            this->m_RdpClient->Property(L"DesktopScaleFactor", Value);
        }
        catch (...)
        {

        }

        try
        {
            VARIANT Value;
            Value.vt = VT_UI4;
            Value.uintVal = 100;
            this->m_RdpClient->Property(L"DeviceScaleFactor", Value);
        }
        catch (...)
        {

        }
    }

    this->m_RdpClient->MarkRdpSettingsSecure(true);

    this->RdpClientConnect();
}

void NanaBox::MainWindow::RdpClientUninitialize()
{
    this->KillTimer(
        NanaBox::MainWindowTimerEvents::SyncDisplaySettings);
    this->m_RdpClient->DisableEventsDispatcher();
    this->m_RdpClientWindow.DestroyWindow();
    this->m_RdpClient = nullptr;
}

void NanaBox::MainWindow::RdpClientConnect()
{
    winrt::check_hresult(::RDPBASE_CreateInstance(
        this->m_PlatformContext.get(),
        CLSID_RDPENCNamedPipeDirectConnector,
        IID_IRDPENCNamedPipeDirectConnector,
        this->m_RdpNamedPipe.put_void()));
    winrt::check_hresult(this->m_RdpNamedPipe->InitializeInstance(
        this->m_RdpNamedPipeCallbacks.get()));
    winrt::check_hresult(this->m_RdpNamedPipe->StartConnect(
        Mile::ToWideString(
            CP_UTF8,
            Mile::FormatString(
                this->m_RdpClientMode == RdpClientMode::EnhancedSession
                ? "\\\\.\\pipe\\%s.EnhancedSession"
                : "\\\\.\\pipe\\%s.BasicSession",
                this->m_Configuration.Name.c_str())).c_str()));
}

NanaBox::RdpNamedPipeCallbacks::RdpNamedPipeCallbacks(
    _In_ MainWindow* Instance)
    : m_Instance(Instance)
{

}

void STDMETHODCALLTYPE NanaBox::RdpNamedPipeCallbacks::OnConnectionCompleted(
    _In_ IUnknown* pNetStream)
{
    this->m_Instance->m_RdpClient->ConnectWithEndpoint(pNetStream);

    this->m_Instance->m_RdpClient->Connect();

    this->m_Instance->m_RdpClientWindow.SetFocus();
}

void STDMETHODCALLTYPE NanaBox::RdpNamedPipeCallbacks::OnConnectorError(
    _In_ HRESULT hrError)
{
    UNREFERENCED_PARAMETER(hrError);

    this->m_Instance->RdpClientOnDisconnected(
        exDiscReasonServerDeniedConnection);
}
