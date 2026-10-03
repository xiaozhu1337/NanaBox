#include "pch.h"
#include "MainWindow.h"
#include "SnapshotPage.h"
#include "SnapshotFiles.h"

void NanaBox::MainWindow::ShowSnapshots()
{
    try
    {
        HWND Handle = ::CreateXamlDialog(this->m_hWnd);
        if (!Handle) winrt::throw_last_error();
        auto Page = winrt::make_self<winrt::NanaBox::implementation::SnapshotPage>(
            Handle, this->m_ConfigurationFilePath);
        ::ShowXamlDialog(Handle, 760, 720, winrt::get_abi(Page.as<winrt::NanaBox::SnapshotPage>()),
            this->m_hWnd);
        auto Request = Page->Request();
        if (Request.Action != SnapshotAction::None)
            this->RunSnapshotOperation(Request);
    }
    catch (...)
    {
        auto Error = Mile::WinRT::ToHResultError();
        SnapshotFiles::Log("open snapshot manager", Error);
        ::ShowErrorMessageDialog(this->m_hWnd, Error);
    }
}

winrt::fire_and_forget NanaBox::MainWindow::RunSnapshotOperation(SnapshotRequest Request)
{
    if (this->m_SnapshotOperation.exchange(true)) co_return;
    winrt::apartment_context Ui;
    auto System = this->m_VirtualMachine;
    bool WasPaused = this->m_VirtualMachinePaused;
    winrt::hresult_error Error;
    bool Failed = false;
    bool Stopped = false;
    HWND Waiting = nullptr;
    this->EnableWindow(FALSE);
    try
    {
        auto Configuration = nlohmann::json::parse(
            NanaBox::SerializeConfiguration(this->m_Configuration)).at("NanaBox");
        SnapshotStore Store(this->m_ConfigurationFilePath);
        if (!System) throw winrt::hresult_illegal_method_call(L"The virtual machine is unavailable.");
        Waiting = ::ShowOperationWaitingWindow(this->m_hWnd);
        co_await winrt::resume_background();
        if (Request.Action == SnapshotAction::Create) Store.Validate(Configuration);
        if (WasPaused) System->Resume();
        System->ShutdownForSnapshot();
        Stopped = true;
        co_await Ui;
        // Close HCS handles before touching VMGS or creating differencing disks.
        this->m_VirtualMachine = nullptr;
        System = nullptr;
        this->m_VirtualMachineRunning = false;
        this->m_RdpClient->Disconnect();
        co_await winrt::resume_background();
        switch (Request.Action)
        {
        case SnapshotAction::Create:
            Store.Create(Configuration, Request.Name, Request.Description);
            break;
        case SnapshotAction::Restore:
            Store.Restore(Request.Id);
            break;
        case SnapshotAction::Delete:
            Store.Delete(Request.Id);
            break;
        default:
            break;
        }
    }
    catch (...)
    {
        Error = Mile::WinRT::ToHResultError();
        SnapshotFiles::Log("snapshot operation", Error);
        Failed = true;
        if (System && System->HasExited()) Stopped = true;
    }
    co_await Ui;
    if (Stopped)
    {
        this->m_VirtualMachine = nullptr;
        System = nullptr;
        this->m_VirtualMachineRunning = false;
        try
        {
            // Startup recovery finishes any durable transaction before booting.
            this->InitializeVirtualMachine();
            this->RdpClientUninitialize();
            this->RdpClientInitialize();
        }
        catch (...)
        {
            Error = Mile::WinRT::ToHResultError();
            SnapshotFiles::Log("restart after snapshot", Error);
            Failed = true;
        }
    }
    else if (WasPaused && System)
    {
        try { System->Pause(); }
        catch (...) { SnapshotFiles::Log("restore pause state", Mile::WinRT::ToHResultError()); }
    }
    if (Waiting) ::SendMessageW(Waiting, WM_CLOSE, 0, 0);
    this->EnableWindow(TRUE);
    this->m_SnapshotOperation.store(false);
    if (Failed) ::ShowErrorMessageDialog(this->m_hWnd, Error);
    else ::ShowMessageDialog(this->m_hWnd,
        Mile::WinRT::GetLocalizedString(L"Snapshots/SuccessTitle"),
        Mile::WinRT::GetLocalizedString(L"Snapshots/SuccessText"));
    if (Stopped && !this->m_VirtualMachineRunning) this->PostMessageW(WM_CLOSE);
}
