#pragma once

#include <Windows.h>
#include <Mile.Json.h>
#include <winrt/base.h>
#include <filesystem>
#include <string>
#include <vector>

namespace NanaBox::SnapshotFiles
{
    nlohmann::json ReadYaml(std::filesystem::path const& Path);
    void WriteYaml(std::filesystem::path const& Path,
        nlohmann::json const& Document);
    nlohmann::json ReadConfiguration(std::filesystem::path const& Path);
    void WriteConfiguration(std::filesystem::path const& Path,
        nlohmann::json const& Configuration);
    void AtomicWrite(std::filesystem::path const& Path,
        std::string const& Content);
    void CopyState(std::filesystem::path const& Source,
        std::filesystem::path const& Destination);
    void Remove(std::filesystem::path const& Path);
    void Cleanup(std::vector<std::filesystem::path> const& Paths) noexcept;
    std::string NewId();
    std::string UtcNow();
    void ValidateId(std::string const& Id);
    void ValidateDisk(std::filesystem::path const& Path);
    void CreateChild(std::filesystem::path const& Path,
        std::filesystem::path const& Parent);
    void Log(std::string const& Operation,
        winrt::hresult_error const& Error) noexcept;
}
