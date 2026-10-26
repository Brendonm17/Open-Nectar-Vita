#include "button_style.h"

#include <algorithm>
#include <cmath>

namespace pikmin {
namespace launcher {
namespace dewdrop {
namespace {

constexpr float kPi = 3.14159265f;

Rgb hex(uint32_t v) { return { ((v >> 16) & 255) / 255.0f, ((v >> 8) & 255) / 255.0f, (v & 255) / 255.0f }; }
Rgb scaled(Rgb c, float k) { return { c.r * k, c.g * k, c.b * k }; }
Rgb mix(Rgb a, Rgb b, float t) { return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t }; }
float clamp01(float v) { return std::min(1.0f, std::max(0.0f, v)); }
Rgb clamped(Rgb c) { return { clamp01(c.r), clamp01(c.g), clamp01(c.b) }; }

const Rgb kBorder = hex(0xE8962A), kBorderHover = hex(0xFFC24D), kBorderSelected = hex(0xFFD36B);
const Rgb kShadow = hex(0x020D04), kGlow = hex(0xFFB02E), kHotspot = hex(0xE9FFD8), kRim = hex(0xB7F59A);
const Rgb kWhite { 1, 1, 1 };

// Píxel premultiplicado.
struct Px {
    float r = 0, g = 0, b = 0, a = 0;
    void over(Rgb c, float alpha) // pinta encima
    {
        alpha = clamp01(alpha);
        r = r * (1 - alpha) + c.r * alpha;
        g = g * (1 - alpha) + c.g * alpha;
        b = b * (1 - alpha) + c.b * alpha;
        a = a * (1 - alpha) + alpha;
    }
    void light(Rgb c, float amount) // luz que se suma (solo donde hay botón)
    {
        amount = std::max(0.0f, amount);
        r = std::min(a, r + c.r * amount);
        g = std::min(a, g + c.g * amount);
        b = std::min(a, b + c.b * amount);
    }
};

float pillDistance(float x, float y, float cx, float cy, float w, float h)
{
    const float r = h * 0.5f;
    const float px = std::max(std::fabs(x - cx) - (w * 0.5f - r), 0.0f);
    return std::sqrt(px * px + (y - cy) * (y - cy)) - r;
}

float gauss(float v) { return std::exp(-v * v); }
// Escalón desenfocado (como un GaussianBlur de radio sigma sobre la forma).
float blurredStep(float d, float sigma) { return 0.5f * std::erfc(d / (sigma * 1.41421356f)); }

void store(Bitmap& out, int i, const Px& p)
{
    unsigned char* o = &out.rgba[size_t(i) * 4];
    const float a = clamp01(p.a);
    const float inv = a > 1e-5f ? 1.0f / a : 0.0f;
    o[0] = (unsigned char)std::lround(clamp01(p.r * inv) * 255.0f);
    o[1] = (unsigned char)std::lround(clamp01(p.g * inv) * 255.0f);
    o[2] = (unsigned char)std::lround(clamp01(p.b * inv) * 255.0f);
    o[3] = (unsigned char)std::lround(a * 255.0f);
}

} // namespace

Style styleFor(Look look)
{
    Style s;
    s.border = kBorder;
    s.bodyLight = hex(0x5CC064);
    s.bodyDark = hex(0x123F17);
    const bool selected = look == Look::Selected || look == Look::SelectedHovered || look == Look::SelectedPressed;
    const bool hovered = look == Look::Hovered || look == Look::SelectedHovered;
    const bool pressed = look == Look::Pressed || look == Look::SelectedPressed;
    if (selected) {
        s.border = kBorderSelected;
        s.borderWidth = 3.0f;
        s.innerGlow = 0.30f;
        s.glow = 0.35f;
        s.glowSigma = 6.0f;
    }
    if (hovered) {
        // El relleno no se aclara: el texto se sigue leyendo. Se nota en el
        // borde, el halo y el brillo.
        if (!selected) s.border = kBorderHover;
        s.glow = 0.5f;
        s.glowSigma = 8.0f;
        s.hotspot = 0.42f;
        s.streak = 1.3f;
        s.glint = 1.27f;
        s.flare = 1.0f;
    }
    if (pressed) {
        if (!selected) s.border = kBorderHover;
        s.bodyLight = hex(0x45A04B);
        s.bodyDark = hex(0x0E3412);
        s.highlightScale = 0.6f;
        s.glint = 0.75f;
        s.shadowX = 1.5f;
        s.shadowY = 2.0f;
        s.bodyOffsetY = 1.0f;
        s.glow = selected ? 0.25f : 0.3f;
    }
    if (look == Look::Disabled) s.disabled = true;
    return s;
}

void glintCentre(float w, float h, float& x, float& y)
{
    (void)w;
    x = h * 0.5f * 0.62f;
    y = h * 0.33f;
}

Bitmap render(Look look, float w, float h, float scale)
{
    const Style st = styleFor(look);
    Bitmap out;
    out.width = std::max(1, int(std::ceil((w + 2 * kPad) * scale)));
    out.height = std::max(1, int(std::ceil((h + 2 * kPad) * scale)));
    out.rgba.assign(size_t(out.width) * out.height * 4, 0);

    const float px = 1.0f / scale; // un píxel en unidades lógicas, para el antialias
    const float left = kPad, top = kPad + st.bodyOffsetY;
    const float cx = left + w * 0.5f, cy = top + h * 0.5f;
    const float r = h * 0.5f;
    const float b = st.borderWidth;
    const float hs = st.highlightScale;
    const float unit = r / 27.5f; // el boceto medía 55 de alto
    float gx, gy;
    glintCentre(w, h, gx, gy);
    gx += left;
    gy += top;
    const float gs = std::max(1.4f, 2.75f * unit * st.glint);
    const float line = std::max(0.6f, 0.7f * px);
    const float hx = left + std::min(w * 0.22f, r * 1.76f), hy = top + h * 0.18f;
    const float cxl = left + r; // centro del arco izquierdo

    for (int j = 0; j < out.height; ++j) {
        for (int i = 0; i < out.width; ++i) {
            const float x = (i + 0.5f) * px, y = (j + 0.5f) * px;
            const float d = pillDistance(x, y, cx, cy, w, h);
            Px p;
            // Sombra abajo a la derecha.
            const float ds = pillDistance(x - st.shadowX, y - st.shadowY, cx, cy, w, h);
            p.over(kShadow, blurredStep(ds, 4.5f) * st.shadowAlpha);
            // Halo dorado.
            if (st.glow > 0) p.over(kGlow, blurredStep(d - 1.0f, st.glowSigma) * st.glow);
            const float inside = clamp01(0.5f - d * scale);
            if (inside > 0) {
                // Borde con la luz desde arriba a la izquierda.
                const float lx = (x - left) / w, ly = (y - top) / h;
                const float k = clamp01(1 - (0.6f * lx + 0.4f * ly));
                p.over(clamped(mix(scaled(st.border, 0.72f), scaled(st.border, 1.15f), k)), inside);
                const float body = clamp01(0.5f - (d + b) * scale);
                if (body > 0) {
                    const float rx = (x - hx) / (w * 0.85f), ry = (y - hy) / (h * 1.3f);
                    const float rr = clamp01(std::sqrt(rx * rx + ry * ry));
                    p.over(mix(st.bodyLight, st.bodyDark, std::pow(rr, 0.8f)), body);
                    // Punto brillante suave.
                    const float hot = std::exp(-(std::pow((x - (left + r * 1.05f)) / (r * 0.95f * (0.6f + 0.4f * hs)), 2.0f)
                                                 + std::pow((y - (top + h * 0.34f)) / (h * 0.26f * (0.6f + 0.4f * hs)), 2.0f)));
                    p.light(kHotspot, hot * body * st.hotspot * (hs < 1 ? 0.6f : 1.0f));
                    // Veta que sigue el arco de arriba a la izquierda (s = longitud de arco).
                    const float a = std::atan2(y - cy, x - cxl);
                    float s;
                    if (x < cxl) s = y < cy ? r * (a + kPi) : -r * (kPi - a);
                    else s = (y > cy + 1) ? -999.0f : r * kPi * 0.5f + (x - cxl);
                    const float radial = gauss((d + b + 3.0f) / 1.4f);
                    const float along = gauss((s - r * 0.95f) / (r * 0.55f * hs));
                    p.light(kWhite, radial * along * body * st.streak * (hs < 1 ? 0.6f : 1.0f));
                    // Destello: punto nítido, halo y cruz.
                    const float gd = std::sqrt((x - gx) * (x - gx) + (y - gy) * (y - gy) * 1.3225f);
                    p.light(kWhite, clamp01(0.5f - (gd - gs) / std::max(0.8f, px)) * body);
                    p.light(kWhite, gauss(gd / (gs * 2.2f)) * 0.5f * body);
                    const float fl = gauss((y - gy) / line) * gauss((x - gx) / (gs * 3.2f))
                                   + gauss((x - gx) / line) * gauss((y - gy) / (gs * 2.4f));
                    p.light(kWhite, fl * 0.55f * body * st.flare);
                    // Luz refractada abajo a la derecha.
                    p.light(kRim, gauss((d + b + 1.5f) / 1.25f) * clamp01((lx - 0.55f) / 0.45f) * clamp01((ly - 0.5f) / 0.5f) * body * 0.25f);
                    if (st.innerGlow > 0) p.light(kBorderHover, std::exp(-std::max(-(d + b), 0.0f) / 3.5f) * body * st.innerGlow);
                }
                if (st.disabled) {
                    // Apagado: sin color y con menos contraste.
                    const float m = clamp01(0.5f - (d - 0.5f) * scale);
                    const float a = std::max(p.a, 1e-5f);
                    Rgb c { p.r / a, p.g / a, p.b / a };
                    const float grey = c.r * 0.3f + c.g * 0.59f + c.b * 0.11f;
                    Rgb dc = mix(c, Rgb { grey, grey, grey }, 0.8f);
                    dc = { dc.r * 0.62f + 0.05f, dc.g * 0.62f + 0.05f, dc.b * 0.62f + 0.05f };
                    const Rgb fin = mix(c, dc, m);
                    p.r = fin.r * p.a;
                    p.g = fin.g * p.a;
                    p.b = fin.b * p.a;
                }
            }
            store(out, j * out.width + i, p);
        }
    }
    return out;
}

Bitmap renderSparkle(int size)
{
    Bitmap out;
    out.width = out.height = std::max(4, size);
    out.rgba.assign(size_t(out.width) * out.height * 4, 0);
    const float c = out.width * 0.5f, unit = out.width / 32.0f;
    for (int j = 0; j < out.height; ++j) {
        for (int i = 0; i < out.width; ++i) {
            const float x = (i + 0.5f - c) / unit, y = (j + 0.5f - c) / unit;
            const float rr = std::sqrt(x * x + y * y);
            // Cruz de cuatro puntas que se afinan, más un núcleo redondo.
            float v = gauss(y / (0.9f + 0.02f * std::fabs(x))) * gauss(x / 13.0f)
                    + gauss(x / (0.9f + 0.02f * std::fabs(y))) * gauss(y / 13.0f);
            v += gauss(rr / 2.2f) + 0.35f * gauss(rr / 6.0f);
            unsigned char* o = &out.rgba[(size_t(j) * out.width + i) * 4];
            o[0] = o[1] = o[2] = 255;
            o[3] = (unsigned char)std::lround(clamp01(v) * 255.0f);
        }
    }
    return out;
}

// ── Animación ──────────────────────────────────────────────────────────────
float easeToward(float value, float target, float dt, float seconds)
{
    if (seconds <= 0 || dt <= 0) return dt > 0 ? target : value;
    // exp(-3) ≈ 5 %: a los `seconds` queda un 5 % del recorrido.
    const float k = 1.0f - std::exp(-3.0f * dt / seconds);
    float next = value + (target - value) * k;
    if (std::fabs(target - next) < 0.002f) next = target;
    return next;
}

bool Anim::animating() const
{
    const auto settled = [](float v) { return v == 0.0f || v == 1.0f; };
    return !settled(hover) || !settled(press) || !settled(select) || !settled(disabled) || twinkle >= 0 || pressing;
}

void step(Anim& anim, bool hovered, bool held, bool selected, bool disabled, float dt)
{
    if (!anim.initialised) {
        // Un botón que aparece ya seleccionado o desactivado no se anima al entrar.
        anim.select = selected ? 1.0f : 0.0f;
        anim.disabled = disabled ? 1.0f : 0.0f;
        anim.initialised = true;
    }
    if (hovered && !anim.wasHovered && !disabled) anim.twinkle = 0;
    anim.wasHovered = hovered;
    if (anim.twinkle >= 0) {
        anim.twinkle += dt;
        if (anim.twinkle >= kTwinkleSeconds) anim.twinkle = -1;
    }
    // Un clic de un solo fotograma también se ve: se sigue hundiendo hasta kPressLatch.
    if (held) anim.pressing = true;
    else if (anim.press >= kPressLatch) anim.pressing = false;
    const bool down = held || anim.pressing;
    anim.press = easeToward(anim.press, down ? 1.0f : 0.0f, dt, down ? kPressSeconds : kReleaseSeconds);
    if (!held && anim.press >= kPressLatch) anim.pressing = false;
    anim.hover = easeToward(anim.hover, hovered ? 1.0f : 0.0f, dt, kHoverSeconds);
    anim.select = easeToward(anim.select, selected ? 1.0f : 0.0f, dt, kSelectSeconds);
    anim.disabled = easeToward(anim.disabled, disabled ? 1.0f : 0.0f, dt, kDisableSeconds);
}

Layers layersFor(const Anim& anim)
{
    Layers l;
    const float d = anim.disabled, on = 1 - d;
    const float s = anim.select, h = anim.hover, p = anim.press;
    const float plain = (1 - h) * (1 - p), lifted = h * (1 - p);
    l.weight[int(Look::Normal)] = on * (1 - s) * plain;
    l.weight[int(Look::Hovered)] = on * (1 - s) * lifted;
    l.weight[int(Look::Pressed)] = on * (1 - s) * p;
    l.weight[int(Look::Selected)] = on * s * plain;
    l.weight[int(Look::SelectedHovered)] = on * s * lifted;
    l.weight[int(Look::SelectedPressed)] = on * s * p;
    l.weight[int(Look::Disabled)] = d;
    l.textDrop = p * on;
    l.textAlpha = 1.0f - 0.5f * d;
    if (anim.twinkle >= 0 && d < 1.0f) {
        const float u = std::min(1.0f, anim.twinkle / kTwinkleSeconds);
        // Sube rápido y se apaga despacio, una sola vez.
        l.twinkle = std::sin(kPi * std::pow(u, 0.6f)) * (1 - p) * on;
        l.twinkleScale = 0.6f + 0.7f * std::sin(kPi * std::min(1.0f, u * 1.2f));
        l.twinkleAngle = 0.5f * u; // gira unos 30 grados
    }
    return l;
}

Look settledLook(const Layers& layers)
{
    for (int i = 0; i < kLookCount; ++i)
        if (layers.weight[i] >= 0.999f) return Look(i);
    return Look::Count;
}

void composite(const Bitmap* const looks[kLookCount], const float weight[kLookCount], Bitmap& out)
{
    const Bitmap* first = nullptr;
    int used[kLookCount], n = 0;
    float w[kLookCount];
    for (int i = 0; i < kLookCount; ++i) {
        if (weight[i] <= 0.0005f || !looks[i]) continue;
        if (!first) first = looks[i];
        if (looks[i]->width != first->width || looks[i]->height != first->height) continue;
        used[n] = i;
        w[n++] = weight[i];
    }
    if (!first) {
        out = Bitmap();
        return;
    }
    out.width = first->width;
    out.height = first->height;
    out.rgba.resize(first->rgba.size());
    const size_t count = size_t(out.width) * out.height;
    for (size_t px = 0; px < count; ++px) {
        float r = 0, g = 0, b = 0, a = 0;
        for (int k = 0; k < n; ++k) {
            const unsigned char* s = &looks[used[k]]->rgba[px * 4];
            const float sa = s[3] * w[k];
            r += s[0] * sa;
            g += s[1] * sa;
            b += s[2] * sa;
            a += sa;
        }
        unsigned char* o = &out.rgba[px * 4];
        if (a <= 0.0f) {
            o[0] = o[1] = o[2] = o[3] = 0;
            continue;
        }
        const float inv = 1.0f / a;
        o[0] = (unsigned char)std::min(255.0f, r * inv + 0.5f);
        o[1] = (unsigned char)std::min(255.0f, g * inv + 0.5f);
        o[2] = (unsigned char)std::min(255.0f, b * inv + 0.5f);
        o[3] = (unsigned char)std::min(255.0f, a + 0.5f);
    }
}

float luminance(Rgb c)
{
    const auto lin = [](float v) { return v <= 0.03928f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
    return 0.2126f * lin(c.r) + 0.7152f * lin(c.g) + 0.0722f * lin(c.b);
}

float contrastRatio(Rgb a, Rgb b)
{
    const float la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
}

} // namespace dewdrop
} // namespace launcher
} // namespace pikmin
