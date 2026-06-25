#include "FileDialog.h"

#ifdef WL_PLATFORM_WINDOWS
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#endif

namespace FileDialog {

std::string OpenFile(const char* filter)
{
#ifdef WL_PLATFORM_WINDOWS
	OPENFILENAMEA ofn;
	ZeroMemory(&ofn, sizeof(ofn));
	char szFile[260];
	ZeroMemory(szFile, sizeof(szFile));

	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = nullptr;
	ofn.lpstrFile = szFile;
	ofn.nMaxFile = sizeof(szFile);
	ofn.lpstrFilter = filter;
	ofn.nFilterIndex = 1;
	ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

	if (GetOpenFileNameA(&ofn))
		return std::string(ofn.lpstrFile);
	return std::string();
#else
	return std::string();
#endif
}

std::string SaveFile(const char* filter)
{
#ifdef WL_PLATFORM_WINDOWS
	OPENFILENAMEA ofn;
	ZeroMemory(&ofn, sizeof(ofn));
	char szFile[260];
	ZeroMemory(szFile, sizeof(szFile));

	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = nullptr;
	ofn.lpstrFile = szFile;
	ofn.nMaxFile = sizeof(szFile);
	ofn.lpstrFilter = filter;
	ofn.nFilterIndex = 1;
	ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;

	if (GetSaveFileNameA(&ofn))
		return std::string(ofn.lpstrFile);
	return std::string();
#else
	return std::string();
#endif
}

} // namespace FileDialog