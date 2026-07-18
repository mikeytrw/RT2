#pragma once

#ifndef FILE_DIALOG_H
#define FILE_DIALOG_H

#include <filesystem>
#include <string>

namespace FileDialog {

// Windows implementations use the UTF-16 common-item APIs and return UTF-8.
// `filter` is the standard double-NUL-terminated Windows filter string.
std::string OpenFile(const wchar_t* filter,
                     const std::filesystem::path& initialDirectory = {});
std::string SaveFile(const wchar_t* filter,
                     const std::filesystem::path& initialDirectory = {});
std::string OpenFolder(const std::filesystem::path& initialDirectory = {});

} // namespace FileDialog

#endif // FILE_DIALOG_H
