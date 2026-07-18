#include "EditorSettings.h"

#include "json.hpp"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <system_error>

#ifdef _WIN32
#  include <windows.h>
#endif

using json = nlohmann::json;

namespace rt2::core {

// ----------------------------------------------------------------------------
// Path helpers
// ----------------------------------------------------------------------------

std::filesystem::path EditorSettingsStore::Normalize(const std::filesystem::path& p)
{
    if (p.empty()) return {};
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::weakly_canonical(p, ec);
    if (ec) abs = p;
    // Strip trailing separator for stable comparison/storage.
    std::string s = abs.generic_u8string();
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return std::filesystem::u8path(s);
}

std::string EditorSettingsStore::FoldKey(const std::filesystem::path& p)
{
    std::string s = Normalize(p).generic_u8string();
#ifdef _WIN32
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(::tolower(c)); });
#endif
    return s;
}

// ----------------------------------------------------------------------------
// Construction
// ----------------------------------------------------------------------------

EditorSettingsStore::EditorSettingsStore(std::filesystem::path appDataRoot,
                                         size_t maxRecents)
    : m_AppDataRoot(std::move(appDataRoot))
    , m_MaxRecents(maxRecents)
{
}

std::filesystem::path EditorSettingsStore::SettingsFilePath() const
{
    return m_AppDataRoot / "settings.json";
}

// ----------------------------------------------------------------------------
// Load
// ----------------------------------------------------------------------------

bool EditorSettingsStore::Load(Error& err)
{
    err = Error{};
    // Defaults remain in place; only overwrite on a successful parse.
    std::filesystem::path fp = SettingsFilePath();
    if (!std::filesystem::exists(fp))
        return true; // missing file is not an error

    std::ifstream in(fp, std::ios::binary);
    if (!in)
    {
        err.code = Error::Io;
        err.path = fp.string();
        err.detail = "failed to open settings file for reading";
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string content = ss.str();

    json root;
    try
    {
        root = json::parse(content);
    }
    catch (const std::exception& e)
    {
        err.code = Error::Parse;
        err.path = fp.string();
        err.detail = std::string("settings JSON parse error: ") + e.what();
        return false;
    }

    if (!root.contains("version") || !root["version"].is_number_unsigned())
    {
        err.code = Error::Parse;
        err.path = fp.string();
        err.detail = "settings missing or invalid version field";
        return false;
    }
    uint32_t version = root["version"].get<uint32_t>();
    if (version != SettingsVersion)
    {
        err.code = Error::SchemaVersion;
        err.path = fp.string();
        err.detail = "unsupported settings version " + std::to_string(version) +
                     " (supported " + std::to_string(SettingsVersion) + ")";
        return false;
    }

    std::filesystem::path parsedProjectRoot;
    std::vector<std::filesystem::path> parsedRecents;

    // projectRoot — optional. Unknown fields ignored.
    if (root.contains("projectRoot") && root["projectRoot"].is_string())
    {
        std::string s = root["projectRoot"].get<std::string>();
        parsedProjectRoot = s.empty() ? std::filesystem::path{}
                                      : Normalize(std::filesystem::u8path(s));
    }

    // recentScenes — optional array of strings.
    if (root.contains("recentScenes") && root["recentScenes"].is_array())
    {
        for (const auto& entry : root["recentScenes"])
        {
            if (!entry.is_string() || entry.get<std::string>().empty()) continue;
            const auto normalized = Normalize(
                std::filesystem::u8path(entry.get<std::string>()));
            const auto key = FoldKey(normalized);
            const bool duplicate = std::any_of(
                parsedRecents.begin(), parsedRecents.end(),
                [&](const std::filesystem::path& existing) {
                    return FoldKey(existing) == key;
                });
            if (!duplicate && parsedRecents.size() < m_MaxRecents)
                parsedRecents.push_back(normalized);
        }
    }

    m_ProjectRoot = std::move(parsedProjectRoot);
    m_RecentScenes = std::move(parsedRecents);

    return true;
}

// ----------------------------------------------------------------------------
// Save (atomic)
// ----------------------------------------------------------------------------

bool EditorSettingsStore::Save(Error& err) const
{
    err = Error{};
    std::filesystem::path fp = SettingsFilePath();

    // Ensure directory exists.
    std::error_code ec;
    std::filesystem::create_directories(m_AppDataRoot, ec);
    if (ec)
    {
        err.code = Error::Io;
        err.path = m_AppDataRoot.string();
        err.detail = "failed to create settings directory: " + ec.message();
        return false;
    }

    json root;
    root["version"] = SettingsVersion;
    root["projectRoot"] = m_ProjectRoot.empty() ? std::string{} : m_ProjectRoot.generic_u8string();

    json recents = json::array();
    for (const auto& p : m_RecentScenes)
        recents.push_back(p.generic_u8string());
    root["recentScenes"] = recents;

    std::string content = root.dump(2);

    std::filesystem::path tmp = fp;
    tmp += ".tmp";

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            err.code = Error::Io;
            err.path = tmp.string();
            err.detail = "failed to open settings temp file for writing";
            return false;
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!out)
        {
            err.code = Error::Io;
            err.path = tmp.string();
            err.detail = "failed while writing settings temp file";
            out.close();
            std::filesystem::remove(tmp, ec);
            return false;
        }
        out.close();
    }

#ifdef _WIN32
    std::wstring wTarget = fp.wstring();
    std::wstring wTmp    = tmp.wstring();
    if (std::filesystem::exists(fp))
    {
        if (!ReplaceFileW(wTarget.c_str(), wTmp.c_str(), nullptr,
                          REPLACEFILE_WRITE_THROUGH, nullptr, nullptr))
        {
            if (!MoveFileExW(wTmp.c_str(), wTarget.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                err.code = Error::Io;
                err.path = fp.string();
                err.detail = "settings ReplaceFileW and MoveFileExW both failed";
                std::filesystem::remove(tmp, ec);
                return false;
            }
        }
    }
    else
    {
        if (!MoveFileExW(wTmp.c_str(), wTarget.c_str(), MOVEFILE_WRITE_THROUGH))
        {
            err.code = Error::Io;
            err.path = fp.string();
            err.detail = "settings MoveFileExW failed for new file";
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
#else
    std::filesystem::rename(tmp, fp, ec);
    if (ec)
    {
        err.code = Error::Io;
        err.path = fp.string();
        err.detail = "settings rename failed: " + ec.message();
        std::filesystem::remove(tmp, ec);
        return false;
    }
#endif

    return true;
}

// ----------------------------------------------------------------------------
// Recents
// ----------------------------------------------------------------------------

void EditorSettingsStore::AddRecentScene(const std::filesystem::path& path)
{
    if (path.empty()) return;
    std::filesystem::path norm = Normalize(path);
    std::string key = FoldKey(norm);

    // Remove any existing equivalent entry.
    for (auto it = m_RecentScenes.begin(); it != m_RecentScenes.end(); ++it)
    {
        if (FoldKey(*it) == key)
        {
            m_RecentScenes.erase(it);
            break;
        }
    }

    m_RecentScenes.insert(m_RecentScenes.begin(), norm);
    if (m_RecentScenes.size() > m_MaxRecents)
        m_RecentScenes.resize(m_MaxRecents);
}

void EditorSettingsStore::RemoveRecentScene(const std::filesystem::path& path)
{
    std::string key = FoldKey(path);
    for (auto it = m_RecentScenes.begin(); it != m_RecentScenes.end(); ++it)
    {
        if (FoldKey(*it) == key)
        {
            m_RecentScenes.erase(it);
            return;
        }
    }
}

} // namespace rt2::core
