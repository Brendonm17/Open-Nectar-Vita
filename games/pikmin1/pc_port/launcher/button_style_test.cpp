// Pruebas del estilo "gota de rocío" de los botones del launcher: la
// animación (duraciones, independencia de los FPS, pulsación corta), los pesos
// de las capas, los parámetros de cada aspecto y el contraste del texto.
#include "button_style.h"

#include <cmath>
#include <cstdio>

using namespace pikmin::launcher::dewdrop;

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

// Segundos hasta que `field` llega a `level` (0..1) con el botón en el estado dado.
template <typename Field>
float timeToReach(Anim anim, Field field, float level, bool hovered, bool held, float fps)
{
    const float dt = 1.0f / fps;
    float t = 0;
    while (t < 2.0f) {
        step(anim, hovered, held, false, false, dt);
        t += dt;
        const float v = field(anim);
        if (level >= 0.5f ? v >= level : v <= level) return t;
    }
    return 99.0f;
}

float sum(const Layers& l)
{
    float s = 0;
    for (float w : l.weight) s += w;
    return s;
}

struct Rgba {
    float r, g, b, a;
};
Rgba at(const Bitmap& b, int x, int y)
{
    const unsigned char* p = &b.rgba[(size_t(y) * b.width + x) * 4];
    return { p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f };
}
float brightness(Rgba c) { return luminance({ c.r, c.g, c.b }); }

} // namespace

