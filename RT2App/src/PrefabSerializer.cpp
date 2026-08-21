#include "PrefabSerializer.h"

#include "json.hpp"

#include <fstream>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using json = nlohmann::json;

namespace rt2::core {

namespace {

constexpr const char* kPrefabHeader = "rt2prefab";

} // namespace

bool PrefabSerializer::WriteBytesAtomic(const std::filesystem::path& path,
                                        const std::string& content,
                                        Error& err)
{
    err = Error{};
    std::filesystem::path tmpPath = path;
    tmpPath += ".tmp";
    {
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            err.code = Error::Io;
            err.path = tmpPath.string();
            err.detail = "failed to open temp file for writing";
            return false;
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
        if (!out)
        {
            err.code = Error::Io;
            err.path = tmpPath.string();
            err.detail = "failed to write prefab temp file";
            std::error_code ec;
            std::filesystem::remove(tmpPath, ec);
            return false;
        }
    }
#ifdef _WIN32
    const std::wstring wTmp = tmpPath.wstring();
    const std::wstring wTarget = path.wstring();
    if (!MoveFileExW(wTmp.c_str(), wTarget.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        err.code = Error::Io;
        err.path = path.string();
        err.detail = "failed to replace prefab file atomically";
        std::error_code ec;
        std::filesystem::remove(tmpPath, ec);
        return false;
    }
#else
    std::error_code ec;
    std::filesystem::rename(tmpPath, path, ec);
    if (ec)
    {
        err.code = Error::Io;
        err.path = path.string();
        err.detail = "failed to rename prefab temp: " + ec.message();
        std::filesystem::remove(tmpPath, ec);
        return false;
    }
#endif
    return true;
}

bool PrefabSerializer::Serialize(const PrefabDocument& doc,
                                 std::string& content,
                                 Error& err)
{
    err = Error{};
    if (doc.version != FormatVersion)
    {
        err.code = Error::InvalidArgument;
        err.detail = "cannot write prefab format version " +
                     std::to_string(doc.version) + " (supported " +
                     std::to_string(FormatVersion) + ")";
        return false;
    }

    json root;
    root["header"]  = kPrefabHeader;
    root["version"] = doc.version;
    json entityArray = json::array();
    // The record codec emits advisory asset diagnostics (e.g. absolute
    // paths); PrefabSerializer::Save has no diagnostics channel (W1 —
    // recorded in the W1 report). Loud failures still propagate via err.
    std::vector<rt2::core::AssetDiagnostic> droppedDiagnostics;
    for (const auto& record : doc.entities)
    {
        json j;
        if (!PrefabRecordToJson(record, droppedDiagnostics, err, j))
        {
            err.path = ":" + err.path;
            return false;
        }
        entityArray.push_back(std::move(j));
    }
    root["entities"] = std::move(entityArray);

    try
    {
        content = root.dump(2);
    }
    catch (const std::exception& e)
    {
        err.code = Error::InvalidArgument;
        err.detail = std::string("prefab contains text that cannot be "
                                 "serialized: ") + e.what();
        return false;
    }
    return true;
}

bool PrefabSerializer::Save(const PrefabDocument& doc,
                            const std::filesystem::path& path,
                            Error& err)
{
    err = Error{};
    std::string content;
    if (!PrefabSerializer::Serialize(doc, content, err))
    {
        err.path = path.string() + err.path;
        return false;
    }
    return PrefabSerializer::WriteBytesAtomic(path, content, err);
}

bool PrefabSerializer::Load(PrefabDocument& doc,
                            const std::filesystem::path& path,
                            Error& err)
{
    err = Error{};
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        err.code = Error::Io;
        err.path = path.string();
        err.detail = "failed to open prefab file";
        return false;
    }
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    if (in.bad())
    {
        err.code = Error::Io;
        err.path = path.string();
        err.detail = "failed to read prefab file";
        return false;
    }
    return LoadBytes(doc, content, path, err);
}

bool PrefabSerializer::LoadBytes(PrefabDocument& doc,
                                 const std::string& content,
                                 const std::filesystem::path& path,
                                 Error& err)
{
    err = Error{};
    json root;
    try
    {
        root = json::parse(content);
    }
    catch (const std::exception& e)
    {
        err.code = Error::Parse;
        err.path = path.string();
        err.detail = std::string("JSON parse error: ") + e.what();
        return false;
    }

    if (!root.contains("header") || !root["header"].is_string() ||
        root["header"].get<std::string>() != kPrefabHeader)
    {
        err.code = Error::Parse;
        err.path = path.string();
        err.detail = "not an rt2prefab file (missing or wrong header)";
        return false;
    }

    if (!root.contains("version") || !root["version"].is_number_unsigned())
    {
        err.code = Error::Parse;
        err.path = path.string();
        err.detail = "missing or invalid prefab version field";
        return false;
    }
    const uint32_t version = root["version"].get<uint32_t>();
    if (version != FormatVersion)
    {
        err.code = Error::SchemaVersion;
        err.path = path.string();
        err.detail = "unsupported prefab format version " +
                     std::to_string(version) + " (supported " +
                     std::to_string(FormatVersion) + ")";
        return false;
    }

    if (!root.contains("entities") || !root["entities"].is_array())
    {
        err.code = Error::Parse;
        err.path = path.string();
        err.detail = "missing or invalid prefab entities array";
        return false;
    }

    // Parse every record before touching `doc` (transactional load).
    PrefabDocument parsed;
    parsed.version = version;
    parsed.entities.reserve(root["entities"].size());
    for (const auto& j : root["entities"])
    {
        PrefabEntityRecord record;
        if (!JsonToPrefabRecord(j, err, record))
        {
            err.path = path.string() + ":" + err.path;
            return false;
        }
        parsed.entities.push_back(std::move(record));
    }

    doc.version = parsed.version;
    doc.entities = std::move(parsed.entities);
    return true;
}

} // namespace rt2::core
