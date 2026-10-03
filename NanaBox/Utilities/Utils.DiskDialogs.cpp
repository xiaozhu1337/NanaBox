#include "Utils.h"

#include <Mile.Helpers.h>
#include <Mile.Xaml.h>

#include <initguid.h>
#include <virtdisk.h>
#pragma comment(lib,"virtdisk.lib")

#include <Shlwapi.h>
#pragma comment(lib, "Shlwapi.lib")

#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

#include <CommCtrl.h>
#pragma comment(lib, "comctl32.lib")

#include <sddl.h>

#include <winrt/Windows.UI.Xaml.Controls.h>

#include "MessagePage.h"
#include "AboutPage.h"
#include "NewVirtualHardDiskPage.h"
#include "ResizeVirtualHardDiskPage.h"

namespace winrt
{
    using Windows::UI::Xaml::HorizontalAlignment;
    using Windows::UI::Xaml::ThicknessHelper;
    using Windows::UI::Xaml::VerticalAlignment;
    using NanaBox::ProgressRing;
}

winrt::handle ShowAboutDialog(
    _In_ HWND ParentWindowHandle)
{
    return winrt::handle(Mile::CreateThread([=]()
    {
        winrt::check_hresult(::MileXamlThreadInitialize());

        HWND WindowHandle = ::CreateXamlDialog(ParentWindowHandle);
        if (!WindowHandle)
        {
            return;
        }

        using InstanceType = winrt::NanaBox::AboutPage;
        using ImplementationType = winrt::NanaBox::implementation::AboutPage;
        InstanceType Window = winrt::make<ImplementationType>(WindowHandle);
        ::ShowXamlDialog(
            WindowHandle,
            480,
            320,
            winrt::get_abi(Window),
            ParentWindowHandle);

        // Do not call MileXamlThreadUninitialize instead, because it will cause
        // second time uninitialization in some Windows releases, which will
        // lead to a crash.
        {
            ::SendMessageW(WindowHandle, WM_CLOSE, 0, 0);
            ::TerminateThread(::GetCurrentThread(), 0);
        }
    }));
}

winrt::handle ShowNewVirtualHardDiskDialog(
    _In_ HWND ParentWindowHandle)
{
    return winrt::handle(Mile::CreateThread([=]()
    {
        winrt::check_hresult(::MileXamlThreadInitialize());

        HWND WindowHandle = ::CreateXamlDialog(ParentWindowHandle);
        if (!WindowHandle)
        {
            return;
        }

        using InstanceType =
            winrt::NanaBox::NewVirtualHardDiskPage;
        using ImplementationType =
            winrt::NanaBox::implementation::NewVirtualHardDiskPage;
        InstanceType Window = winrt::make<ImplementationType>(WindowHandle);
        ::ShowXamlDialog(
            WindowHandle,
            480,
            320,
            winrt::get_abi(Window),
            ParentWindowHandle);

        // Do not call MileXamlThreadUninitialize instead, because it will cause
        // second time uninitialization in some Windows releases, which will
        // lead to a crash.
        {
            ::SendMessageW(WindowHandle, WM_CLOSE, 0, 0);
            ::TerminateThread(::GetCurrentThread(), 0);
        }
    }));
}

winrt::handle ShowCompactVirtualHardDiskDialog(
    _In_ HWND ParentWindowHandle)
{
    return winrt::handle(Mile::CreateThread([=]()
    {
        try
        {
            winrt::com_ptr<IFileDialog> FileDialog =
                winrt::create_instance<IFileDialog>(CLSID_FileOpenDialog);

            DWORD Flags = 0;
            winrt::check_hresult(FileDialog->GetOptions(&Flags));

            Flags |= FOS_FORCEFILESYSTEM;
            Flags |= FOS_NOCHANGEDIR;
            Flags |= FOS_DONTADDTORECENT;
            winrt::check_hresult(FileDialog->SetOptions(Flags));

            COMDLG_FILTERSPEC SupportedFileTypes[] =
            {
                {
                    L"Virtual Hard Disk Files (*.vhd;*.vhdx)",
                    L"*.vhd;*.vhdx"
                }
            };
            winrt::check_hresult(FileDialog->SetFileTypes(
                ARRAYSIZE(SupportedFileTypes),
                SupportedFileTypes));

            winrt::check_hresult(FileDialog->SetTitle(
                Mile::WinRT::GetLocalizedString(
                    L"CompactVirtualHardDiskWizard/PathTip").c_str()));

            winrt::check_hresult(FileDialog->Show(ParentWindowHandle));

            std::wstring FilePath = ::GetFileSystemPathFromFileDialog(
                FileDialog.get());
            if (FilePath.empty())
            {
                return;
            }

            winrt::hstring SuccessInstructionText =
                Mile::WinRT::GetLocalizedString(
                    L"CompactVirtualHardDiskWizard/SuccessInstructionText");
            winrt::hstring SuccessContentText =
                Mile::WinRT::GetLocalizedString(
                    L"CompactVirtualHardDiskWizard/SuccessContextText");

            try
            {
                HWND WaitingHandle = ::ShowOperationWaitingWindow(
                    ParentWindowHandle);

                DWORD Error = ::SimpleCompactVirtualDisk(FilePath.c_str());

                if (ERROR_SUCCESS == Error)
                {
                    ::ShowMessageDialog(
                        ParentWindowHandle,
                        SuccessInstructionText.c_str(),
                        Mile::FormatWideString(
                            SuccessContentText.c_str(),
                            FilePath.c_str()).c_str());
                }
                else
                {
                    ::ShowErrorMessageDialog(
                        ParentWindowHandle,
                        winrt::hresult_error(HRESULT_FROM_WIN32(Error)));
                }

                ::SendMessageW(WaitingHandle, WM_CLOSE, 0, 0);
            }
            catch (...)
            {

            }
        }
        catch (...)
        {

        }
    }));
}

