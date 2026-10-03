#include "SnapshotStore.h"
#include "SnapshotFiles.h"

#include <map>
#include <set>
#include <algorithm>

namespace Files = NanaBox::SnapshotFiles;

void NanaBox::SnapshotStore::Restore(std::string const& Id)
{
    Files::ValidateId(Id);
    auto Guard = this->Lock();
    this->RecoverLocked();
    auto Catalog = this->LoadCatalog();
    bool Found = false;
    for (auto const& Record : Catalog.at("snapshots"))
        if (Record.at("id") == Id) Found = true;
    if (!Found) throw winrt::hresult_invalid_argument(L"Snapshot no longer exists.");
    auto Working = this->ReadSnapshot(Id).at("configuration");
    auto Previous = Files::ReadConfiguration(this->m_ConfigurationPath);
    if (Working.at("Name") != Previous.at("Name"))
        throw winrt::hresult_invalid_argument(L"Snapshot belongs to another VM.");
    this->Validate(Working);
    std::string Branch = Files::NewId();
    std::vector<std::filesystem::path> Prepared;
    bool Published = false;
    try
    {
        this->CreateChildren(Working, Catalog, Branch, Prepared);
        auto Guest = this->m_Root / (Branch + ".vmgs");
        this->Prepare(Guest, Prepared);
        Files::CopyState(this->Resolve(Working.at("GuestStateFile")), Guest);
        auto Runtime = this->m_Root / (Branch + ".vmrs");
        Working["GuestStateFile"] = this->StorePath(Guest);
        Working["RuntimeStateFile"] = this->StorePath(Runtime);
        Working["SaveStateFile"] = "";
        if (!Catalog.contains("state_files")) Catalog["state_files"] = nlohmann::json::array();
        Catalog["state_files"].push_back(this->StorePath(Guest));
        Catalog["state_files"].push_back(this->StorePath(Runtime));
        Catalog["active_snapshot"] = Id;
        this->Commit(Catalog, Working, &Published);
    }
    catch (...)
    {
        if (!Published && !std::filesystem::exists(this->m_Root / L"transaction.yaml"))
            Files::Cleanup(Prepared);
        throw;
    }
}

void NanaBox::SnapshotStore::Delete(std::string const& Id)
{
    Files::ValidateId(Id);
    auto Guard = this->Lock();
    this->RecoverLocked();
    auto Catalog = this->LoadCatalog();
    auto& Records = Catalog.at("snapshots");
    std::string Parent;
    bool Found = false;
    for (auto const& Record : Records)
    {
        if (Record.at("id") == Id)
        {
            Parent = Record.at("parent_id");
            Found = true;
        }
    }
    if (!Found) throw winrt::hresult_invalid_argument(L"Snapshot no longer exists.");
    auto Manifest = this->ReadSnapshot(Id);
    // Reparent the visible tree only; never merge or modify frozen disk data.
    for (auto& Record : Records)
        if (Record.at("parent_id") == Id) Record["parent_id"] = Parent;
    if (Catalog.at("active_snapshot") == Id) Catalog["active_snapshot"] = Parent;
    Records.erase(std::remove_if(Records.begin(), Records.end(), [&](auto const& Record)
        { return Record.at("id") == Id; }), Records.end());
    Catalog["garbage"].push_back(this->StorePath(this->m_Root / (Id + ".yaml")));
    Catalog["garbage"].push_back(Manifest.at("configuration").at("GuestStateFile"));
    this->Commit(Catalog, Files::ReadConfiguration(this->m_ConfigurationPath));
}

void NanaBox::SnapshotStore::Collect(nlohmann::json& Catalog)
{
    auto Configuration = Files::ReadConfiguration(this->m_ConfigurationPath);
    auto Key = [&](std::string const& Path)
    {
        auto Text = this->Resolve(Path).wstring();
        std::transform(Text.begin(), Text.end(), Text.begin(), ::towlower);
        return winrt::to_string(Text);
    };
    std::map<std::string, std::string> Parents;
    for (auto const& Disk : Catalog.at("disks"))
        Parents[Key(Disk.at("path"))] = Key(Disk.at("parent"));
    std::set<std::string> Reachable;
    auto Mark = [&](nlohmann::json const& Value)
    {
        for (auto const& Device : Value.value("ScsiDevices", nlohmann::json::array()))
        {
            if (Device.at("Type") != "VirtualDisk") continue;
            std::string Path = Key(Device.at("Path"));
            std::set<std::string> Chain;
            while (Parents.count(Path))
            {
                if (!Chain.insert(Path).second)
                    throw winrt::hresult_invalid_argument(L"Snapshot disk graph contains a cycle.");
                Reachable.insert(Path);
                Path = Parents.at(Path);
            }
        }
    };
    Mark(Configuration);
    for (auto const& Record : Catalog.at("snapshots"))
        Mark(this->ReadSnapshot(Record.at("id")).at("configuration"));

    bool Changed = false;
    auto& Disks = Catalog.at("disks");
    for (auto const& Disk : Disks)
    {
        if (!Reachable.count(Key(Disk.at("path"))))
        {
            Catalog["garbage"].push_back(Disk.at("path"));
            Changed = true;
        }
    }
    Disks.erase(std::remove_if(Disks.begin(), Disks.end(), [&](auto const& Disk)
        { return !Reachable.count(Key(Disk.at("path"))); }), Disks.end());
    std::set<std::string> StatePaths;
    auto MarkState = [&](nlohmann::json const& Value)
    {
        for (auto Name : { "GuestStateFile", "RuntimeStateFile", "SaveStateFile" })
            if (!Value.value(Name, std::string()).empty())
                StatePaths.insert(Key(Value.at(Name)));
    };
    MarkState(Configuration);
    for (auto const& Record : Catalog.at("snapshots"))
        MarkState(this->ReadSnapshot(Record.at("id")).at("configuration"));
    if (Catalog.contains("state_files"))
    {
        auto& States = Catalog.at("state_files");
        for (auto const& Path : States)
        {
            if (!StatePaths.count(Key(Path)))
            {
                Catalog["garbage"].push_back(Path);
                Changed = true;
            }
        }
        States.erase(std::remove_if(States.begin(), States.end(), [&](auto const& Path)
            { return !StatePaths.count(Key(Path)); }), States.end());
    }
    if (Changed) Files::WriteYaml(this->m_Root / L"catalog.yaml", Catalog);

    auto& Garbage = Catalog.at("garbage");
    if (Garbage.empty()) return;
    auto Pending = nlohmann::json::array();
    for (auto const& Entry : Garbage)
    {
        auto Path = this->Resolve(Entry.get<std::string>());
        // Only delete explicitly tracked files directly inside our store.
        if (Path.parent_path() != this->m_Root.lexically_normal() ||
            std::filesystem::is_symlink(Path))
            throw winrt::hresult_invalid_argument(L"Unsafe snapshot cleanup path.");
        Files::ValidateId(winrt::to_string(Path.filename().wstring()).substr(0, 36));
        if (Reachable.count(Key(Entry)) || StatePaths.count(Key(Entry)))
        {
            Pending.push_back(Entry);
            continue;
        }
        try { Files::Remove(Path); }
        catch (...)
        {
            Files::Log("garbage collection", winrt::hresult_error(winrt::to_hresult()));
            Pending.push_back(Entry);
        }
    }
    Garbage = Pending;
    Files::WriteYaml(this->m_Root / L"catalog.yaml", Catalog);
}
