#pragma once
// Informes de crash de los juegos (pikmin2_crash.log, ...): el launcher los
// busca al abrirse, también cuando lo lanza el propio juego al fallar
// (--crash-report), y ofrece copiarlos al portapapeles y abrir la página de
// issues de GitHub. Sin servidor ni tokens: el jugador envía el issue con su
// cuenta y decide qué pega.

#include <filesystem>
#include <string>
#include <vector>

namespace pikmin {
namespace launcher {

class HubWindow;

struct CrashSource {
    std::string game;              // nombre para el título ("Pikmin 2")
    std::filesystem::path directory; // carpeta de trabajo del juego
    std::string crashFile;         // "pikmin2_crash.log"
    std::string logFile;           // "pikmin2.log" (puede no existir)
};

struct CrashReport {
    std::filesystem::path file; // informe sin ver
    std::string title;          // título del issue
    std::string text;           // Markdown listo para pegar
};

// Primer informe sin ver de `sources`. false si no hay ninguno.
bool findCrashReport(const std::vector<CrashSource>& sources, CrashReport& report);

// Página de nuevo issue con el título puesto y la indicación de pegar.
std::string crashIssueUrl(const CrashReport& report);

// Lo renombra a <archivo>.reported para no volver a ofrecerlo.
void markCrashReportSeen(const CrashReport& report);

// Busca un informe y, si lo hay, muestra la ventana: "Copiar informe y abrir
// GitHub" o "Cerrar". En los dos casos queda marcado como visto.
void offerCrashReport(HubWindow& hub, const std::vector<CrashSource>& sources);

} // namespace launcher
} // namespace pikmin
