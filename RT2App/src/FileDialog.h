#pragma once

#ifndef FILE_DIALOG_H
#define FILE_DIALOG_H

#include <string>

namespace FileDialog {

// Returns empty string if user cancels.
std::string OpenFile(const char* filter);

}

#endif // !FILE_DIALOG_H