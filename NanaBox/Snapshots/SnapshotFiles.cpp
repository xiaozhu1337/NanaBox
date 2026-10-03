#include "SnapshotFiles.h"

#include <winrt/base.h>
#include <initguid.h>
#include <virtdisk.h>
#include <objbase.h>
#include <fstream>
#include <sstream>
#include <thread>
#include <mutex>
#include <map>

#pragma comment(lib, "virtdisk.lib")
#pragma comment(lib, "ole32.lib")

namespace
{
    std::string Read(std::filesystem::path const& Path)
    {
        std::ifstream Stream(Path, std::ios::binary);
        if (!Stream)
        {
            throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),
                winrt::hstring(Path.wstring()));
        }
        std::ostringstream Buffer;
        Buffer << Stream.rdbuf();
        return Buffer.str();
    }

    VIRTUAL_STORAGE_TYPE StorageType(std::filesystem::path const& Path)
    {
        std::wstring Extension = Path.extension().wstring();
        VIRTUAL_STORAGE_TYPE Type = {};
        Type.VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT;
        if (!_wcsicmp(Extension.c_str(), L".vhdx"))
        {
            Type.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
        }
        else if (!_wcsicmp(Extension.c_str(), L".vhd"))
        {
            Type.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHD;
        }
        else
        {
            throw winrt::hresult_invalid_argument(L"Snapshots require VHD or VHDX disks.");
        }
        return Type;
    }
}

