#include "EditorSettings.h"

#include "json.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#  include <windows.h>
#endif

namespace rt2::core {

using json = nlohmann::json;

std::filesystem::path EditorSettingsStore::Normalize(
    const std::filesystem::path& path)
{
    if (path.empty()) return {};
    std::error_code ec;
    auto normalized = std::filesystem::weakly_canonical(path, ec);
    if (ec) normalized = path.lexically_normal();
    std::string value = normalized.generic_u8string();
    while (value.size() > 1 && value.back() == '/') value.pop_back();
    return std::filesystem::u8path(value);
}

std::string EditorSettingsStore::FoldKey(
    const std::filesystem::path& path)
{
    std::string value = Normalize(path).generic_u8string();
#ifdef _WIN32
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(::tolower(c)); });
#endif
    return value;
}

EditorSettingsStore::EditorSettingsStore(std::filesystem::path appDataRoot,
                                         size_t maxRecents)
    : m_AppDataRoot(std::move(appDataRoot)), m_MaxRecents(maxRecents)
{
}

std::filesystem::path EditorSettingsStore::SettingsFilePath() const
{
    return m_AppDataRoot / "settings.json";
}

bool EditorSettingsStore::Load(Error& err)
{
    EditorSettingsLoadReport ignored;
    return Load(ignored, err);
}

bool EditorSettingsStore::Load(EditorSettingsLoadReport& report, Error& err)
{
    err = Error{};
    report = EditorSettingsLoadReport{};
    const auto file = SettingsFilePath();
    if (!std::filesystem::exists(file)) return true;

    std::ifstream input(file, std::ios::binary);
    if (!input)
    {
        err.code = Error::Io;
        err.path = file.u8string();
        err.detail = "failed to open settings file for reading";
        return false;
    }
    std::stringstream stream;
    stream << input.rdbuf();

    json root;
    try
    {
        root = json::parse(stream.str());
    }
    catch (const std::exception& exception)
    {
        err.code = Error::Parse;
        err.path = file.u8string();
        err.detail = std::string("settings JSON parse error: ") +
                     exception.what();
        return false;
    }
    if (!root.is_object() || !root.contains("version") ||
        !root["version"].is_number_unsigned())
    {
        err.code = Error::Parse;
        err.path = file.u8string();
        err.detail = "settings missing or invalid version field";
        return false;
    }

    const uint32_t version = root["version"].get<uint32_t>();
    report.sourceVersion = version;
    report.migrated = version != SettingsVersion;
    if (version < kOldestSupportedVersion || version > SettingsVersion)
    {
        err.code = Error::SchemaVersion;
        err.path = file.u8string();
        err.detail = "unsupported settings version " +
                     std::to_string(version) + " (supported 1-" +
                     std::to_string(SettingsVersion) + ")";
        return false;
    }

    const char* browseKey = version >= 3
        ? "lastBrowseDirectory" : "projectRoot";
    std::filesystem::path parsedBrowse;
    if (root.contains(browseKey))
    {
        if (!root[browseKey].is_string())
        {
            err.code = Error::Parse;
            err.path = file.u8string();
            err.detail = std::string(browseKey) + " must be a string";
            return false;
        }
        const std::string value = root[browseKey].get<std::string>();
        if (!value.empty())
            parsedBrowse = Normalize(std::filesystem::u8path(value));
    }

    std::vector<std::filesystem::path> parsedRecents;
    if (root.contains("recentScenes"))
    {
        if (!root["recentScenes"].is_array())
        {
            err.code = Error::Parse;
            err.path = file.u8string();
            err.detail = "recentScenes must be an array";
            return false;
        }
        for (const auto& entry : root["recentScenes"])
        {
            if (!entry.is_string() || entry.get<std::string>().empty()) continue;
            const auto normalized = Normalize(
                std::filesystem::u8path(entry.get<std::string>()));
            const auto key = FoldKey(normalized);
            const bool duplicate = std::any_of(
                parsedRecents.begin(), parsedRecents.end(),
                [&](const auto& existing) { return FoldKey(existing) == key; });
            if (!duplicate && parsedRecents.size() < m_MaxRecents)
                parsedRecents.push_back(normalized);
        }
    }

    std::vector<InputContextRecord> parsedOverrides;
    if (version == 2 && root.contains("inputContexts"))
    {
        std::vector<InputContextRecord> legacy;
        Error inputError;
        if (!ParseInputContextRecords(root["inputContexts"],
                                      InputConfigScope::UserOverrides,
                                      legacy, inputError))
        {
            err = inputError;
            err.path = file.u8string() + ":" + err.path;
            return false;
        }
        for (auto& record : legacy)
        {
            const bool drop = IsEditorOwnedInputContext(record.contextId);
            if (!drop) parsedOverrides.push_back(record);
            if (record.mappings.empty())
            {
                report.diagnostics.push_back({
                    drop ? EditorSettingsMigrationDiagnostic::Kind::DroppedReservedContext
                         : EditorSettingsMigrationDiagnostic::Kind::PromotedOverride,
                    record.contextId, {}});
            }
            for (const auto& mapping : record.mappings)
            {
                report.diagnostics.push_back({
                    drop ? EditorSettingsMigrationDiagnostic::Kind::DroppedReservedContext
                         : EditorSettingsMigrationDiagnostic::Kind::PromotedOverride,
                    record.contextId, mapping.name});
            }
        }
    }
    else if (version >= 3 && root.contains("inputOverrides"))
    {
        Error inputError;
        if (!ParseInputContextRecords(root["inputOverrides"],
                                      InputConfigScope::UserOverrides,
                                      parsedOverrides, inputError))
        {
            err = inputError;
            err.path = file.u8string() + ":" + err.path;
            return false;
        }
    }
    std::sort(report.diagnostics.begin(), report.diagnostics.end(),
              [](const auto& a, const auto& b) {
                  if (a.kind != b.kind) return a.kind < b.kind;
                  if (a.contextId != b.contextId)
                      return a.contextId < b.contextId;
                  return a.mappingName < b.mappingName;
              });

    m_LastBrowseDirectory = std::move(parsedBrowse);
    m_RecentScenes = std::move(parsedRecents);
    m_InputOverrides = std::move(parsedOverrides);
    return true;
}

