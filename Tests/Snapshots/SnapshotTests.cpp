#include "SnapshotStore.h"
#include "SnapshotFiles.h"

#include <virtdisk.h>
#include <array>
#include <fstream>
#include <iostream>
#include <functional>

namespace fs = std::filesystem;
namespace Files = NanaBox::SnapshotFiles;
using Json = nlohmann::json;

namespace
{
    void Require(bool Value, char const* Message)
    {
        if (!Value) throw std::runtime_error(Message);
    }

    void Reject(std::function<void()> const& Operation, char const* Message)
    {
        bool Failed = false;
        try { Operation(); }
        catch (...) { Failed = true; }
        Require(Failed, Message);
    }

    fs::path Resolve(fs::path const& Directory, std::string const& Value)
    {
        auto Path = fs::path(winrt::to_hstring(Value).c_str());
        return Path.is_absolute() ? Path : Directory / Path;
    }

    VIRTUAL_STORAGE_TYPE Type(fs::path const& Path)
    {
        return { static_cast<ULONG>(Path.extension() == L".vhd" ? VIRTUAL_STORAGE_TYPE_DEVICE_VHD :
            VIRTUAL_STORAGE_TYPE_DEVICE_VHDX), VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT };
    }

    void BaseDisk(fs::path const& Path)
    {
        auto Storage = Type(Path);
        CREATE_VIRTUAL_DISK_PARAMETERS Parameters = {};
        Parameters.Version = CREATE_VIRTUAL_DISK_VERSION_2;
        Parameters.Version2.MaximumSize = 64ULL * 1024 * 1024;
        Parameters.Version2.SectorSizeInBytes = 512;
        Parameters.Version2.PhysicalSectorSizeInBytes = 512;
        winrt::handle Handle;
        winrt::check_hresult(HRESULT_FROM_WIN32(::CreateVirtualDisk(&Storage,
            Path.c_str(), VIRTUAL_DISK_ACCESS_NONE, nullptr, CREATE_VIRTUAL_DISK_FLAG_NONE,
            0, &Parameters, nullptr, Handle.put())));
    }

