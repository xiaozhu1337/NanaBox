#pragma once

#include <Windows.h>
#include <Mile.Json.h>
#include <winrt/base.h>

#include <filesystem>
#include <string>
#include <vector>

namespace NanaBox
{
    struct SnapshotSummary
    {
        std::string Id;
        std::string ParentId;
        std::string Name;
        std::string Description;
        std::string CreatedUtc;
        bool Current = false;
    };

    struct SnapshotPageResult
    {
        std::vector<SnapshotSummary> Items;
        std::size_t Total = 0;
    };

    // All mutation methods require a stopped VM with its HCS handle closed.
    class SnapshotStore
    {
    public:
        explicit SnapshotStore(std::filesystem::path const& ConfigurationPath);

        void Recover();
        SnapshotPageResult List(std::size_t Page, std::size_t PageSize = 20);
        void Validate(nlohmann::json const& Configuration);
        void Create(nlohmann::json const& Configuration,
            std::string const& Name, std::string const& Description);
        void Restore(std::string const& Id);
        void Delete(std::string const& Id);

    private:
        std::filesystem::path m_ConfigurationPath;
        std::filesystem::path m_Root;

        winrt::file_handle Lock();
        nlohmann::json LoadCatalog();
        void RecoverLocked();
        void Prepare(std::filesystem::path const& Path,
            std::vector<std::filesystem::path>& Prepared);
        void Commit(nlohmann::json const& Catalog,
            nlohmann::json const& Configuration, bool* Published = nullptr);
        void Collect(nlohmann::json& Catalog);
        nlohmann::json ReadSnapshot(std::string const& Id);
        std::filesystem::path Resolve(std::string const& Path) const;
        std::string StorePath(std::filesystem::path const& Path) const;
        void CreateChildren(nlohmann::json& Configuration,
            nlohmann::json& Catalog, std::string const& Id,
            std::vector<std::filesystem::path>& Prepared);
    };
}
