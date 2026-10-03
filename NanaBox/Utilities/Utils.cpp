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

void SplitCommandLineEx(
    std::wstring const& CommandLine,
    std::vector<std::wstring> const& OptionPrefixes,
    std::vector<std::wstring> const& OptionParameterSeparators,
    std::wstring& ApplicationName,
    std::map<std::wstring, std::wstring>& OptionsAndParameters,
    std::wstring& UnresolvedCommandLine)
{
    ApplicationName.clear();
    OptionsAndParameters.clear();
    UnresolvedCommandLine.clear();

    size_t arg_size = 0;
    for (auto& SplitArgument : Mile::SplitCommandLineWideString(CommandLine))
    {
        // We need to process the application name at the beginning.
        if (ApplicationName.empty())
        {
            // For getting the unresolved command line, we need to cumulate
            // length which including spaces.
            arg_size += SplitArgument.size() + 1;

            // Save
            ApplicationName = SplitArgument;
        }
        else
        {
            bool IsOption = false;
            size_t OptionPrefixLength = 0;

            for (auto& OptionPrefix : OptionPrefixes)
            {
                if (0 == ::_wcsnicmp(
                    SplitArgument.c_str(),
                    OptionPrefix.c_str(),
                    OptionPrefix.size()))
                {
                    IsOption = true;
                    OptionPrefixLength = OptionPrefix.size();
                }
            }

            if (IsOption)
            {
                // For getting the unresolved command line, we need to cumulate
                // length which including spaces.
                arg_size += SplitArgument.size() + 1;

                // Get the option name and parameter.

                wchar_t* OptionStart = &SplitArgument[0] + OptionPrefixLength;
                wchar_t* ParameterStart = nullptr;

                for (auto& OptionParameterSeparator
                    : OptionParameterSeparators)
                {
                    wchar_t* Result = std::wcsstr(
                        OptionStart,
                        OptionParameterSeparator.c_str());
                    if (nullptr == Result)
                    {
                        continue;
                    }

                    Result[0] = L'\0';
                    ParameterStart = Result + OptionParameterSeparator.size();

                    break;
                }

                // Save
                OptionsAndParameters[(OptionStart ? OptionStart : L"")] =
                    (ParameterStart ? ParameterStart : L"");
            }
            else
            {
                // Get the approximate location of the unresolved command line.
                // We use "(arg_size - 1)" to ensure that the program path
                // without quotes can also correctly parse.
                wchar_t* search_start =
                    const_cast<wchar_t*>(CommandLine.c_str()) + (arg_size - 1);

                // Get the unresolved command line. Search for the beginning of
                // the first parameter delimiter called space and exclude the
                // first space by adding 1 to the result.
                wchar_t* command = std::wcsstr(search_start, L" ") + 1;

                // Omit the space. (Thanks to wzzw.)
                while (command && *command == L' ')
                {
                    ++command;
                }

                // Save
                if (command)
                {
                    UnresolvedCommandLine = command;
                }

                break;
            }
        }
    }
}

winrt::hstring FromGuid(
    winrt::guid const& Value)
{
    return winrt::hstring(Mile::FormatWideString(
        L"%08x-%04hx-%04hx-%02hhx%02hhx-%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx",
        Value.Data1,
        Value.Data2,
        Value.Data3,
        Value.Data4[0],
        Value.Data4[1],
        Value.Data4[2],
        Value.Data4[3],
        Value.Data4[4],
        Value.Data4[5],
        Value.Data4[6],
        Value.Data4[7]));
}

