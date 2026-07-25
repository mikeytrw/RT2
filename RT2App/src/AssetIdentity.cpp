#include "AssetIdentity.h"

#include <fstream>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#  define NOMINMAX
#  include <windows.h>
#endif

namespace rt2::core {

std::filesystem::path AssetSidecarPath(const std::filesystem::path& assetPath)
{
    // Append ".rt2meta" to the full filename (preserves extension): "cube.glb"
    // -> "cube.glb.rt2meta". generic_u8string keeps forward slashes portable.
    std::filesystem::path p = assetPath;
    p += ".rt2meta";
    return p;
}

UUID ReadSidecarId(const std::filesystem::path& sidecarPath, Error& err)
{
    err = Error{};
    std::error_code ec;
    if (!std::filesystem::exists(sidecarPath, ec))
        return UUID::Nil(); // absent is normal; not an error

    std::ifstream in(sidecarPath);
    if (!in)
    {
        err.code = Error::Io;
        err.path = sidecarPath.string();
        err.detail = "sidecar exists but is unreadable";
        return UUID::Nil();
    }

    std::string line;
    if (!std::getline(in, line))
    {
        err.code = Error::Parse;
        err.path = sidecarPath.string();
        err.detail = "sidecar is empty";
        return UUID::Nil();
    }

    // Trim trailing whitespace/newline so "id\n" and "id" both parse.
    while (!line.empty() &&
           (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
        line.pop_back();

    const UUID id = UUID::Parse(line);
    if (id.IsNull())
    {
        err.code = Error::Parse;
        err.path = sidecarPath.string();
        err.detail = "sidecar does not contain a valid UUID: \"" + line + "\"";
        return UUID::Nil();
    }
    return id;
}

bool WriteSidecarId(const std::filesystem::path& sidecarPath,
                    const UUID& id, Error& err)
{
    err = Error{};
    if (id.IsNull())
    {
        err.code = Error::InvalidArgument;
        err.detail = "cannot write a nil asset ID to a sidecar";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(sidecarPath.parent_path(), ec);
    if (ec)
    {
        err.code = Error::Io;
        err.path = sidecarPath.parent_path().string();
        err.detail = "failed to create sidecar directory: " + ec.message();
        return false;
    }

    // Atomic write: temp sibling, then MoveFileExW/ReplaceFileW on Windows.
    std::filesystem::path tmp = sidecarPath;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            err.code = Error::Io;
            err.path = tmp.string();
            err.detail = "failed to open sidecar temp file for writing";
            return false;
        }
        out << id.ToString() << "\n";
        out.flush();
        if (!out)
        {
            err.code = Error::Io;
            err.path = tmp.string();
            err.detail = "failed while writing sidecar temp file";
            out.close();
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }

#ifdef _WIN32
    auto toWide = [](const std::filesystem::path& p) { return p.wstring(); };
    std::wstring wTarget = toWide(sidecarPath);
    std::wstring wTmp = toWide(tmp);
    if (std::filesystem::exists(sidecarPath, ec))
    {
        BOOL ok = ReplaceFileW(wTarget.c_str(), wTmp.c_str(), nullptr,
                               REPLACEFILE_WRITE_THROUGH, nullptr, nullptr);
        if (!ok)
        {
            if (!MoveFileExW(wTmp.c_str(), wTarget.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                err.code = Error::Io;
                err.path = sidecarPath.string();
                err.detail = "failed to replace sidecar atomically";
                std::filesystem::remove(tmp, ec);
                return false;
            }
        }
    }
    else
    {
        if (!MoveFileExW(wTmp.c_str(), wTarget.c_str(),
                        MOVEFILE_WRITE_THROUGH))
        {
            err.code = Error::Io;
            err.path = sidecarPath.string();
            err.detail = "failed to place sidecar";
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
#else
    std::filesystem::rename(tmp, sidecarPath, ec);
    if (ec)
    {
        err.code = Error::Io;
        err.path = sidecarPath.string();
        err.detail = "failed to rename sidecar temp: " + ec.message();
        std::filesystem::remove(tmp, ec);
        return false;
    }
#endif
    return true;
}

UUID ResolveOrAssign(const std::filesystem::path& assetPath,
                      IUuidProvider& provider,
                      bool& minted,
                      Error& err)
{
    err = Error{};
    minted = false;
    const auto sidecar = AssetSidecarPath(assetPath);

    Error readErr;
    const UUID existing = ReadSidecarId(sidecar, readErr);
    if (readErr.IsOk() && !existing.IsNull())
        return existing; // stable ID from committed sidecar

    // Absent or malformed: mint a fresh v4 and (re)write the sidecar.
    const UUID fresh = provider.CreateV4();
    Error writeErr;
    if (!WriteSidecarId(sidecar, fresh, writeErr))
    {
        // A write failure is reported but does NOT block the import: the
        // scene still gets an ID for this session; the next successful save
        // will retry the sidecar write. Surface both errors if both failed.
        if (!readErr.IsOk())
        {
            err = readErr; // malformed sidecar is the more interesting cause
            err.detail += "; then write failed: " + writeErr.detail;
        }
        else
        {
            err = writeErr;
        }
        minted = true;
        return fresh;
    }

    if (!readErr.IsOk())
    {
        // Malformed sidecar was overwritten with a valid one. Report the
        // parse failure so it surfaces as a diagnostic, but the asset now
        // has a stable written ID.
        err = readErr;
    }
    minted = true;
    return fresh;
}

} // namespace rt2::core