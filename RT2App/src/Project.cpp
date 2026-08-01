#include "Project.h"

#include "json.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#  include <windows.h>
#endif

namespace rt2::core {

namespace {

using json = nlohmann::json;

bool Fail(Error& err, Error::Code code,
          const std::filesystem::path& path, const std::string& detail)
{
    err.code = code;
    err.path = path.u8string();
    err.detail = detail;
    return false;
}

std::string Fold(std::string value)
{
#ifdef _WIN32
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(::tolower(c)); });
#endif
    return value;
}

bool IsContained(const std::filesystem::path& root,
                 const std::filesystem::path& candidate)
{
    auto rootIt = root.begin();
    auto candidateIt = candidate.begin();
    for (; rootIt != root.end(); ++rootIt, ++candidateIt)
    {
        if (candidateIt == candidate.end() ||
            Fold(rootIt->generic_u8string()) !=
                Fold(candidateIt->generic_u8string()))
            return false;
    }
    return true;
}

bool ValidRelativeLocator(const std::string& locator, bool allowEmpty)
{
    if (locator.empty()) return allowEmpty;
    const auto path = std::filesystem::u8path(locator);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory())
        return false;
    for (const auto& part : path)
    {
        if (part == "..") return false;
    }
    return true;
}

std::filesystem::path CanonicalForContainment(
    const std::filesystem::path& path)
{
    std::error_code ec;
    auto result = std::filesystem::weakly_canonical(path, ec);
    return ec ? std::filesystem::absolute(path, ec).lexically_normal()
              : result.lexically_normal();
}

bool WriteAtomic(const std::filesystem::path& path,
                 const std::string& content,
                 Error& err)
{
    std::error_code ec;
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
        return Fail(err, Error::Io, path.parent_path(),
                    "failed to create project directory: " + ec.message());

    std::filesystem::path temp = path;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out)
            return Fail(err, Error::Io, temp,
                        "failed to open project temp file");
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
        if (!out)
        {
            out.close();
            std::filesystem::remove(temp, ec);
            return Fail(err, Error::Io, temp,
                        "failed while writing project temp file");
        }
    }

#ifdef _WIN32
    const std::wstring target = path.wstring();
    const std::wstring source = temp.wstring();
    if (std::filesystem::exists(path))
    {
        if (!ReplaceFileW(target.c_str(), source.c_str(), nullptr,
                          REPLACEFILE_WRITE_THROUGH, nullptr, nullptr) &&
            !MoveFileExW(source.c_str(), target.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            std::filesystem::remove(temp, ec);
            return Fail(err, Error::Io, path,
                        "failed to atomically replace project file");
        }
    }
    else if (!MoveFileExW(source.c_str(), target.c_str(),
                          MOVEFILE_WRITE_THROUGH))
    {
        std::filesystem::remove(temp, ec);
        return Fail(err, Error::Io, path,
                    "failed to atomically create project file");
    }
#else
    std::filesystem::rename(temp, path, ec);
    if (ec)
    {
        std::filesystem::remove(temp, ec);
        return Fail(err, Error::Io, path,
                    "failed to rename project temp file: " + ec.message());
    }
#endif
    return true;
}

} // namespace

bool ProjectStore::ValidateAndResolve(ProjectDocument& project,
                                      const std::filesystem::path& projectFile,
                                      Error& err)
{
    err = Error{};
    if (project.projectId.IsNull())
        return Fail(err, Error::InvalidArgument, projectFile,
                    "projectId must be a non-nil UUID");
    if (!ValidRelativeLocator(project.assetRootLocator, false))
        return Fail(err, Error::InvalidArgument, projectFile,
                    "assetRoot must be a non-empty relative path without '..'");
    if (!ValidRelativeLocator(project.cacheRootLocator, false))
        return Fail(err, Error::InvalidArgument, projectFile,
                    "cacheRoot must be a non-empty relative path without '..'");
    if (!ValidRelativeLocator(project.startupSceneLocator, true))
        return Fail(err, Error::InvalidArgument, projectFile,
                    "startupScene must be relative to assetRoot without '..'");
    if (!project.startupSceneLocator.empty() &&
        Fold(std::filesystem::u8path(project.startupSceneLocator).
                 extension().u8string()) != ".rt2scene")
        return Fail(err, Error::InvalidArgument, projectFile,
                    "startupScene must have a .rt2scene extension");

    std::error_code ec;
    auto absoluteFile = std::filesystem::absolute(projectFile, ec);
    if (ec)
        return Fail(err, Error::InvalidArgument, projectFile,
                    "failed to make project path absolute: " + ec.message());
    absoluteFile = absoluteFile.lexically_normal();
    const auto projectDirectory = CanonicalForContainment(
        absoluteFile.parent_path());
    const auto assetRoot = CanonicalForContainment(
        projectDirectory / std::filesystem::u8path(project.assetRootLocator));
    const auto cacheRoot = CanonicalForContainment(
        projectDirectory / std::filesystem::u8path(project.cacheRootLocator));
    if (!IsContained(projectDirectory, assetRoot) ||
        !IsContained(projectDirectory, cacheRoot))
        return Fail(err, Error::InvalidArgument, projectFile,
                    "project roots escape the project directory");
    if (IsContained(assetRoot, cacheRoot) || IsContained(cacheRoot, assetRoot))
        return Fail(err, Error::InvalidArgument, projectFile,
                    "assetRoot and cacheRoot must not overlap");

    std::filesystem::path startupScene;
    if (!project.startupSceneLocator.empty())
    {
        startupScene = CanonicalForContainment(
            assetRoot / std::filesystem::u8path(project.startupSceneLocator));
        if (!IsContained(assetRoot, startupScene))
            return Fail(err, Error::InvalidArgument, projectFile,
                        "startupScene escapes assetRoot");
    }

    project.projectFile = absoluteFile;
    project.projectDirectory = projectDirectory;
    project.assetRoot = assetRoot;
    project.cacheRoot = cacheRoot;
    project.startupScene = startupScene;
    return true;
}

