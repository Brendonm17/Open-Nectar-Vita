#include "launcher_page_layout.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

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

    std::cout << "launcher_page_layout_test: ok\n";
    return 0;
}