winrt::handle ShowResizeVirtualHardDiskDialog(
    _In_ HWND ParentWindowHandle)
{
    return winrt::handle(Mile::CreateThread([=]()
    {
        winrt::check_hresult(::MileXamlThreadInitialize());

        HWND WindowHandle = ::CreateXamlDialog(ParentWindowHandle);
        if (!WindowHandle)
        {
            return;
        }

        using InstanceType =
            winrt::NanaBox::ResizeVirtualHardDiskPage;
        using ImplementationType =
            winrt::NanaBox::implementation::ResizeVirtualHardDiskPage;
        InstanceType Window = winrt::make<ImplementationType>(WindowHandle);
        ::ShowXamlDialog(
            WindowHandle,
            480,
            320,
            winrt::get_abi(Window),
            ParentWindowHandle);

        // Do not call MileXamlThreadUninitialize instead, because it will cause
        // second time uninitialization in some Windows releases, which will
        // lead to a crash.
        {
            ::SendMessageW(WindowHandle, WM_CLOSE, 0, 0);
            ::TerminateThread(::GetCurrentThread(), 0);
        }
    }));
}

BOOL LaunchDocumentation()
{
    SHELLEXECUTEINFOW ExecInfo = {};
    ExecInfo.cbSize = sizeof(SHELLEXECUTEINFOW);
    ExecInfo.lpVerb = L"open";
    ExecInfo.lpFile =
        L"https://github.com/M2Team/NanaBox/"
        L"blob/main/Documents/ConfigurationReference.md";
    ExecInfo.nShow = SW_SHOWNORMAL;
    return ::ShellExecuteExW(&ExecInfo);
}

std::string GetCurrentProcessUserStringSid()
{
    static std::string CachedResult = ([]() -> std::string
    {
        std::string Result;

        HANDLE CurrentProcessToken = nullptr;
        if (::OpenProcessToken(
            ::GetCurrentProcess(),
            TOKEN_ALL_ACCESS,
            &CurrentProcessToken))
        {
            DWORD Length = 0;
            ::GetTokenInformation(
                CurrentProcessToken,
                TOKEN_INFORMATION_CLASS::TokenUser,
                nullptr,
                0,
                &Length);
            if (ERROR_INSUFFICIENT_BUFFER == ::GetLastError())
            {
                PTOKEN_USER Information = reinterpret_cast<PTOKEN_USER>(
                    ::MileAllocateMemory(Length));
                if (Information)
                {
                    if (::GetTokenInformation(
                        CurrentProcessToken,
                        TOKEN_INFORMATION_CLASS::TokenUser,
                        Information,
                        Length,
                        &Length))
                    {
                        LPWSTR StringSid = nullptr;
                        if (::ConvertSidToStringSidW(
                            Information->User.Sid,
                            &StringSid))
                        {
                            Result = Mile::ToString(
                                CP_UTF8,
                                std::wstring(StringSid));
                            ::LocalFree(StringSid);
                        }
                    }

                    ::MileFreeMemory(Information);
                }
            }

            ::CloseHandle(CurrentProcessToken);
        }

        return Result;
    }());

    return CachedResult;
}

HWND ShowOperationWaitingWindow(
    _In_ HWND ParentWindowHandle)
{
    HWND Result = nullptr;

    HANDLE ReturnEvent = ::CreateEventExW(
        nullptr,
        nullptr,
        0,
        EVENT_ALL_ACCESS);
    if (!ReturnEvent)
    {
        return nullptr;
    }

    ::CloseHandle(Mile::CreateThread([=, &Result]()
    {
        winrt::check_hresult(::MileXamlThreadInitialize());

        HWND WindowHandle = ::CreateXamlWindow(
            ParentWindowHandle,
            0,
            WS_POPUP);
        if (!WindowHandle)
        {
            return;
        }
        Result = WindowHandle;

        ::SetEvent(ReturnEvent);

        DWM_WINDOW_CORNER_PREFERENCE Preference = DWMWCP_ROUNDSMALL;
        ::DwmSetWindowAttribute(
            WindowHandle,
            DWMWA_WINDOW_CORNER_PREFERENCE,
            &Preference,
            sizeof(DWM_WINDOW_CORNER_PREFERENCE));

        winrt::ProgressRing Content = winrt::ProgressRing();
        Content.Margin(winrt::ThicknessHelper::FromUniformLength(12));
        Content.HorizontalAlignment(winrt::HorizontalAlignment::Stretch);
        Content.VerticalAlignment(winrt::VerticalAlignment::Stretch);
        // Content.IsActive(true);

        ::ShowXamlDialog(
            WindowHandle,
            72,
            72,
            winrt::get_abi(Content),
            ParentWindowHandle);

        // Do not call MileXamlThreadUninitialize instead, because it will cause
        // second time uninitialization in some Windows releases, which will
        // lead to a crash.
        {
            ::SendMessageW(WindowHandle, WM_CLOSE, 0, 0);
            ::TerminateThread(::GetCurrentThread(), 0);
        }
    }));

    ::WaitForSingleObjectEx(ReturnEvent, INFINITE, FALSE);

    return Result;
}
