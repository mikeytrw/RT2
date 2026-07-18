#include "FileDialog.h"

#ifdef WL_PLATFORM_WINDOWS
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <vector>
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Comdlg32.lib")
#endif

namespace FileDialog {

#ifdef WL_PLATFORM_WINDOWS
namespace {

std::string WideToUtf8(const wchar_t* value)
{
    if (!value || !*value) return {};
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                                   nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                        result.data(), size, nullptr, nullptr);
    result.pop_back(); // remove the terminator written by WideCharToMultiByte
    return result;
}

std::string RunLegacyFileDialog(const wchar_t* filter,
                                const std::filesystem::path& initialDirectory,
                                bool save)
{
    std::vector<wchar_t> fileBuffer(32768, L'\0');
    std::wstring initial = initialDirectory.empty() ? std::wstring{}
                                                     : initialDirectory.wstring();

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFile = fileBuffer.data();
    ofn.nMaxFile = static_cast<DWORD>(fileBuffer.size());
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrInitialDir = initial.empty() ? nullptr : initial.c_str();
    ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;
    if (!save) ofn.Flags |= OFN_FILEMUSTEXIST;
    else ofn.Flags |= OFN_OVERWRITEPROMPT;

    const BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    return ok ? WideToUtf8(fileBuffer.data()) : std::string{};
}

} // namespace
#endif

std::string OpenFile(const wchar_t* filter,
                     const std::filesystem::path& initialDirectory)
{
#ifdef WL_PLATFORM_WINDOWS
    return RunLegacyFileDialog(filter, initialDirectory, false);
#else
    (void)filter;
    (void)initialDirectory;
    return {};
#endif
}

std::string SaveFile(const wchar_t* filter,
                     const std::filesystem::path& initialDirectory)
{
#ifdef WL_PLATFORM_WINDOWS
    return RunLegacyFileDialog(filter, initialDirectory, true);
#else
    (void)filter;
    (void)initialDirectory;
    return {};
#endif
}

std::string OpenFolder(const std::filesystem::path& initialDirectory)
{
#ifdef WL_PLATFORM_WINDOWS
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize = SUCCEEDED(init);

    IFileOpenDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog)
    {
        if (uninitialize) CoUninitialize();
        return {};
    }

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options)))
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                           FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
    dialog->SetTitle(L"Select Project Root");

    if (!initialDirectory.empty())
    {
        IShellItem* initialItem = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(initialDirectory.wstring().c_str(),
                                                  nullptr, IID_PPV_ARGS(&initialItem))))
        {
            dialog->SetFolder(initialItem);
            initialItem->Release();
        }
    }

    std::string result;
    if (SUCCEEDED(dialog->Show(nullptr)))
    {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item)
        {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path)
            {
                result = WideToUtf8(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }

    dialog->Release();
    if (uninitialize) CoUninitialize();
    return result;
#else
    (void)initialDirectory;
    return {};
#endif
}

} // namespace FileDialog
