#pragma once
// Botones "gota de rocío" del launcher: la luz viene de arriba a la izquierda,
// con un punto brillante, un destello y una veta que sigue el arco del
// extremo izquierdo. Todo es CPU puro (sin ImGui ni GL) para poder probarlo:
// launcher_gui.cpp sube los mapas de bits a texturas y los funde según la
// animación de cada botón.

#include <cstdint>
#include <vector>

namespace pikmin {
namespace launcher {
namespace dewdrop {

struct Rgb {
    float r = 0, g = 0, b = 0;
};

// Aspectos que se hornean en textura. Los estados intermedios de la animación
// se consiguen fundiendo estas capas, nunca con una textura por fotograma.
enum class Look { Normal, Hovered, Pressed, Selected, SelectedHovered, SelectedPressed, Disabled, Count };

constexpr int kLookCount = int(Look::Count);

// Parámetros de cada aspecto (en unidades lógicas; la escala se aplica al hornear).
struct Style {
    Rgb border;              // oro del borde antes del sombreado de luz
    float borderWidth = 2;   // 3 cuando está seleccionado
    Rgb bodyLight, bodyDark; // degradado radial desde arriba a la izquierda
    float hotspot = 0.30f;   // intensidad del punto brillante
    float streak = 1.0f;     // intensidad de la veta del arco
    float glint = 1.0f;      // tamaño relativo del destello
    float flare = 0.7f;      // brillo de la cruz del destello
    float highlightScale = 1.0f; // encoge el brillo al pulsar
    float glow = 0;          // halo dorado exterior
    float glowSigma = 8;
    float innerGlow = 0;     // brillo dorado interior (seleccionado)
    float shadowX = 3.5f, shadowY = 5.0f, shadowAlpha = 0.8f;
    float bodyOffsetY = 0;   // el botón baja al pulsarlo
    bool disabled = false;
};

Style styleFor(Look look);

// Margen alrededor del botón dentro de la textura, para sombra y halo.
constexpr float kPad = 14.0f;

struct Bitmap {
    int width = 0, height = 0;          // píxeles
    std::vector<unsigned char> rgba;    // RGBA sin premultiplicar
};

// Hornea el botón de w x h unidades lógicas a `scale` píxeles por unidad. La
// textura empieza kPad unidades arriba a la izquierda del botón.
Bitmap render(Look look, float w, float h, float scale);
inline int pixels(float logical, float scale) { return int(logical * scale + 0.5f); }

// Destello en cruz para el centelleo al entrar el ratón (blanco con alfa).
Bitmap renderSparkle(int sizePx);
// Dónde va el destello, relativo a la esquina del botón (unidades lógicas).
void glintCentre(float w, float h, float& x, float& y);

// ── Animación ──────────────────────────────────────────────────────────────
// Duraciones hasta el 95 % del cambio. Acercamiento exponencial: empieza
// rápido y frena (ease-out), no depende de los fotogramas por segundo y se
// puede invertir a mitad sin saltos.
constexpr float kHoverSeconds = 0.15f;
constexpr float kPressSeconds = 0.075f;
constexpr float kReleaseSeconds = 0.12f;
constexpr float kSelectSeconds = 0.18f;
constexpr float kDisableSeconds = 0.15f;
constexpr float kTwinkleSeconds = 0.55f;
constexpr float kPressLatch = 0.85f; // un clic rápido llega al menos hasta aquí

float easeToward(float value, float target, float dt, float seconds);

struct Anim {
    float hover = 0, press = 0, select = 0, disabled = 0;
    float twinkle = -1; // segundos desde que entró el ratón; <0 = parado
    bool pressing = false;
    bool wasHovered = false;
    bool initialised = false;
    bool animating() const;
};

void step(Anim& anim, bool hovered, bool held, bool selected, bool disabled, float dt);

// Peso de cada aspecto horneado. Suman 1: el fotograma intermedio es la mezcla
// lineal (premultiplicada) de los aspectos, así que en reposo coincide
// exactamente con uno de ellos y la sombra o el halo no se acumulan al fundir.
struct Layers {
    float weight[kLookCount] = {};
    float textDrop = 0;    // unidades lógicas que baja el texto
    float textAlpha = 1;
    float twinkle = 0;     // 0..1 brillo del centelleo
    float twinkleScale = 1;
    float twinkleAngle = 0;
};

Layers layersFor(const Anim& anim);
// El aspecto con peso 1, o Look::Count si hay que mezclar.
Look settledLook(const Layers& layers);

// Mezcla los aspectos (todos del mismo tamaño) según `weight`. Los que pesan
// 0 pueden ser nullptr.
void composite(const Bitmap* const looks[kLookCount], const float weight[kLookCount], Bitmap& out);

// ── Texto ──────────────────────────────────────────────────────────────────
constexpr Rgb kTextCream { 1.0f, 243 / 255.0f, 214 / 255.0f };   // #FFF3D6
constexpr Rgb kTextOutline { 15 / 255.0f, 58 / 255.0f, 20 / 255.0f }; // #0F3A14
float luminance(Rgb c);
float contrastRatio(Rgb a, Rgb b);

} // namespace dewdrop
} // namespace launcher
} // namespace pikmin