int main()
{
    // ── Easing ───────────────────────────────────────────────────────────────
    {
        const float v = easeToward(0, 1, kHoverSeconds, kHoverSeconds);
        check(v > 0.94f && v < 0.96f, "easeToward reaches ~95% at its duration");
        // Ease-out: el primer tramo avanza más que el último.
        const float early = easeToward(0, 1, kHoverSeconds * 0.25f, kHoverSeconds);
        check(early > 0.45f, "easeToward starts fast (ease-out)");
        check(easeToward(0.999f, 1, 0.001f, 1) == 1.0f, "easeToward snaps when close");
        check(easeToward(0.3f, 1, 0, 1) == 0.3f, "zero dt does not move");
        // Independiente de los FPS: 60 y 144 Hz dan lo mismo tras el mismo tiempo.
        float a = 0, b = 0;
        for (int i = 0; i < 6; ++i) a = easeToward(a, 1, 1 / 60.0f, kHoverSeconds);
        for (int i = 0; i < 15; ++i) b = easeToward(b, 1, 1 / 150.0f, kHoverSeconds);
        check(std::fabs(a - b) < 0.01f, "easing is frame-rate independent");
    }

    // ── Duraciones de hover y pulsación ─────────────────────────────────────
    {
        const auto hover = [](const Anim& a) { return a.hover; };
        const auto press = [](const Anim& a) { return a.press; };
        for (float fps : { 30.0f, 60.0f, 144.0f }) {
            Anim idle;
            const float in = timeToReach(idle, hover, 0.95f, true, false, fps);
            check(in >= 0.12f - 1 / fps && in <= 0.18f + 1 / fps, "hover-in settles in 120-180 ms");
            Anim hot;
            hot.hover = 1;
            hot.initialised = hot.wasHovered = true;
            const float out = timeToReach(hot, hover, 0.05f, false, false, fps);
            check(out >= 0.12f - 1 / fps && out <= 0.18f + 1 / fps, "hover-out settles in 120-180 ms");
            const float down = timeToReach(idle, press, 0.95f, true, true, fps);
            check(down >= 0.06f - 1 / fps && down <= 0.09f + 1 / fps, "press settles in 60-90 ms");
        }
    }

    // ── Un clic de un solo fotograma también se ve ───────────────────────────
    {
        Anim a;
        step(a, true, true, false, false, 1 / 144.0f);
        float peak = a.press;
        for (int i = 0; i < 60; ++i) {
            step(a, true, false, false, false, 1 / 144.0f);
            peak = std::max(peak, a.press);
        }
        check(peak >= kPressLatch, "a one-frame click still presses to the latch");
        check(a.press == 0.0f && !a.pressing, "the press releases afterwards");
    }

    // ── Estado inicial, centelleo y fin de la animación ─────────────────────
    {
        Anim a;
        step(a, false, false, true, false, 1 / 60.0f);
        check(a.select == 1.0f, "a button that starts selected does not animate in");
        check(!a.animating(), "idle button is not animating");
        step(a, true, false, true, false, 1 / 60.0f);
        check(a.twinkle >= 0 && a.animating(), "hover-enter starts the twinkle");
        for (int i = 0; i < 120; ++i) step(a, true, false, true, false, 1 / 60.0f);
        check(a.twinkle < 0, "the twinkle plays once");
        check(!a.animating(), "everything settles while still hovered (no endless loop)");
        Anim off;
        step(off, false, false, false, true, 1 / 60.0f);
        step(off, true, false, false, true, 1 / 60.0f);
        check(off.twinkle < 0, "disabled buttons do not twinkle");
        // Seleccionar funde en ~180 ms.
        Anim s;
        step(s, false, false, false, false, 1 / 60.0f);
        float t = 0;
        while (s.select < 0.95f && t < 1) {
            step(s, false, false, true, false, 1 / 60.0f);
            t += 1 / 60.0f;
        }
        check(t > 0.12f && t < 0.2f, "selection cross-fades in ~180 ms");
    }

    // ── Pesos de las capas ───────────────────────────────────────────────────
    {
        Anim a;
        a.initialised = true;
        check(settledLook(layersFor(a)) == Look::Normal, "rest is Normal");
        a.hover = 1;
        check(settledLook(layersFor(a)) == Look::Hovered, "hover is Hovered");
        a.press = 1;
        check(settledLook(layersFor(a)) == Look::Pressed, "held is Pressed");
        check(layersFor(a).textDrop == 1.0f, "text drops 1 unit when pressed");
        a.select = 1;
        check(settledLook(layersFor(a)) == Look::SelectedPressed, "selected + held");
        a.press = 0;
        check(settledLook(layersFor(a)) == Look::SelectedHovered, "selected + hover");
        a.hover = 0;
        check(settledLook(layersFor(a)) == Look::Selected, "selected");
        a.disabled = 1;
        check(settledLook(layersFor(a)) == Look::Disabled, "disabled");
        check(layersFor(a).textAlpha < 0.6f, "disabled text is dimmed");
        bool ok = true;
        for (float h : { 0.0f, 0.3f, 1.0f })
            for (float p : { 0.0f, 0.5f, 1.0f })
                for (float s : { 0.0f, 0.7f, 1.0f })
                    for (float d : { 0.0f, 0.2f, 1.0f }) {
                        Anim m;
                        m.hover = h, m.press = p, m.select = s, m.disabled = d;
                        const Layers l = layersFor(m);
                        ok = ok && std::fabs(sum(l) - 1) < 1e-4f;
                        for (float w : l.weight) ok = ok && w >= 0;
                    }
        check(ok, "layer weights are a partition of one");
        Anim mid;
        mid.hover = 0.5f;
        check(settledLook(layersFor(mid)) == Look::Count, "mid-transition needs a blend");
    }

    // ── Parámetros de cada aspecto ───────────────────────────────────────────
    {
        const Style n = styleFor(Look::Normal), h = styleFor(Look::Hovered), p = styleFor(Look::Pressed);
        const Style s = styleFor(Look::Selected);
        check(luminance(h.bodyLight) <= luminance(n.bodyLight) && luminance(h.bodyDark) <= luminance(n.bodyDark),
            "hover keeps the fill dark");
        check(luminance(h.border) > luminance(n.border), "hover brightens the border");
        check(h.glow > 0 && n.glow == 0, "hover adds an outer glow");
        check(h.glint > n.glint && h.streak > n.streak && h.flare > n.flare, "hover strengthens sparkle and streak");
        check(luminance(p.bodyLight) < luminance(n.bodyLight), "pressed fill is darker");
        check(p.glint < n.glint && p.highlightScale < 1, "pressed highlight is smaller");
        check(p.shadowY < n.shadowY && p.bodyOffsetY > 0, "pressed sits lower with a shorter shadow");
        check(s.borderWidth > n.borderWidth && s.innerGlow > 0, "selected has a thicker border and inner glow");
        check(styleFor(Look::Disabled).disabled, "disabled look is flagged");
    }

    // ── Mapa de bits ─────────────────────────────────────────────────────────
    {
        const float w = 200, h = 44;
        const Bitmap b1 = render(Look::Normal, w, h, 1);
        const Bitmap b2 = render(Look::Normal, w, h, 2);
        check(b1.width == pixels(w + 2 * kPad, 1) && b1.height == pixels(h + 2 * kPad, 1), "1x size");
        check(b2.width == 2 * b1.width && b2.height == 2 * b1.height, "2x bitmap doubles");
        check(at(b2, 0, 0).a == 0 && at(b2, b2.width - 1, 0).a == 0, "corners are transparent");
        // Esquina redondeada: dentro del rectángulo pero fuera de la píldora.
        const int k = int(kPad * 2 + 1);
        check(at(b2, k, k).a < 0.9f, "pill corners are round");
        const Rgba tl = at(b2, int((kPad + h * 0.6f) * 2), int((kPad + h * 0.3f) * 2));
        const Rgba br = at(b2, int((kPad + w - h * 0.6f) * 2), int((kPad + h * 0.7f) * 2));
        check(tl.a == 1 && br.a == 1, "body is opaque");
        check(brightness(tl) > brightness(br) * 1.5f, "lit from the top-left");
        const Rgba shadow = at(b2, int((kPad + w * 0.5f) * 2), int((kPad + h + 4) * 2));
        check(shadow.a > 0.2f && brightness(shadow) < 0.02f, "drop shadow below");
        const Bitmap dis = render(Look::Disabled, w, h, 2);
        const Rgba d = at(dis, int((kPad + w * 0.5f) * 2), int((kPad + h * 0.5f) * 2));
        check(std::fabs(d.r - d.g) < 0.06f && std::fabs(d.g - d.b) < 0.06f, "disabled is desaturated");
        // Mezclar al 50 % queda entre los dos aspectos.
        const Bitmap hov = render(Look::Hovered, w, h, 2);
        const Bitmap* looks[kLookCount] = { &b2, &hov };
        float weight[kLookCount] = { 0.5f, 0.5f };
        Bitmap mix;
        composite(looks, weight, mix);
        const int sx = int((kPad + w * 0.5f) * 2), sy = int((kPad + h + 4) * 2);
        const float lo = std::min(at(b2, sx, sy).a, at(hov, sx, sy).a), hi = std::max(at(b2, sx, sy).a, at(hov, sx, sy).a);
        check(at(mix, sx, sy).a >= lo - 0.01f && at(mix, sx, sy).a <= hi + 0.01f, "blend does not stack shadows");
        float only[kLookCount] = { 1.0f };
        composite(looks, only, mix);
        check(mix.rgba == b2.rgba || at(mix, sx, sy).a == at(b2, sx, sy).a, "weight 1 reproduces the look");
        const Bitmap sp = renderSparkle(32);
        check(at(sp, 16, 16).a > 0.9f && at(sp, 0, 0).a < 0.05f, "sparkle is a bright point with clear edges");
    }

    // ── Contraste del texto ──────────────────────────────────────────────────
    {
        check(contrastRatio(kTextCream, kTextOutline) >= 7.0f, "cream text vs outline >= 7:1");
        const Style n = styleFor(Look::Normal), p = styleFor(Look::Pressed), h = styleFor(Look::Hovered);
        // Donde va el texto (el centro del botón), en todos los aspectos activos.
        for (Look look : { Look::Normal, Look::Hovered, Look::Pressed, Look::Selected, Look::SelectedHovered, Look::SelectedPressed }) {
            const Bitmap b = render(look, 260, 55, 2);
            const Rgba c = at(b, b.width / 2, b.height / 2);
            check(contrastRatio(kTextCream, { c.r, c.g, c.b }) >= 4.5f, "cream text vs body centre >= 4.5:1");
        }
        check(contrastRatio(kTextCream, n.bodyLight) >= 2.0f, "cream text stays readable on the lit side");
        check(contrastRatio(kTextCream, p.bodyDark) >= contrastRatio(kTextCream, n.bodyDark), "pressed does not lose contrast");
        check(contrastRatio(kTextCream, h.bodyLight) >= contrastRatio(kTextCream, n.bodyLight), "hover does not lose contrast");
    }

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("button_style_test: all checks passed\n");
    return 0;
}