bool ProjectStore::Load(const std::filesystem::path& path,
                        ProjectDocument& out,
                        Error& err)
{
    err = Error{};
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return Fail(err, Error::Io, path, "failed to open project file");
    std::stringstream stream;
    stream << input.rdbuf();

    json root;
    try
    {
        root = json::parse(stream.str());
    }
    catch (const std::exception& exception)
    {
        return Fail(err, Error::Parse, path,
                    std::string("project JSON parse error: ") + exception.what());
    }
    if (!root.is_object() || !root.contains("version") ||
        !root["version"].is_number_unsigned())
        return Fail(err, Error::Parse, path,
                    "missing or invalid project version");
    const uint32_t version = root["version"].get<uint32_t>();
    if (version != SchemaVersion)
        return Fail(err, Error::SchemaVersion, path,
                    "unsupported project version " + std::to_string(version));
    if (!root.contains("projectId") || !root["projectId"].is_string())
        return Fail(err, Error::Parse, path,
                    "missing or invalid projectId");

    ProjectDocument parsed;
    parsed.projectId = UUID::Parse(root["projectId"].get<std::string>());
    if (root.contains("assetRoot") && !root["assetRoot"].is_string())
        return Fail(err, Error::Parse, path, "assetRoot must be a string");
    if (root.contains("cacheRoot") && !root["cacheRoot"].is_string())
        return Fail(err, Error::Parse, path, "cacheRoot must be a string");
    if (root.contains("startupScene") && !root["startupScene"].is_string())
        return Fail(err, Error::Parse, path, "startupScene must be a string");
    parsed.assetRootLocator = root.value("assetRoot", std::string("Assets"));
    parsed.cacheRootLocator = root.value("cacheRoot", std::string(".rt2/cache"));
    parsed.startupSceneLocator = root.value("startupScene", std::string{});

    if (root.contains("inputContexts"))
    {
        Error inputErr;
        if (!ParseInputContextRecords(root["inputContexts"],
                                      InputConfigScope::ProjectDefaults,
                                      parsed.inputContexts, inputErr))
        {
            err = inputErr;
            err.path = path.u8string() + ":" + err.path;
            return false;
        }
    }

    if (!ValidateAndResolve(parsed, path, err)) return false;
    out = std::move(parsed);
    return true;
}

bool ProjectStore::Save(const ProjectDocument& project,
                        const std::filesystem::path& path,
                        Error& err)
{
    ProjectDocument validated = project;
    if (!ValidateAndResolve(validated, path, err)) return false;

    // Validate input independently of whatever order the caller supplied.
    std::vector<InputContextRecord> checked;
    Error inputErr;
    const auto inputJson = InputContextRecordsToJson(project.inputContexts);
    if (!ParseInputContextRecords(inputJson,
                                  InputConfigScope::ProjectDefaults,
                                  checked, inputErr))
    {
        err = inputErr;
        err.path = path.u8string() + ":" + err.path;
        return false;
    }

    json root;
    root["version"] = SchemaVersion;
    root["projectId"] = project.projectId.ToString();
    root["assetRoot"] = project.assetRootLocator;
    root["cacheRoot"] = project.cacheRootLocator;
    root["startupScene"] = project.startupSceneLocator;
    root["inputContexts"] = InputContextRecordsToJson(checked);
    return WriteAtomic(path, root.dump(2), err);
}

} // namespace rt2::core
