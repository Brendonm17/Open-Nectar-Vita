#pragma once

#include <algorithm>

namespace pikmin::launcher {

// Reparto vertical de las páginas del launcher en ventanas bajas. Todo es
// aritmética pura para poder probarlo sin ventana.

// Alto máximo del logo. Hasta 700 px de alto manda el ancho, como siempre; por
// debajo el logo encoge con la altura para dejar sitio a la página.
inline float logoHeightCap(float windowH)
{
    if (windowH >= 700.0f) return 1e9f;
    return std::max(56.0f, 118.0f - (700.0f - windowH) * 0.34f);
}

// Una página de alto fijo (Move Install): el hueco de arriba encoge hasta un
// mínimo y, si ni así cabe, la página pasa a tener scroll.
struct PageFit {
    float top = 0.0f;     // hueco entre las pestañas y la página
    bool scroll = false;
    float content = 0.0f; // alto total (hueco + página) para la región con scroll
};

inline PageFit fitPage(float content, float available, float preferredTop, float minTop)
{
    PageFit out;
    out.top = std::clamp(available - content, minTop, preferredTop);
    out.content = out.top + content;
    out.scroll = out.content > available + 1e-3f;
    return out;
}

// Huecos verticales de la página Games, de arriba abajo.
struct GamesSpacing {
    float top;     // pestañas → portadas
    float name;    // portada → nombre del juego
    float buttons; // nombre → primera fila de botones (el nombre mide 28 px)
    float update;  // última fila → Update
};

constexpr GamesSpacing kGamesNormal{ 24.0f, 22.0f, 38.0f, 32.0f };
constexpr GamesSpacing kGamesTight{ 12.0f, 12.0f, 34.0f, 16.0f };

struct GamesMetrics {
    float buttonH = 44.0f;
    float rowGap = 10.0f;
    float aspect = 1.40f;      // alto / ancho de la portada
    float comfortCoverH = 180.0f;
    float minCoverH = 120.0f;
};

struct GamesLayout {
    float columnW = 0.0f; // ancho de los botones bajo cada portada
    float gap = 0.0f;     // entre las dos columnas
    float coverW = 0.0f, coverH = 0.0f;
    GamesSpacing space = kGamesNormal;
    bool scroll = false;
    float content = 0.0f; // de las pestañas al final de la última fila
};

// Alto fijo debajo de las portadas: nombre, filas de botones y Update.
inline float gamesBelowCovers(const GamesSpacing& s, float rows, bool update, const GamesMetrics& m = {})
{
    const float rowsH = rows * m.buttonH + std::max(0.0f, rows - 1.0f) * m.rowGap;
    return s.name + s.buttons + rowsH + (update ? s.update + m.buttonH : 0.0f);
}

// Las portadas se quedan a su tamaño mientras caben; después encogen hasta un
// mínimo cómodo, luego se aprietan los huecos, luego encogen hasta el mínimo y
// solo entonces la página pasa a tener scroll. Los botones mantienen el ancho
// de su columna aunque la portada encoja. Cada paso continúa el anterior, así
// que al cambiar el alto de la ventana nada salta.
inline GamesLayout fitGames(float windowW, float available, float rows, bool update, GamesMetrics m = {})
{
    GamesLayout out;
    out.columnW = std::clamp(windowW * 0.19f, 190.0f, 280.0f);
    out.gap = out.columnW * 0.42f;
    const float preferredH = out.columnW * m.aspect;
    available = std::max(0.0f, available);

    auto fixed = [&](const GamesSpacing& s) { return s.top + gamesBelowCovers(s, rows, update, m); };
    const float normal = fixed(kGamesNormal), tight = fixed(kGamesTight);
    const float comfort = std::min(preferredH, m.comfortCoverH);
    if (available - normal >= comfort) {
        out.coverH = std::min(preferredH, available - normal);
        out.space = kGamesNormal;
    } else if (available - tight >= comfort) {
        // Huecos de normal a apretado, en proporción a lo que falta.
        const float k = (normal + comfort - available) / (normal - tight);
        auto lerp = [k](float a, float b) { return a + (b - a) * k; };
        out.space = { lerp(kGamesNormal.top, kGamesTight.top), lerp(kGamesNormal.name, kGamesTight.name),
                      lerp(kGamesNormal.buttons, kGamesTight.buttons), lerp(kGamesNormal.update, kGamesTight.update) };
        out.coverH = comfort;
    } else {
        out.space = kGamesTight;
        out.coverH = std::max(available - tight, std::min(comfort, m.minCoverH));
        out.scroll = available - tight < std::min(comfort, m.minCoverH) - 1e-3f;
    }
    out.coverW = out.coverH / m.aspect;
    out.content = fixed(out.space) + out.coverH;
    return out;
}

// Ajustes: filas arriba, caja de ayuda debajo. La caja cede primero (hasta dos
// líneas) para que las filas conserven unas tres opciones; si ni así caben, se
// oculta y las filas se quedan todo el alto.
struct SettingsPanelFit {
    float rowsH = 0.0f;
    float helpH = 0.0f; // 0: sin caja de ayuda
};

inline constexpr float kSettingsHelpFull = 104.0f; // cuatro líneas
inline constexpr float kSettingsHelpMin = 54.0f;   // dos líneas
inline constexpr float kSettingsRowsComfort = 250.0f;
inline constexpr float kSettingsRowsMin = 140.0f;
inline constexpr float kSettingsGap = 12.0f;

inline SettingsPanelFit fitSettingsPanel(float total)
{
    SettingsPanelFit out;
    if (total - kSettingsGap - kSettingsHelpMin < kSettingsRowsMin) {
        out.rowsH = std::max(1.0f, total);
        return out;
    }
    out.helpH = std::clamp(total - kSettingsGap - kSettingsRowsComfort, kSettingsHelpMin, kSettingsHelpFull);
    out.rowsH = total - kSettingsGap - out.helpH;
    return out;
}

} // namespace pikmin::launcher