void NanaBox::SnapshotFiles::AtomicWrite(
    std::filesystem::path const& Path, std::string const& Content)
{
    std::filesystem::path Temporary = Path.wstring() + L".pending";
    winrt::file_handle File(::CreateFileW(Temporary.c_str(), GENERIC_WRITE,
        0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    winrt::check_bool(static_cast<bool>(File));
    DWORD Written = 0;
    winrt::check_bool(::WriteFile(File.get(), Content.data(),
        static_cast<DWORD>(Content.size()), &Written, nullptr));
    winrt::check_bool(Written == Content.size());
    winrt::check_bool(::FlushFileBuffers(File.get()));
    File.close();
    winrt::check_bool(::MoveFileExW(Temporary.c_str(), Path.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
}

void NanaBox::SnapshotFiles::CopyState(std::filesystem::path const& Source,
    std::filesystem::path const& Destination)
{
    winrt::check_bool(::CopyFileW(Source.c_str(), Destination.c_str(), TRUE));
    winrt::file_handle File(::CreateFileW(Destination.c_str(), GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    winrt::check_bool(static_cast<bool>(File));
    winrt::check_bool(::FlushFileBuffers(File.get()));
}

nlohmann::json NanaBox::SnapshotFiles::ReadYaml(std::filesystem::path const& Path)
{
    // Schema-specific YAML: top-level keys with JSON flow values. JSON strings
    // preserve Unicode, colons, backslashes and line breaks without ambiguity.
    std::istringstream Stream(::Read(Path));
    nlohmann::json Result = nlohmann::json::object();
    std::string Line;
    while (std::getline(Stream, Line))
    {
        if (Line.empty() || Line[0] == '#' || Line == "\r") continue;
        std::size_t Separator = Line.find(": ");
        if (Separator == std::string::npos)
            throw winrt::hresult_invalid_argument(L"Invalid snapshot YAML.");
        std::string Key = Line.substr(0, Separator);
        if (Result.contains(Key))
            throw winrt::hresult_invalid_argument(L"Duplicate snapshot YAML key.");
        Result[Key] = nlohmann::json::parse(Line.substr(Separator + 2));
    }
    if (Result.at("version") != 1)
        throw winrt::hresult_invalid_argument(L"Unsupported snapshot version.");
    return Result;
}

void NanaBox::SnapshotFiles::WriteYaml(std::filesystem::path const& Path,
    nlohmann::json const& Document)
{
    std::string Content = "# NanaBox snapshot metadata; UTF-8 without BOM.\n";
    static std::map<std::string, std::string> const Descriptions = {
        { "version", "Metadata schema version; currently 1." },
        { "active_snapshot", "Last created or restored snapshot ID for the working branch." },
        { "snapshots", "Paged list summaries: ID, parent ID, name, description and UTC time." },
        { "disks", "Owned differencing disk paths and their parent disk paths." },
        { "state_files", "Owned working guest and runtime state file paths." },
        { "garbage", "Exact owned file paths queued for safe deletion." },
        { "id", "Snapshot identifier; must match the manifest filename." },
        { "configuration", "Frozen configuration or target configuration of a transaction." },
        { "catalog", "Target catalog to install when rolling a transaction forward." },
        { "paths", "Exact staged file paths to roll back before transaction publication." }
    };
    for (auto const& Item : Document.items())
    {
        Content += "# " + Descriptions.at(Item.key()) + "\n";
        Content += Item.key() + ": " + Item.value().dump() + "\n";
    }
    AtomicWrite(Path, Content);
}

nlohmann::json NanaBox::SnapshotFiles::ReadConfiguration(
    std::filesystem::path const& Path)
{
    return nlohmann::json::parse(::Read(Path)).at("NanaBox");
}

void NanaBox::SnapshotFiles::WriteConfiguration(std::filesystem::path const& Path,
    nlohmann::json const& Configuration)
{
    AtomicWrite(Path, nlohmann::json({ { "NanaBox", Configuration } }).dump(2));
}

void NanaBox::SnapshotFiles::Remove(std::filesystem::path const& Path)
{
    if (!::DeleteFileW(Path.c_str()))
    {
        DWORD Error = ::GetLastError();
        if (Error != ERROR_FILE_NOT_FOUND)
            winrt::throw_hresult(HRESULT_FROM_WIN32(Error));
    }
}

void NanaBox::SnapshotFiles::Cleanup(
    std::vector<std::filesystem::path> const& Paths) noexcept
{
    for (auto const& Path : Paths)
    {
        try { Remove(Path); }
        catch (...) { Log("cleanup", winrt::hresult_error(winrt::to_hresult())); }
    }
}

std::string NanaBox::SnapshotFiles::NewId()
{
    GUID Id;
    winrt::check_hresult(::CoCreateGuid(&Id));
    wchar_t Text[40] = {};
    ::StringFromGUID2(Id, Text, 40);
    return winrt::to_string(std::wstring(Text + 1, 36));
}

std::string NanaBox::SnapshotFiles::UtcNow()
{
    SYSTEMTIME Time;
    ::GetSystemTime(&Time);
    char Text[32] = {};
    sprintf_s(Text, "%04u-%02u-%02uT%02u:%02u:%02uZ", Time.wYear,
        Time.wMonth, Time.wDay, Time.wHour, Time.wMinute, Time.wSecond);
    return Text;
}

void NanaBox::SnapshotFiles::ValidateId(std::string const& Id)
{
    if (Id.size() != 36)
        throw winrt::hresult_invalid_argument(L"Invalid snapshot identifier.");
    for (std::size_t i = 0; i < Id.size(); ++i)
    {
        bool Separator = i == 8 || i == 13 || i == 18 || i == 23;
        if (Separator ? Id[i] != '-' : !((Id[i] >= '0' && Id[i] <= '9') ||
            (Id[i] >= 'a' && Id[i] <= 'f') || (Id[i] >= 'A' && Id[i] <= 'F')))
            throw winrt::hresult_invalid_argument(L"Invalid snapshot identifier.");
    }
}

void NanaBox::SnapshotFiles::ValidateDisk(std::filesystem::path const& Path)
{
    VIRTUAL_STORAGE_TYPE Type = ::StorageType(Path);
    OPEN_VIRTUAL_DISK_PARAMETERS Parameters = {};
    Parameters.Version = OPEN_VIRTUAL_DISK_VERSION_1;
    winrt::handle Disk;
    DWORD Error = ::OpenVirtualDisk(&Type, Path.c_str(),
        VIRTUAL_DISK_ACCESS_GET_INFO, OPEN_VIRTUAL_DISK_FLAG_NONE,
        &Parameters, Disk.put());
    winrt::check_hresult(HRESULT_FROM_WIN32(Error));
}

void NanaBox::SnapshotFiles::CreateChild(std::filesystem::path const& Path,
    std::filesystem::path const& Parent)
{
    VIRTUAL_STORAGE_TYPE Type = ::StorageType(Parent);
    CREATE_VIRTUAL_DISK_PARAMETERS Parameters = {};
    Parameters.Version = CREATE_VIRTUAL_DISK_VERSION_2;
    Parameters.Version2.ParentPath = Parent.c_str();
    Parameters.Version2.ParentVirtualStorageType = Type;
    winrt::handle Disk;
    DWORD Error = ::CreateVirtualDisk(&Type, Path.c_str(),
        VIRTUAL_DISK_ACCESS_NONE, nullptr, CREATE_VIRTUAL_DISK_FLAG_NONE,
        0, &Parameters, nullptr, Disk.put());
    winrt::check_hresult(HRESULT_FROM_WIN32(Error));
}

void NanaBox::SnapshotFiles::Log(std::string const& Operation,
    winrt::hresult_error const& Error) noexcept
{
    try
    {
        wchar_t Module[32768] = {};
        ::GetModuleFileNameW(nullptr, Module, 32768);
        auto Directory = std::filesystem::path(Module).parent_path() / L"logs";
        std::string Message = UtcNow() + " " + Operation + " " +
            std::to_string(Error.code().value) + " " + winrt::to_string(Error.message()) + "\n";
        std::thread([Directory, Message]()
        {
            try
            {
                static std::mutex Mutex;
                std::lock_guard<std::mutex> Guard(Mutex);
                std::filesystem::create_directories(Directory);
                std::ofstream Stream(Directory / L"snapshots.log", std::ios::binary | std::ios::app);
                Stream << Message;
            }
            catch (...) {}
        }).detach();
    }
    catch (...) {}
}
