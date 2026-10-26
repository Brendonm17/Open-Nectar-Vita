/**
 * @file pc_file_dialog.cpp
 * @brief Native open-file dialog for the desktop game binary (the launcher
 * has its own copy of this logic; the game cannot link the launcher).
 */
#include "pc_file_dialog.h"
#include "pc_dialog_command.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Same prefix as the launcher's dialogs (launcher_platform_*.cpp).
static std::string dialogTitle(const char* title)
{
    return std::string("Open Nectar - ") + title;
}

#if defined(_WIN32)
#include <windows.h>
#include <commdlg.h>

static std::wstring widen(const char* s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring out(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, &out[0], n);
    return out;
}

bool pc_file_dialog_open(const char* title, const char* filterName, const char* patterns, char* out, size_t outSize)
{
    if (!out || outSize == 0) return false;
    out[0] = '\0';
    // "Name\0*.zip;*.rar\0All files\0*.*\0\0"
    std::wstring pat = widen(patterns);
    for (wchar_t& c : pat) if (c == L' ') c = L';';
    std::wstring filter = widen(filterName);
    filter.push_back(L'\0');
    filter += pat;
    filter.push_back(L'\0');
    filter += L"All files";
    filter.push_back(L'\0');
    filter += L"*.*";
    filter.push_back(L'\0');
    filter.push_back(L'\0');
    std::wstring wtitle = widen(dialogTitle(title).c_str());

    std::vector<wchar_t> selected(32768, L'\0');
    OPENFILENAMEW dialog {};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFilter = filter.c_str();
    dialog.lpstrFile   = selected.data();
    dialog.nMaxFile    = static_cast<DWORD>(selected.size());
    dialog.lpstrTitle  = wtitle.c_str();
    dialog.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetOpenFileNameW(&dialog)) return false;
    int n = WideCharToMultiByte(CP_UTF8, 0, selected.data(), -1, out, (int)outSize, nullptr, nullptr);
    if (n <= 0) { out[0] = '\0'; return false; }
    return out[0] != '\0';
}

#elif defined(__ANDROID__)

bool pc_file_dialog_open(const char*, const char*, const char*, char* out, size_t outSize)
{
    if (out && outSize) out[0] = '\0';
    return false; // Android uses the Java SAF picker
}

#else

static bool commandExists(const char* name)
{
    std::string cmd = std::string("command -v ") + name + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

using pc_dialog::shellQuote;

static std::string runDialog(const std::string& command)
{
    std::string result;
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return result;
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) result += buf;
    pclose(pipe);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
    return result;
}

bool pc_file_dialog_open(const char* title, const char* filterName, const char* patterns, char* out, size_t outSize)
{
    if (!out || outSize == 0) return false;
    out[0] = '\0';
    std::string selected;
    if (commandExists("zenity")) {
        selected = runDialog("zenity --file-selection " + shellQuote("--title=" + dialogTitle(title)) + " "
                             + shellQuote(std::string("--file-filter=") + filterName + " | " + patterns)
                             + " --file-filter='All files | *' 2>/dev/null");
    } else if (commandExists("kdialog")) {
        selected = runDialog("kdialog --getopenfilename . " + shellQuote(std::string(patterns) + "|" + filterName)
                             + " --title " + shellQuote(dialogTitle(title)) + " 2>/dev/null");
    } else {
#if defined(__APPLE__)
        // osascript ships with macOS. Cancel prints nothing, so it reads as
        // "no file chosen". The patterns are not passed (see chooseFileScript):
        // the caller checks the chosen file.
        selected = runDialog(pc_dialog::osascriptCommand(pc_dialog::chooseFileScript(dialogTitle(title))));
#else
        snprintf(out, outSize, "No file dialog: install zenity or kdialog.");
        return false;
#endif
    }
    if (selected.empty()) return false;
    snprintf(out, outSize, "%s", selected.c_str());
    return true;
}

#endif