std::string ReadAllTextFromUtf8TextFile(
    std::wstring const& Path)
{
    winrt::file_handle FileHandle;

    FileHandle.attach(::MileCreateFile(
        Path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!FileHandle)
    {
        winrt::throw_last_error();
    }

    std::size_t FileSize = 0;
    winrt::check_bool(::MileGetFileSizeByHandle(
        FileHandle.get(),
        &FileSize));

    std::string Content(FileSize, '\0');

    DWORD NumberOfBytesRead = 0;

    winrt::check_bool(::MileReadFile(
        FileHandle.get(),
        const_cast<char*>(Content.c_str()),
        static_cast<DWORD>(FileSize),
        &NumberOfBytesRead));

    if (FileSize >= 3 &&
        Content[0] == '\xEF' &&
        Content[1] == '\xBB' &&
        Content[2] == '\xBF')
    {
        Content.erase(0, 3);
    }

    return Content;
}

void WriteAllTextToUtf8TextFile(
    std::wstring const& Path,
    std::string& Content)
{
    winrt::file_handle FileHandle;

    FileHandle.attach(::MileCreateFile(
        Path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_WRITE,
        nullptr,
        CREATE_ALWAYS,
        FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!FileHandle)
    {
        winrt::throw_last_error();
    }

    DWORD NumberOfBytesWritten = 0;

    winrt::check_bool(::MileWriteFile(
        FileHandle.get(),
        Content.c_str(),
        static_cast<DWORD>(Content.size()),
        &NumberOfBytesWritten));
}

std::wstring GetAbsolutePath(
    std::wstring const& FileName)
{
    // 32767 is the maximum path length without the terminating null character.
    std::wstring Path(32767, L'\0');
    Path.resize(::GetFullPathNameW(
        FileName.c_str(),
        static_cast<DWORD>(Path.size()),
        &Path[0],
        nullptr));
    return Path;
}

std::string GetAbsoluteUtf8Path(
    std::string const& FileName)
{
    return Mile::ToString(
        CP_UTF8,
        ::GetAbsolutePath(Mile::ToWideString(CP_UTF8, FileName)));
}

std::wstring GetDisplayNameFromShellItem(
    _In_ IShellItem* Object,
    _In_ SIGDN Type)
{
    std::wstring Name;
    if (Object)
    {
        LPWSTR RawName = nullptr;
        if (SUCCEEDED(Object->GetDisplayName(Type, &RawName)))
        {
            Name = std::wstring(RawName);
            ::CoTaskMemFree(RawName);
        }
    }
    return Name;
}

std::wstring GetFileSystemPathFromFileDialog(
    _In_ IFileDialog* Object)
{
    std::wstring Path;
    if (Object)
    {
        IShellItem* ShellItem = nullptr;
        if (SUCCEEDED(Object->GetResult(&ShellItem)))
        {
            Path = GetDisplayNameFromShellItem(ShellItem, SIGDN_FILESYSPATH);
            ShellItem->Release();
        }
    }
    return Path;
}

void SetForegroundWindowSimple(
    _In_ HWND WindowHandle)
{
    DWORD ForegroundThreadId = ::GetWindowThreadProcessId(
        ::GetForegroundWindow(),
        nullptr);
    DWORD CurrentThreadId = ::GetCurrentThreadId();
    ::AttachThreadInput(ForegroundThreadId, CurrentThreadId, TRUE);
    ::SetWindowPos(
        WindowHandle,
        HWND_TOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOSIZE | SWP_NOMOVE);
    ::SetWindowPos(
        WindowHandle,
        HWND_NOTOPMOST,
        0,
        0,
        0,
        0,
        SWP_SHOWWINDOW | SWP_NOSIZE | SWP_NOMOVE);
    ::SetForegroundWindow(WindowHandle);
    ::SetFocus(WindowHandle);
    ::SetActiveWindow(WindowHandle);
    ::AttachThreadInput(ForegroundThreadId, CurrentThreadId, FALSE);
}

HWND CreateXamlWindow(
    _In_opt_ HWND ParentWindowHandle,
    _In_ DWORD ExtendedWindowStyle,
    _In_ DWORD WindowStyle)
{
    HWND WindowHandle = ::CreateWindowExW(
        ExtendedWindowStyle,
        L"Mile.Xaml.ContentWindow",
        nullptr,
        WindowStyle,
        CW_USEDEFAULT,
        0,
        CW_USEDEFAULT,
        0,
        ParentWindowHandle,
        nullptr,
        nullptr,
        nullptr);
    if (!WindowHandle)
    {
        return nullptr;
    }
    if (!::SetWindowSubclass(
        WindowHandle,
        [](
            _In_ HWND hWnd,
            _In_ UINT uMsg,
            _In_ WPARAM wParam,
            _In_ LPARAM lParam,
            _In_ UINT_PTR uIdSubclass,
            _In_ DWORD_PTR dwRefData) -> LRESULT
    {
        UNREFERENCED_PARAMETER(uIdSubclass);
        UNREFERENCED_PARAMETER(dwRefData);

        switch (uMsg)
        {
        case WM_CLOSE:
        {
            HWND ParentWindow = ::GetWindow(hWnd, GW_OWNER);
            if (ParentWindow)
            {
                ::SetForegroundWindowSimple(ParentWindow);
                ::EnableWindow(ParentWindow, TRUE);
            }
            break;
        }
        default:
            break;
        }

        return ::DefSubclassProc(
            hWnd,
            uMsg,
            wParam,
            lParam);
    },
        0,
        0))
    {
        ::DestroyWindow(WindowHandle);
        return nullptr;
    }
    return WindowHandle;
}

HWND CreateXamlDialog(
    _In_opt_ HWND ParentWindowHandle)
{
    return ::CreateXamlWindow(
        ParentWindowHandle,
        WS_EX_STATICEDGE | WS_EX_DLGMODALFRAME,
        WS_CAPTION | WS_SYSMENU);
}

int ShowXamlWindow(
    _In_opt_ HWND WindowHandle,
    _In_ int Width,
    _In_ int Height,
    _In_ HWND ParentWindowHandle)
{
    if (!WindowHandle)
    {
        return -1;
    }

    UINT DpiValue = ::GetDpiForWindow(WindowHandle);

    int ScaledWidth = ::MulDiv(Width, DpiValue, USER_DEFAULT_SCREEN_DPI);
    int ScaledHeight = ::MulDiv(Height, DpiValue, USER_DEFAULT_SCREEN_DPI);

    RECT ParentRect = {};
    if (ParentWindowHandle)
    {
        ::GetWindowRect(ParentWindowHandle, &ParentRect);
    }
    else
    {
        HMONITOR MonitorHandle = ::MonitorFromWindow(
            WindowHandle,
            MONITOR_DEFAULTTONEAREST);
        if (MonitorHandle)
        {
            MONITORINFO MonitorInfo;
            MonitorInfo.cbSize = sizeof(MONITORINFO);
            if (::GetMonitorInfoW(MonitorHandle, &MonitorInfo))
            {
                ParentRect = MonitorInfo.rcWork;
            }
        }
    }

    int ParentWidth = ParentRect.right - ParentRect.left;
    int ParentHeight = ParentRect.bottom - ParentRect.top;

    ::SetWindowPos(
        WindowHandle,
        nullptr,
        ParentRect.left + ((ParentWidth - ScaledWidth) / 2),
        ParentRect.top + ((ParentHeight - ScaledHeight) / 2),
        ScaledWidth,
        ScaledHeight,
        SWP_NOZORDER | SWP_NOACTIVATE);

    ::ShowWindow(WindowHandle, SW_SHOW);
    ::UpdateWindow(WindowHandle);

    return ::MileXamlContentWindowDefaultMessageLoop();
}

int ShowXamlDialog(
    _In_opt_ HWND WindowHandle,
    _In_ int Width,
    _In_ int Height,
    _In_ LPVOID Content,
    _In_ HWND ParentWindowHandle)
{
    if (!WindowHandle)
    {
        return -1;
    }

    ::MileAllowNonClientDefaultDrawingForWindow(WindowHandle, FALSE);

    HMENU MenuHandle = ::GetSystemMenu(WindowHandle, FALSE);
    if (MenuHandle)
    {
        ::RemoveMenu(MenuHandle, 0, MF_SEPARATOR);
        ::RemoveMenu(MenuHandle, SC_RESTORE, MF_BYCOMMAND);
        ::RemoveMenu(MenuHandle, SC_SIZE, MF_BYCOMMAND);
        ::RemoveMenu(MenuHandle, SC_MINIMIZE, MF_BYCOMMAND);
        ::RemoveMenu(MenuHandle, SC_MAXIMIZE, MF_BYCOMMAND);
    }

    if (ParentWindowHandle)
    {
        ::EnableWindow(ParentWindowHandle, FALSE);
    }

    if (FAILED(::MileXamlSetXamlContentForContentWindow(
        WindowHandle,
        Content)))
    {
        ::DestroyWindow(WindowHandle);
        return -1;
    }

    int Result = ::ShowXamlWindow(
        WindowHandle,
        Width,
        Height,
        ParentWindowHandle);

    return Result;
}
