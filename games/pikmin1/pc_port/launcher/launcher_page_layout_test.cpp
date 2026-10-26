#include "launcher_page_layout.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace pikmin::launcher;

static void check(bool value, const char* message)
{
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

static bool near(float a, float b) { return std::fabs(a - b) < 0.01f; }

static void checkGames(float width, float available, float rows, bool update)
{
    const GamesMetrics m;
    const GamesLayout l = fitGames(width, available, rows, update);
    check(near(l.content, l.space.top + l.coverH + gamesBelowCovers(l.space, rows, update)), "content adds up");
    check(near(l.coverW * m.aspect, l.coverH), "covers keep their shape");
    check(l.coverW <= l.columnW + 0.01f, "covers never wider than their column");
    check(l.coverH >= m.minCoverH - 0.01f, "covers never below the minimum");
    if (!l.scroll) check(l.content <= available + 0.01f, "a page that does not scroll fits");
    else check(near(l.coverH, m.minCoverH) && l.content > available, "scroll only at the minimum");
}

int main()
{
    // Big window: the old layout, unchanged (224 px wide covers at 1180).
    GamesLayout g = fitGames(1180.0f, 900.0f, 2.0f, true);
    check(!g.scroll && near(g.coverW, 224.2f) && near(g.space.top, 24.0f) && near(g.space.update, 32.0f), "roomy page unchanged");

    // 1180x780: the page has 549 px under the tabs. The old 314 px covers pushed
    // Update 7 px past the bottom; now the covers shrink and everything fits.
    g = fitGames(1180.0f, 549.0f, 2.0f, true);
    check(!g.scroll && g.content <= 549.0f && g.coverH < 314.0f && near(g.space.top, 24.0f), "default window fits");
    check(near(g.columnW, 224.2f), "buttons keep their width when the covers shrink");

    // A bit shorter: covers stop at the comfortable size and the gaps tighten.
    g = fitGames(960.0f, 400.0f, 2.0f, true);
    check(!g.scroll && near(g.coverH, 180.0f) && g.space.top < 24.0f && g.space.top > 12.0f, "gaps tighten second");

    // Shorter still: tight gaps, smaller covers, then scroll.
    g = fitGames(960.0f, 350.0f, 2.0f, true);
    check(!g.scroll && g.coverH < 180.0f && near(g.space.top, 12.0f), "covers shrink last");
    g = fitGames(960.0f, 250.0f, 2.0f, true);
    check(g.scroll && near(g.coverH, 120.0f), "too short: minimum covers and scroll");

    // Every height, with and without Update or the second row of buttons: no
    // jumps while resizing (the covers and the content only grow with the room).
    for (float rows : { 1.0f, 2.0f, 4.0f }) {
        for (bool update : { false, true }) {
            float lastH = fitGames(1180.0f, 0.0f, rows, update).coverH;
            float lastContent = fitGames(1180.0f, 0.0f, rows, update).content;
            for (float a = 0.0f; a <= 1200.0f; a += 0.5f) {
                checkGames(1180.0f, a, rows, update);
                checkGames(640.0f, a, rows, update);
                const GamesLayout l = fitGames(1180.0f, a, rows, update);
                check(l.coverH >= lastH - 0.01f && l.coverH - lastH < 1.0f, "covers grow smoothly");
                check(l.content >= lastContent - 0.01f && l.content - lastContent < 1.0f, "page grows smoothly");
                lastH = l.coverH;
                lastContent = l.content;
            }
        }
    }

    // Move Install: the gap above shrinks before the page scrolls.
    PageFit p = fitPage(380.0f, 500.0f, 30.0f, 12.0f);
    check(!p.scroll && near(p.top, 30.0f) && near(p.content, 410.0f), "move: roomy");
    p = fitPage(380.0f, 400.0f, 30.0f, 12.0f);
    check(!p.scroll && near(p.top, 20.0f) && near(p.content, 400.0f), "move: gap shrinks");
    p = fitPage(380.0f, 350.0f, 30.0f, 12.0f);
    check(p.scroll && near(p.top, 12.0f) && near(p.content, 392.0f), "move: scrolls");

    // Logo: as before from 700 px tall, smaller below, never tiny.
    check(logoHeightCap(700.0f) > 1000.0f && logoHeightCap(780.0f) > 1000.0f, "logo unchanged from 700 px");
    check(near(logoHeightCap(699.99f), 118.0f) && near(logoHeightCap(600.0f), 84.0f), "logo shrinks below 700 px");
    check(near(logoHeightCap(300.0f), 56.0f), "logo minimum");

    // Settings: the help box gives way first, then hides; rows never collapse.
    SettingsPanelFit s = fitSettingsPanel(500.0f);
    check(near(s.helpH, kSettingsHelpFull) && near(s.rowsH, 500.0f - 12.0f - 104.0f), "settings: roomy");
    s = fitSettingsPanel(300.0f);
    check(near(s.helpH, kSettingsHelpMin) && near(s.rowsH, 300.0f - 12.0f - 54.0f), "settings: help shrinks");
    s = fitSettingsPanel(206.0f);
    check(near(s.helpH, kSettingsHelpMin) && near(s.rowsH, kSettingsRowsMin), "settings: rows at their minimum");
    s = fitSettingsPanel(200.0f);
    check(s.helpH == 0.0f && near(s.rowsH, 200.0f), "settings: help hidden");
    s = fitSettingsPanel(-40.0f);
    check(s.helpH == 0.0f && s.rowsH >= 1.0f, "settings: never a zero or negative child height");
    float lastRows = 0.0f;
    for (float total = 206.0f; total <= 900.0f; total += 1.0f) {
        s = fitSettingsPanel(total);
        check(s.rowsH >= kSettingsRowsMin - 0.01f && s.rowsH + 0.01f >= lastRows, "settings: rows grow with the height");
        check(near(s.rowsH + kSettingsGap + s.helpH, total), "settings: fills the space");
        lastRows = s.rowsH;
    }

    // ── Anchos estrechos ──
    {
        auto measure = [](const std::string& s) { return 10.0f * float(s.size()); };
        // Panel de 640x700: interior ~340. "Instant Whistle Response" (240) no
        // cabe al lado del control: va encima, nunca debajo.
        SettingRowFit r = fitSettingRow(340.0f, 240.0f);
        check(r.mode == SettingRowMode::Stacked && near(r.controlW, 300.0f), "narrow panel stacks the label");
        r = fitSettingRow(340.0f, 120.0f);
        check(r.mode == SettingRowMode::Inline && 120.0f + kSettingLabelGap + r.controlW <= 340.0f, "short label stays inline");
        r = fitSettingRow(600.0f, 400.0f);
        check(r.mode == SettingRowMode::Ellipsized && r.labelMaxW + kSettingLabelGap + r.controlW <= 600.01f, "wide panel ellipsizes");
        for (float w = 100.0f; w <= 900.0f; w += 7.0f)
            for (float lw = 10.0f; lw <= 600.0f; lw += 13.0f) {
                const SettingRowFit f = fitSettingRow(w, lw);
                if (f.mode == SettingRowMode::Inline) check(lw + kSettingLabelGap + f.controlW <= w + 0.01f, "inline never overlaps");
                if (f.mode == SettingRowMode::Ellipsized) check(f.labelMaxW + kSettingLabelGap + f.controlW <= w + 0.01f, "cut label never overlaps");
                if (f.mode == SettingRowMode::Stacked) check(f.controlW <= w + 0.01f, "stacked control fits");
            }
        check(ellipsizeEnd("Better Pathfinding", 500.0f, measure) == "Better Pathfinding", "fitting text unchanged");
        const std::string cut = ellipsizeEnd("Chain Pikmin Actions", 120.0f, measure);
        check(measure(cut) <= 120.0f && cut.size() > 3 && cut.substr(cut.size() - 3) == "...", "ellipsized fits");
        check(cut == "Chain Pik...", "cut keeps as much as fits");
        check(ellipsizeEnd("Chain Pikmin Actions", 90.0f, measure) == "Chain...", "cut drops trailing spaces");
        check(ellipsizeEnd("abc", 5.0f, measure) == "...", "nothing fits: dots");
        const std::string utf = ellipsizeEnd("Ñandú corriendo", 60.0f, [](const std::string& s) { return 5.0f * float(s.size()); });
        check((static_cast<unsigned char>(utf[utf.size() - 4]) & 0xC0) != 0xC0, "never cut inside a UTF-8 character");
    }
    {
        // Botones del diálogo.
        int rows = 0;
        auto b = fitDialogButtons({ 180.0f, 150.0f }, 130.0f, 572.0f, rows);
        check(rows == 1 && b.size() == 3 && near(b[1].x + b[1].w, 572.0f) && near(b[2].x, 0.0f), "dialog: one row when it fits");
        check(b[0].x + b[0].w + kDialogGap <= b[1].x + 0.01f && b[2].w + kDialogCancelGap <= b[0].x + 0.01f, "dialog: no overlap");
        b = fitDialogButtons({ 220.0f, 200.0f }, 130.0f, 480.0f, rows);
        check(rows == 2 && near(b[0].w, b[1].w) && b[0].row == 0 && b[2].row == 1, "dialog: actions split evenly, cancel below");
        check(near(b[1].x + b[1].w, 480.0f), "dialog: even split fills the row");
        b = fitDialogButtons({ 300.0f, 260.0f }, 130.0f, 400.0f, rows);
        check(rows == 3 && b[1].row == 0 && b[0].row == 1 && b[2].row == 2, "dialog: one per row, primary on top");
        for (const DialogButton& d : b) check(d.x + d.w <= 400.01f, "dialog: stacked buttons fit");
        b = fitDialogButtons({ 150.0f }, 0.0f, 300.0f, rows);
        check(rows == 1 && b.size() == 1, "dialog: single button");
    }

    std::cout << "launcher_page_layout_test: ok\n";
    return 0;
}
