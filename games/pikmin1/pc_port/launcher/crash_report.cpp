#include "crash_report.h"

#include "launcher_gui.h"

#include <SDL.h>

#include <cctype>
#include <deque>
#include <fstream>
#include <regex>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace pikmin {
namespace launcher {
namespace {

constexpr const char* kIssuesNewUrl = "https://github.com/SSunnKing/Open-Nectar---Pikmin-Native-PC-Mobile-Port/issues/new";
// GitHub admite unos 65 536 caracteres por issue: margen para lo que escriba el jugador.
constexpr std::size_t kMaxReportChars = 58000;
constexpr std::size_t kMaxLogLines    = 400;

std::string readAll(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// Últimas líneas del log, sin pasar de `budget` caracteres.
std::string logTail(const fs::path& path, std::size_t budget)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::string();
    std::deque<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        lines.push_back(line);
        if (lines.size() > kMaxLogLines) lines.pop_front();
    }
    std::string tail;
    std::size_t used = 0;
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        if (used + it->size() + 1 > budget) break;
        used += it->size() + 1;
        tail.insert(0, *it + "\n");
    }
    return tail;
}

// El nombre de usuario aparece en las rutas (C:\Users\nombre\..., /home/nombre/...).
std::string anonymise(std::string text)
{
    static const std::regex windows(R"(([A-Za-z]:[\\/](?:Users|Documents and Settings)[\\/])[^\\/\r\n"]+)",
                                    std::regex::icase);
    static const std::regex unixHome(R"((/home/|/Users/)[^/\r\n"]+)");
    text = std::regex_replace(text, windows, "$1<user>");
    text = std::regex_replace(text, unixHome, "$1<user>");
    return text;
}

// Valor de una línea "clave: valor" de la cabecera del informe.
std::string headerValue(const std::string& crash, const std::string& key)
{
    const std::string prefix = key + ": ";
    std::istringstream in(crash);
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, prefix.size(), prefix) == 0) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return line.substr(prefix.size());
        }
    }
    return std::string();
}

std::string urlEncode(const std::string& text)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += char(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

} // namespace

bool findCrashReport(const std::vector<CrashSource>& sources, CrashReport& report)
{
    for (const CrashSource& source : sources) {
        if (source.directory.empty()) continue;
        const fs::path file = source.directory / source.crashFile;
        std::error_code error;
        if (!fs::is_regular_file(file, error)) continue;

        const std::string crash = anonymise(readAll(file));
        std::string version = headerValue(crash, "version");
        const std::string platform = headerValue(crash, "platform");
        report.file  = file;
        report.title = "[Crash] " + source.game + (version.empty() ? "" : " " + version.substr(0, version.find(' ')))
                     + (platform.empty() ? "" : " (" + platform + ")");

        std::string text;
        text += "**What were you doing when it crashed?**\n\n_(describe it here, optional)_\n\n";
        text += "<details><summary>Crash report</summary>\n\n```\n" + crash + "\n```\n</details>\n\n";
        const std::size_t budget = text.size() < kMaxReportChars ? kMaxReportChars - text.size() : 0;
        const std::string log = anonymise(logTail(source.directory / source.logFile, budget));
        if (!log.empty()) {
            text += "<details><summary>Game log (last lines)</summary>\n\n```\n" + log + "```\n</details>\n";
        }
        report.text = text;
        return true;
    }
    return false;
}

std::string crashIssueUrl(const CrashReport& report)
{
    const std::string body = "Paste the crash report here (Ctrl+V, or long-press > Paste on a phone). "
                             "The launcher already copied it to your clipboard.\n\n";
    return std::string(kIssuesNewUrl) + "?title=" + urlEncode(report.title) + "&body=" + urlEncode(body)
         + "&labels=crash";
}

void markCrashReportSeen(const CrashReport& report)
{
    std::error_code error;
    fs::path seen = report.file;
    seen += ".reported";
    fs::remove(seen, error);
    fs::rename(report.file, seen, error);
    if (error) fs::remove(report.file, error);
}

void offerCrashReport(HubWindow& hub, const std::vector<CrashSource>& sources)
{
    CrashReport report;
    if (!findCrashReport(sources, report)) return;
    const int answer = hub.ask("The game closed unexpectedly",
                               "A crash report was saved. Help us fix it: press the button below, then paste the "
                               "report (Ctrl+V) into the GitHub issue that opens and send it. You can add what you "
                               "were doing. Your user name is removed from file paths.",
                               "Copy report and open GitHub", "Close", true);
    if (answer == 0) {
        SDL_SetClipboardText(report.text.c_str());
        SDL_OpenURL(crashIssueUrl(report).c_str());
    }
    markCrashReportSeen(report);
}

} // namespace launcher
} // namespace pikmin
