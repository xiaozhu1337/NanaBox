#include "SnapshotStore.h"
#include "SnapshotFiles.h"

#include <algorithm>
#include <set>

namespace Files = NanaBox::SnapshotFiles;

NanaBox::SnapshotStore::SnapshotStore(
    std::filesystem::path const& ConfigurationPath) :
    m_ConfigurationPath(std::filesystem::absolute(ConfigurationPath).lexically_normal()),
    m_Root(this->m_ConfigurationPath.wstring() + L".snapshots")
{
}

winrt::file_handle NanaBox::SnapshotStore::Lock()
{
    std::filesystem::create_directories(this->m_Root);
    winrt::file_handle Handle(::CreateFileW((this->m_Root / L"store.lock").c_str(),
        GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!Handle)
        throw winrt::hresult_error(HRESULT_FROM_WIN32(::GetLastError()),
            L"Another snapshot operation is using this virtual machine.");
    return Handle;
}

nlohmann::json NanaBox::SnapshotStore::LoadCatalog()
{
    auto Path = this->m_Root / L"catalog.yaml";
    if (!std::filesystem::exists(Path))
        return { { "version", 1 }, { "active_snapshot", "" },
            { "snapshots", nlohmann::json::array() },
            { "disks", nlohmann::json::array() },
            { "garbage", nlohmann::json::array() } };
    return Files::ReadYaml(Path);
}

std::filesystem::path NanaBox::SnapshotStore::Resolve(std::string const& Path) const
{
    auto Result = std::filesystem::path(winrt::to_hstring(Path).c_str());
    if (Result.is_relative())
        Result = this->m_ConfigurationPath.parent_path() / Result;
    return std::filesystem::absolute(Result).lexically_normal();
}

std::string NanaBox::SnapshotStore::StorePath(std::filesystem::path const& Path) const
{
    auto Relative = Path.lexically_relative(this->m_ConfigurationPath.parent_path());
    return winrt::to_string((Relative.empty() ? Path : Relative).wstring());
}

void NanaBox::SnapshotStore::Recover()
{
    auto Guard = this->Lock();
    this->RecoverLocked();
}

void NanaBox::SnapshotStore::RecoverLocked()
{
    auto Journal = this->m_Root / L"transaction.yaml";
    auto Preparation = this->m_Root / L"preparation.yaml";
    if (std::filesystem::exists(Preparation))
    {
        if (!std::filesystem::exists(Journal))
        {
            auto Paths = Files::ReadYaml(Preparation).at("paths");
            for (auto const& Entry : Paths)
            {
                auto Path = this->Resolve(Entry.get<std::string>());
                if (Path.parent_path() != this->m_Root || std::filesystem::is_symlink(Path))
                    throw winrt::hresult_invalid_argument(L"Unsafe snapshot preparation path.");
                Files::ValidateId(winrt::to_string(Path.filename().wstring()).substr(0, 36));
                Files::Remove(Path);
                Files::Remove(Path.wstring() + L".pending");
            }
        }
        // A published transaction owns these files; discard rollback intent first.
        Files::Remove(Preparation);
    }
    if (std::filesystem::exists(Journal))
    {
        auto Transaction = Files::ReadYaml(Journal);
        // Roll forward before a VM can open either the old or new disks.
        Files::WriteConfiguration(this->m_ConfigurationPath,
            Transaction.at("configuration"));
        Files::WriteYaml(this->m_Root / L"catalog.yaml", Transaction.at("catalog"));
        Files::Remove(Journal);
    }
    auto Catalog = this->LoadCatalog();
    this->Collect(Catalog);
}

void NanaBox::SnapshotStore::Prepare(std::filesystem::path const& Path,
    std::vector<std::filesystem::path>& Prepared)
{
    Prepared.push_back(Path);
    auto Paths = nlohmann::json::array();
    for (auto const& Item : Prepared) Paths.push_back(this->StorePath(Item));
    Files::WriteYaml(this->m_Root / L"preparation.yaml",
        { { "version", 1 }, { "paths", Paths } });
}

void NanaBox::SnapshotStore::Commit(nlohmann::json const& Catalog,
    nlohmann::json const& Configuration, bool* Published)
{
    Files::WriteYaml(this->m_Root / L"transaction.yaml",
        { { "version", 1 }, { "configuration", Configuration }, { "catalog", Catalog } });
    if (Published) *Published = true;
    this->RecoverLocked();
}

NanaBox::SnapshotPageResult NanaBox::SnapshotStore::List(
    std::size_t Page, std::size_t PageSize)
{
    if (PageSize == 0 || PageSize > 100)
        throw winrt::hresult_invalid_argument(L"Snapshot page size must be 1 to 100.");
    auto Guard = this->Lock();
    // Listing never applies a pending transaction while a VM may be running.
    auto Catalog = this->LoadCatalog();
    auto const& Records = Catalog.at("snapshots");
    SnapshotPageResult Result;
    Result.Total = Records.size();
    if (Page > Result.Total / PageSize) return Result;
    auto Begin = Page * PageSize;
    for (std::size_t i = Begin; i < (std::min)(Begin + PageSize, Result.Total); ++i)
    {
        auto const& Record = Records.at(Result.Total - 1 - i);
        Result.Items.push_back({ Record.at("id"), Record.at("parent_id"),
            Record.at("name"), Record.at("description"), Record.at("created_utc"),
            Catalog.at("active_snapshot") == Record.at("id") });
    }
    return Result;
}

void NanaBox::SnapshotStore::Validate(nlohmann::json const& Configuration)
{
    std::set<std::wstring> Paths;
    bool HasDisk = false;
    for (auto const& Device : Configuration.value("ScsiDevices", nlohmann::json::array()))
    {
        auto Type = Device.at("Type").get<std::string>();
        if (Type == "PhysicalDevice")
            throw winrt::hresult_invalid_argument(L"Physical passthrough disks cannot be snapshotted.");
        if (Type != "VirtualDisk") continue;
        HasDisk = true;
        auto Path = this->Resolve(Device.at("Path"));
        auto Key = Path.wstring();
        std::transform(Key.begin(), Key.end(), Key.begin(), ::towlower);
        if (!Paths.insert(Key).second)
            throw winrt::hresult_invalid_argument(L"The same disk is attached more than once.");
        Files::ValidateDisk(Path);
    }
    if (!HasDisk)
        throw winrt::hresult_invalid_argument(L"The VM has no virtual disks to snapshot.");
    if (!Configuration.value("SaveStateFile", std::string()).empty())
        throw winrt::hresult_invalid_argument(L"Resume the suspended VM before taking a disk snapshot.");
}

void NanaBox::SnapshotStore::CreateChildren(nlohmann::json& Configuration,
    nlohmann::json& Catalog, std::string const& Id,
    std::vector<std::filesystem::path>& Prepared)
{
    std::size_t Index = 0;
    for (auto& Device : Configuration.at("ScsiDevices"))
    {
        if (Device.at("Type") != "VirtualDisk") continue;
        auto Parent = this->Resolve(Device.at("Path"));
        auto Child = this->m_Root / (winrt::to_hstring(Id + "-disk" +
            std::to_string(Index++)).c_str() + Parent.extension().wstring());
        this->Prepare(Child, Prepared);
        Files::CreateChild(Child, Parent);
        Device["Path"] = this->StorePath(Child);
        Catalog["disks"].push_back({ { "path", this->StorePath(Child) },
            { "parent", this->StorePath(Parent) } });
    }
}

nlohmann::json NanaBox::SnapshotStore::ReadSnapshot(std::string const& Id)
{
    Files::ValidateId(Id);
    auto Result = Files::ReadYaml(this->m_Root / (Id + ".yaml"));
    if (Result.at("id") != Id)
        throw winrt::hresult_invalid_argument(L"Snapshot identifier does not match its manifest.");
    return Result;
}

void NanaBox::SnapshotStore::Create(nlohmann::json const& Configuration,
    std::string const& Name, std::string const& Description)
{
    if (Name.empty() || Name.size() > 240 || Description.size() > 4096)
        throw winrt::hresult_invalid_argument(L"Enter a snapshot name of up to 240 UTF-8 bytes.");
    auto Guard = this->Lock();
    this->RecoverLocked();
    this->Validate(Configuration);
    auto Catalog = this->LoadCatalog();
    std::string Id = Files::NewId();
    nlohmann::json Frozen = Configuration;
    nlohmann::json Working = Configuration;
    std::vector<std::filesystem::path> Prepared;
    bool Published = false;
    try
    {
        // Normalize paths independently of the process working directory.
        for (auto& Device : Frozen["ScsiDevices"])
            if (Device.contains("Path") && !Device.at("Path").get<std::string>().empty())
                Device["Path"] = this->StorePath(this->Resolve(Device.at("Path")));
        auto Guest = this->m_Root / (Id + ".vmgs");
        this->Prepare(Guest, Prepared);
        Files::CopyState(this->Resolve(Frozen.at("GuestStateFile")), Guest);
        Frozen["GuestStateFile"] = this->StorePath(Guest);
        Frozen["RuntimeStateFile"] = "";
        Frozen["SaveStateFile"] = "";
        auto Manifest = this->m_Root / (Id + ".yaml");
        this->Prepare(Manifest, Prepared);
        Files::WriteYaml(Manifest, { { "version", 1 }, { "id", Id },
            { "configuration", Frozen } });
        this->CreateChildren(Working, Catalog, Id, Prepared);
        Catalog["snapshots"].push_back({ { "id", Id },
            { "parent_id", Catalog.at("active_snapshot") }, { "name", Name },
            { "description", Description }, { "created_utc", Files::UtcNow() } });
        Catalog["active_snapshot"] = Id;
        // Once a journal exists, prepared files belong to its roll-forward.
        this->Commit(Catalog, Working, &Published);
    }
    catch (...)
    {
        if (!Published && !std::filesystem::exists(this->m_Root / L"transaction.yaml"))
            Files::Cleanup(Prepared);
        throw;
    }
}