bool EditorSettingsStore::Save(Error& err) const
{
    err = Error{};
    const auto file = SettingsFilePath();
    std::error_code ec;
    std::filesystem::create_directories(m_AppDataRoot, ec);
    if (ec)
    {
        err.code = Error::Io;
        err.path = m_AppDataRoot.u8string();
        err.detail = "failed to create settings directory: " + ec.message();
        return false;
    }

    // Reparse through the shared codec before writing so invalid in-memory
    // overrides cannot be persisted silently.
    const json overrideJson = InputContextRecordsToJson(m_InputOverrides);
    std::vector<InputContextRecord> validated;
    Error inputError;
    if (!ParseInputContextRecords(overrideJson,
                                  InputConfigScope::UserOverrides,
                                  validated, inputError))
    {
        err = inputError;
        err.path = file.u8string() + ":" + err.path;
        return false;
    }

    json root;
    root["version"] = SettingsVersion;
    root["lastBrowseDirectory"] = m_LastBrowseDirectory.empty()
        ? std::string{} : m_LastBrowseDirectory.generic_u8string();
    root["recentScenes"] = json::array();
    for (const auto& recent : m_RecentScenes)
        root["recentScenes"].push_back(recent.generic_u8string());
    root["inputOverrides"] = InputContextRecordsToJson(validated);

    const std::string content = root.dump(2);
    auto temp = file;
    temp += ".tmp";
    {
        std::ofstream output(temp, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            err.code = Error::Io;
            err.path = temp.u8string();
            err.detail = "failed to open settings temp file for writing";
            return false;
        }
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        output.flush();
        if (!output)
        {
            output.close();
            std::filesystem::remove(temp, ec);
            err.code = Error::Io;
            err.path = temp.u8string();
            err.detail = "failed while writing settings temp file";
            return false;
        }
    }

#ifdef _WIN32
    const std::wstring target = file.wstring();
    const std::wstring source = temp.wstring();
    if (std::filesystem::exists(file))
    {
        if (!ReplaceFileW(target.c_str(), source.c_str(), nullptr,
                          REPLACEFILE_WRITE_THROUGH, nullptr, nullptr) &&
            !MoveFileExW(source.c_str(), target.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            std::filesystem::remove(temp, ec);
            err.code = Error::Io;
            err.path = file.u8string();
            err.detail = "failed to atomically replace settings file";
            return false;
        }
    }
    else if (!MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH))
    {
        std::filesystem::remove(temp, ec);
        err.code = Error::Io;
        err.path = file.u8string();
        err.detail = "failed to atomically create settings file";
        return false;
    }
#else
    std::filesystem::rename(temp, file, ec);
    if (ec)
    {
        std::filesystem::remove(temp, ec);
        err.code = Error::Io;
        err.path = file.u8string();
        err.detail = "failed to rename settings temp file: " + ec.message();
        return false;
    }
#endif
    return true;
}

void EditorSettingsStore::AddRecentScene(const std::filesystem::path& path)
{
    if (path.empty()) return;
    const auto normalized = Normalize(path);
    const auto key = FoldKey(normalized);
    const auto it = std::find_if(
        m_RecentScenes.begin(), m_RecentScenes.end(),
        [&](const auto& existing) { return FoldKey(existing) == key; });
    if (it != m_RecentScenes.end()) m_RecentScenes.erase(it);
    m_RecentScenes.insert(m_RecentScenes.begin(), normalized);
    if (m_RecentScenes.size() > m_MaxRecents)
        m_RecentScenes.resize(m_MaxRecents);
}

void EditorSettingsStore::RemoveRecentScene(
    const std::filesystem::path& path)
{
    const auto key = FoldKey(path);
    const auto it = std::find_if(
        m_RecentScenes.begin(), m_RecentScenes.end(),
        [&](const auto& existing) { return FoldKey(existing) == key; });
    if (it != m_RecentScenes.end()) m_RecentScenes.erase(it);
}

} // namespace rt2::core
