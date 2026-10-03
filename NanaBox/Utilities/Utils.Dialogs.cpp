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

void ShowMessageDialog(
    _In_opt_ HWND ParentWindowHandle,
    _In_ winrt::hstring const& InstructionText,
    _In_ winrt::hstring const& ContentText)
{
    winrt::handle Thread = winrt::handle(Mile::CreateThread([&]()
    {
        winrt::check_hresult(::MileXamlThreadInitialize());

        HWND WindowHandle = ::CreateXamlDialog(ParentWindowHandle);
        if (!WindowHandle)
        {
            return;
        }

        using InstanceType = winrt::NanaBox::MessagePage;
        using ImplementationType = winrt::NanaBox::implementation::MessagePage;
        InstanceType Window = winrt::make<ImplementationType>(
            WindowHandle,
            InstructionText,
            ContentText);
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
    ::WaitForSingleObject(Thread.get(), INFINITE);
}

void ShowErrorMessageDialog(
    _In_opt_ HWND ParentWindowHandle,
    _In_ winrt::hresult_error const& Exception)
{
    ::ShowMessageDialog(
        ParentWindowHandle,
        Mile::WinRT::GetLocalizedString(
            L"Messages/ErrorInstructionText"),
        winrt::hstring(Mile::FormatWideString(
            L"[0x%08lX] - %s",
            static_cast<HRESULT>(Exception.code()),
            Exception.message().c_str())));
}

void ShowErrorMessageDialog(
    _In_ winrt::hresult_error const& Exception)
{
    ::ShowErrorMessageDialog(nullptr, Exception);
}

void SimpleRemoveDirectory(
    _In_ LPCWSTR RootPath)
{
    HANDLE RootHandle = ::MileCreateFile(
        RootPath,
        SYNCHRONIZE |
        FILE_LIST_DIRECTORY |
        DELETE |
        FILE_READ_ATTRIBUTES |
        FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (RootHandle != INVALID_HANDLE_VALUE)
    {
        Mile::EnumerateFileByHandle(
            RootHandle,
            [&](
                _In_ PMILE_FILE_ENUMERATE_INFORMATION Information) -> BOOL
        {
            if (::MileIsDotsName(Information->FileName))
            {
                return TRUE;
            }

            std::wstring CurrentPath = Mile::FormatWideString(
                L"%s\\%s",
                RootPath,
                Information->FileName);

            if (Information->FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                ::SimpleRemoveDirectory(CurrentPath.c_str());
                return TRUE;
            }

            ::MileDeleteFileIgnoreReadonlyAttribute(CurrentPath.c_str());

            return TRUE;
        });

        ::MileDeleteFileIgnoreReadonlyAttributeByHandle(RootHandle);

        ::CloseHandle(RootHandle);
    }
}

std::wstring GetLocalStateFolderPath()
{
    static std::wstring CachedResult = ([]() -> std::wstring
    {
        std::wstring FolderPath;
        {
            LPWSTR RawFolderPath = nullptr;
            // KF_FLAG_FORCE_APP_DATA_REDIRECTION, when engaged, causes
            // SHGetKnownFolderPath to return the new AppModel paths
            // (Packages/xxx/RoamingState, etc.) for standard path requests.
            // Using this flag allows us to avoid
            // Windows.Storage.ApplicationData completely.
            winrt::check_hresult(::SHGetKnownFolderPath(
                FOLDERID_LocalAppData,
                KF_FLAG_FORCE_APP_DATA_REDIRECTION,
                nullptr,
                &RawFolderPath));
            FolderPath = std::wstring(RawFolderPath);
            if (!Mile::WinRT::IsPackagedMode())
            {
                FolderPath += L"\\M2-Team\\NanaBox";
            }
            ::CoTaskMemFree(RawFolderPath);
        }

        // Create the directory if it doesn't exist.
        ::MileCreateDirectory(FolderPath.c_str());

        return FolderPath;
    }());

    return CachedResult;
}

std::wstring GetCurrentProcessModulePath()
{
    // 32767 is the maximum path length without the terminating null character.
    std::wstring Path(32767, L'\0');
    Path.resize(::GetModuleFileNameW(
        nullptr, &Path[0], static_cast<DWORD>(Path.size())));
    return Path;
}

DWORD SimpleCreateVirtualDisk(
    _In_ PCWSTR Path,
    _In_ UINT64 Size,
    _Out_ PHANDLE Handle)
{
    VIRTUAL_STORAGE_TYPE StorageType;
    StorageType.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_UNKNOWN;
    StorageType.VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_UNKNOWN;

    CREATE_VIRTUAL_DISK_PARAMETERS Parameters;
    std::memset(&Parameters, 0, sizeof(CREATE_VIRTUAL_DISK_PARAMETERS));
    Parameters.Version = CREATE_VIRTUAL_DISK_VERSION_2;
    Parameters.Version2.MaximumSize = Size;
    Parameters.Version2.BlockSizeInBytes =
        (0 == ::_wcsicmp(::PathFindExtensionW(Path), L".vhdx"))
        ? 0x100000 // 1 MiB for VHDX
        : 0x80000; // 512 KiB for VHD

    return ::CreateVirtualDisk(
        &StorageType,
        Path,
        VIRTUAL_DISK_ACCESS_NONE,
        nullptr,
        CREATE_VIRTUAL_DISK_FLAG_NONE,
        0,
        &Parameters,
        nullptr,
        Handle);
}

DWORD SimpleResizeVirtualDisk(
    _In_ PCWSTR Path,
    _In_ UINT64 Size)
{
    HANDLE DiskHandle = INVALID_HANDLE_VALUE;

    VIRTUAL_STORAGE_TYPE StorageType;
    StorageType.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_UNKNOWN;
    StorageType.VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_UNKNOWN;

    DWORD Error = ::OpenVirtualDisk(
        &StorageType,
        Path,
        VIRTUAL_DISK_ACCESS_ALL,
        OPEN_VIRTUAL_DISK_FLAG_NONE,
        nullptr,
        &DiskHandle);
    if (ERROR_SUCCESS == Error)
    {
        RESIZE_VIRTUAL_DISK_PARAMETERS Parameters;
        std::memset(&Parameters, 0, sizeof(RESIZE_VIRTUAL_DISK_PARAMETERS));
        Parameters.Version = RESIZE_VIRTUAL_DISK_VERSION_1;
        Parameters.Version1.NewSize = Size;

        Error = ::ResizeVirtualDisk(
            DiskHandle,
            RESIZE_VIRTUAL_DISK_FLAG_NONE,
            &Parameters,
            nullptr);

        ::CloseHandle(DiskHandle);
    }
    return Error;
}

DWORD SimpleCompactVirtualDisk(
    _In_ PCWSTR Path)
{
    HANDLE DiskHandle = INVALID_HANDLE_VALUE;

    VIRTUAL_STORAGE_TYPE StorageType;
    StorageType.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_UNKNOWN;
    StorageType.VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_UNKNOWN;

    DWORD Error = ::OpenVirtualDisk(
        &StorageType,
        Path,
        VIRTUAL_DISK_ACCESS_ALL,
        OPEN_VIRTUAL_DISK_FLAG_NONE,
        nullptr,
        &DiskHandle);
    if (ERROR_SUCCESS == Error)
    {
        Error = ::CompactVirtualDisk(
            DiskHandle,
            COMPACT_VIRTUAL_DISK_FLAG_NONE,
            nullptr,
            nullptr);

        ::CloseHandle(DiskHandle);
    }
    return Error;
}