    // Read/write a sector through the actual Windows virtual disk provider.
    // This avoids a mocked disk implementation or dependence on guest images.
    char Sector(fs::path const& Path, char Write = 0)
    {
        auto Storage = Type(Path);
        OPEN_VIRTUAL_DISK_PARAMETERS Parameters = {};
        Parameters.Version = OPEN_VIRTUAL_DISK_VERSION_1;
        winrt::handle Handle;
        winrt::check_hresult(HRESULT_FROM_WIN32(::OpenVirtualDisk(&Storage, Path.c_str(),
            VIRTUAL_DISK_ACCESS_ALL, OPEN_VIRTUAL_DISK_FLAG_NONE, &Parameters, Handle.put())));
        ATTACH_VIRTUAL_DISK_PARAMETERS Attach = {};
        Attach.Version = ATTACH_VIRTUAL_DISK_VERSION_1;
        auto Flags = ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER;
        winrt::check_hresult(HRESULT_FROM_WIN32(::AttachVirtualDisk(Handle.get(), nullptr,
            Flags, 0, &Attach, nullptr)));
        try
        {
            wchar_t Physical[1024] = {};
            ULONG Size = sizeof(Physical);
            winrt::check_hresult(HRESULT_FROM_WIN32(::GetVirtualDiskPhysicalPath(
                Handle.get(), &Size, Physical)));
            winrt::file_handle File(::CreateFileW(Physical, GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
            winrt::check_bool(static_cast<bool>(File));
            LARGE_INTEGER Offset = {};
            Offset.QuadPart = 4 * 1024 * 1024;
            winrt::check_bool(::SetFilePointerEx(File.get(), Offset, nullptr, FILE_BEGIN));
            std::array<char, 512> Buffer = {};
            DWORD Bytes = 0;
            if (Write)
            {
                Buffer.fill(Write);
                winrt::check_bool(::WriteFile(File.get(), Buffer.data(),
                    static_cast<DWORD>(Buffer.size()), &Bytes, nullptr));
                winrt::check_bool(::FlushFileBuffers(File.get()));
            }
            else
                winrt::check_bool(::ReadFile(File.get(), Buffer.data(),
                    static_cast<DWORD>(Buffer.size()), &Bytes, nullptr));
            Require(Bytes == Buffer.size(), "Sector IO was incomplete.");
            File.close();
            winrt::check_hresult(HRESULT_FROM_WIN32(::DetachVirtualDisk(
                Handle.get(), DETACH_VIRTUAL_DISK_FLAG_NONE, 0)));
            return Buffer[0];
        }
        catch (...)
        {
            ::DetachVirtualDisk(Handle.get(), DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
            throw;
        }
    }

    void Run(fs::path const& Directory, std::wstring const& Extension)
    {
        fs::create_directories(Directory);
        auto ConfigPath = Directory / L"test.7b";
        auto Base = Directory / (L"base" + Extension);
        auto Data = Directory / (L"data" + Extension);
        auto Guest = Directory / L"test.vmgs";
        BaseDisk(Base);
        BaseDisk(Data);
        Files::AtomicWrite(Guest, "guest-state-A");
        Sector(Base, 'A');
        Sector(Data, 'X');
        Json Config = { { "Type", "VirtualMachine" }, { "Name", "snapshot-test" },
            { "ProcessorCount", 2 }, { "MemorySize", 1024 },
            { "GuestStateFile", "test.vmgs" }, { "RuntimeStateFile", "test.vmrs" },
            { "SaveStateFile", "" }, { "ScsiDevices", Json::array({
                { { "Type", "VirtualDisk" }, { "Path", winrt::to_string(Base.filename().wstring()) } },
                { { "Type", "VirtualDisk" }, { "Path", winrt::to_string(Data.filename().wstring()) } }
            }) } };
        Files::WriteConfiguration(ConfigPath, Config);
        NanaBox::SnapshotStore Store(ConfigPath);
        Store.Recover();
        Require(Store.List(0).Total == 0, "Initial catalog is not empty.");
        Store.Create(Config, "A", "Initial disks");
        auto A = Store.List(0).Items.at(0).Id;
        Config = Files::ReadConfiguration(ConfigPath);
        auto Working = Resolve(Directory, Config.at("ScsiDevices").at(0).at("Path"));
        auto WorkingData = Resolve(Directory, Config.at("ScsiDevices").at(1).at("Path"));
        Require(Working != Base && Sector(Working) == 'A', "Child did not inherit parent data.");
        Sector(Working, 'B');
        Sector(WorkingData, 'Y');
        Config["MemorySize"] = 2048;
        Files::AtomicWrite(Guest, "guest-state-B");
        Files::WriteConfiguration(ConfigPath, Config);
        Store.Create(Config, "B", "Two-disk state");
        auto B = Store.List(0).Items.at(0).Id;
        Store.Restore(A);
        Config = Files::ReadConfiguration(ConfigPath);
        Working = Resolve(Directory, Config.at("ScsiDevices").at(0).at("Path"));
        WorkingData = Resolve(Directory, Config.at("ScsiDevices").at(1).at("Path"));
        Require(Sector(Working) == 'A' && Sector(WorkingData) == 'X', "Restore A lost disk consistency.");
        Require(Config.at("MemorySize") == 1024 && Config.at("SaveStateFile") == "",
            "Restore did not reset VM configuration or memory state.");
        std::ifstream GuestStream(Resolve(Directory, Config.at("GuestStateFile")), std::ios::binary);
        std::string GuestText((std::istreambuf_iterator<char>(GuestStream)), {});
        Require(GuestText == "guest-state-A", "VMGS was not restored.");
        GuestStream.close();
        Sector(Working, 'C');
        Store.Create(Config, "C", "Branch from A");
        auto C = Store.List(0).Items.at(0);
        Require(C.ParentId == A, "Branch ancestry is wrong.");
        Store.Delete(A);
        Require(Store.List(0).Total == 2, "Delete removed other snapshots.");
        Store.Restore(B);
        Config = Files::ReadConfiguration(ConfigPath);
        Working = Resolve(Directory, Config.at("ScsiDevices").at(0).at("Path"));
        Require(Sector(Working) == 'B' && Config.at("MemorySize") == 2048,
            "Delete removed an ancestor needed by B.");
        Store.Restore(C.Id);
        Config = Files::ReadConfiguration(ConfigPath);
        Working = Resolve(Directory, Config.at("ScsiDevices").at(0).at("Path"));
        Require(Sector(Working) == 'C', "Independent branch C was damaged.");

        auto Root = fs::path(ConfigPath.wstring() + L".snapshots");
        auto Broken = Config;
        Broken["GuestStateFile"] = "missing.vmgs";
        auto Before = Store.List(0).Total;
        Reject([&]() { Store.Create(Broken, "failure", ""); }, "Missing VMGS was accepted.");
        Require(Store.List(0).Total == Before && !fs::exists(Root / L"transaction.yaml"),
            "Preparation failure modified the catalog.");

        // Sharing prevents config replacement after the journal has been published.
        winrt::file_handle Blocker(::CreateFileW(ConfigPath.c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
        winrt::check_bool(static_cast<bool>(Blocker));
        Reject([&]() { Store.Create(Config, "recover", "durable transaction"); },
            "Blocked transaction unexpectedly succeeded.");
        Require(fs::exists(Root / L"transaction.yaml"), "Recovery journal was lost.");
        Require(Store.List(0).Total == Before, "Listing applied a pending transaction.");
        Blocker.close();
        Store.Recover();
        Require(!fs::exists(Root / L"transaction.yaml") && Store.List(0).Total == Before + 1,
            "Transaction did not roll forward.");
        Config = Files::ReadConfiguration(ConfigPath);
        Files::ValidateDisk(Resolve(Directory, Config.at("ScsiDevices").at(0).at("Path")));

        // A crash before publication must roll back only staged, owned files.
        auto Orphan = Root / (Files::NewId() + ".vmgs");
        Files::AtomicWrite(Orphan, "interrupted preparation");
        Files::WriteYaml(Root / L"preparation.yaml", { { "version", 1 },
            { "paths", { winrt::to_string(Orphan.wstring()) } } });
        Store.Recover();
        Require(!fs::exists(Orphan) && !fs::exists(Root / L"preparation.yaml"),
            "Unpublished preparation was not rolled back.");
        Require(fs::exists(Base), "Preparation rollback deleted a user disk.");

        // Repeated snapshots exercise backend/UI page bounds with real disk chains.
        for (int i = 0; i < 20; ++i)
        {
            Store.Create(Config, "page-" + std::to_string(i), "");
            Config = Files::ReadConfiguration(ConfigPath);
        }
        Require(Store.List(0, 20).Items.size() == 20 && Store.List(1, 20).Items.size() == 3,
            "Pagination returns wrong rows.");
        Require(Store.List(SIZE_MAX, 20).Items.empty(), "Page overflow was not rejected.");
        Reject([&]() { Store.List(0, 0); }, "Zero page size was accepted.");
        Reject([&]() { Store.List(0, 101); }, "Excessive page size was accepted.");
        Reject([&]() { Store.Restore("../escape"); }, "Unsafe identifier was accepted.");
        auto Physical = Config;
        Physical["ScsiDevices"].push_back({ { "Type", "PhysicalDevice" }, { "Path", "disk0" } });
        Reject([&]() { Store.Validate(Physical); }, "Passthrough disk was accepted.");
        auto Duplicate = Config;
        Duplicate["ScsiDevices"].push_back(Duplicate.at("ScsiDevices").at(0));
        Reject([&]() { Store.Validate(Duplicate); }, "Duplicate disks were accepted.");
        auto Suspended = Config;
        Suspended["SaveStateFile"] = "suspended.vmrs";
        Reject([&]() { Store.Validate(Suspended); }, "Suspended VM was accepted.");

        // Deleting all visible history still preserves the running branch's chain.
        while (Store.List(0).Total)
            Store.Delete(Store.List(0).Items.at(0).Id);
        Config = Files::ReadConfiguration(ConfigPath);
        Require(Sector(Resolve(Directory, Config.at("ScsiDevices").at(0).at("Path"))) == 'C',
            "Deleting all snapshots damaged the current branch.");
        Require(fs::exists(Base) && fs::exists(Data), "User-owned disks were deleted.");
        std::cout << "PASS " << winrt::to_string(Extension)
            << ": disk rollback, multi-disk consistency, config/VMGS, branches, deletion, recovery, pagination, validation\n";
    }
}

int wmain(int Count, wchar_t** Arguments)
{
    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        Require(Count == 2, "Supply an empty test directory.");
        auto Directory = fs::absolute(Arguments[1]);
        Run(Directory / L"vhdx", L".vhdx");
        Run(Directory / L"vhd", L".vhd");
        return 0;
    }
    catch (winrt::hresult_error const& Error)
    {
        std::cerr << "FAIL: " << winrt::to_string(Error.message()) << " ("
            << Error.code().value << ")\n";
    }
    catch (std::exception const& Error)
    {
        std::cerr << "FAIL: " << Error.what() << "\n";
    }
    return 1;
}
