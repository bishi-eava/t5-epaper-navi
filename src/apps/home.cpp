// ホーム画面: 大きな時刻と日付、アプリアイコン。
#include "apps.h"
#include "net.h"

namespace apps {
namespace {

using board::display;
using lgfx::textdatum_t;

constexpr int kClockTop = ui::kContentTop + 40;
constexpr int kClockHeight = 180;
constexpr int kIconSize = 132;
constexpr int kIconTop = 360;
constexpr int kIconRowPitch = 230;
constexpr int kColumnCenters[3] = {96, 270, 444};

void iconMap(int cx, int cy)
{
    display.fillCircle(cx, cy - 12, 28, TFT_BLACK);
    display.fillTriangle(cx - 25, cy, cx + 25, cy, cx, cy + 44, TFT_BLACK);
    display.fillCircle(cx, cy - 12, 11, TFT_WHITE);
}

void iconCalendar(int cx, int cy)
{
    display.fillRoundRect(cx - 40, cy - 38, 80, 78, 8, TFT_BLACK);
    display.fillRect(cx - 34, cy - 14, 68, 48, TFT_WHITE);
    display.fillRect(cx - 24, cy - 48, 8, 20, TFT_BLACK);
    display.fillRect(cx + 16, cy - 48, 8, 20, TFT_BLACK);
    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 3; ++col) {
            display.fillRect(cx - 26 + col * 22, cy - 4 + row * 20, 10, 10, TFT_BLACK);
        }
    }
}

void iconSettings(int cx, int cy)
{
    for (int i = 0; i < 8; ++i) {
        const float a = i * 0.7853982f;
        display.fillCircle(cx + static_cast<int>(cosf(a) * 36), cy + static_cast<int>(sinf(a) * 36),
                           10, TFT_BLACK);
    }
    display.fillCircle(cx, cy, 34, TFT_BLACK);
    display.fillCircle(cx, cy, 14, TFT_WHITE);
}

void iconFiler(int cx, int cy)
{
    display.fillRoundRect(cx - 42, cy - 36, 40, 20, 6, TFT_BLACK);
    display.fillRoundRect(cx - 42, cy - 24, 84, 60, 6, TFT_BLACK);
    display.fillRect(cx - 36, cy - 10, 72, 4, TFT_WHITE);
}

void iconViewer(int cx, int cy)
{
    display.fillRoundRect(cx - 44, cy - 36, 88, 72, 6, TFT_BLACK);
    display.fillRect(cx - 38, cy - 30, 76, 60, TFT_WHITE);
    display.fillCircle(cx + 20, cy - 14, 8, TFT_BLACK);
    display.fillTriangle(cx - 38, cy + 30, cx - 10, cy - 8, cx + 14, cy + 30, TFT_BLACK);
    display.fillTriangle(cx, cy + 30, cx + 20, cy + 4, cx + 38, cy + 30, TFT_BLACK);
}

struct Entry {
    const char *label;
    void (*drawIcon)(int, int);
    ui::App &(*app)();
};

const Entry kEntries[] = {
    {"マップ", iconMap, map},
    {"カレンダー", iconCalendar, calendar},
    {"設定", iconSettings, settings},
    {"ファイル", iconFiler, filer},
    {"画像", iconViewer, viewer},
};
constexpr int kEntryCount = sizeof(kEntries) / sizeof(kEntries[0]);

ui::Rect iconRect(int index)
{
    const int cx = kColumnCenters[index % 3];
    const int top = kIconTop + (index / 3) * kIconRowPitch;
    return {cx - kIconSize / 2, top, kIconSize, kIconSize};
}

class HomeApp : public ui::App {
public:
    const char *title() const override { return "ホーム"; }

    void enter() override { lastMinute_ = -1; }

    void draw() override
    {
        drawClock();
        for (int i = 0; i < kEntryCount; ++i) {
            const ui::Rect r = iconRect(i);
            ui::touchable(r);
            display.drawRoundRect(r.x, r.y, r.w, r.h, 26, TFT_BLACK);
            display.drawRoundRect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, 25, TFT_BLACK);
            display.drawRoundRect(r.x + 2, r.y + 2, r.w - 4, r.h - 4, 24, TFT_BLACK);
            kEntries[i].drawIcon(r.x + r.w / 2, r.y + r.h / 2);
            ui::text(kEntries[i].label, r.x + r.w / 2, r.y + r.h + 12, ui::fontNormal,
                     textdatum_t::top_center);
        }
    }

    void tap(int x, int y) override
    {
        for (int i = 0; i < kEntryCount; ++i) {
            ui::Rect r = iconRect(i);
            r.h += 44;  // ラベルも反応範囲に含める
            if (r.contains(x, y)) {
                ui::open(kEntries[i].app());
                return;
            }
        }
    }

    void tick() override
    {
        // 分が変わったら時計の部分だけ描き直す。
        const int minute = currentMinute();
        if (lastMinute_ >= 0 && minute != lastMinute_) {
            ui::beginPartial();
            drawClock();
            ui::flush();
        }
    }

private:
    static int currentMinute()
    {
        const time_t now = time(nullptr);
        struct tm t;
        localtime_r(&now, &t);
        return t.tm_hour * 60 + t.tm_min;
    }

    void drawClock()
    {
        lastMinute_ = currentMinute();
        display.fillRect(0, kClockTop, ui::kWidth, kClockHeight, TFT_WHITE);

        if (!net::timeValid()) {
            ui::text("--:--", ui::kWidth / 2, kClockTop + 50, &fonts::Font8,
                     textdatum_t::middle_center);
            ui::text("時刻が未設定です（WiFi 接続で自動設定）", ui::kWidth / 2, kClockTop + 140,
                     ui::fontSmall, textdatum_t::middle_center);
            return;
        }

        const time_t now = time(nullptr);
        struct tm t;
        localtime_r(&now, &t);
        char buf[48];
        snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
        ui::text(buf, ui::kWidth / 2, kClockTop + 50, &fonts::Font8, textdatum_t::middle_center);

        static const char *const kWeekdays[] = {"日", "月", "火", "水", "木", "金", "土"};
        snprintf(buf, sizeof(buf), "%d年%d月%d日 (%s)", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 kWeekdays[t.tm_wday]);
        ui::text(buf, ui::kWidth / 2, kClockTop + 140, ui::fontMedium, textdatum_t::middle_center);
    }

    int lastMinute_ = -1;
};

HomeApp g_home;

}  // namespace

ui::App &home() { return g_home; }

}  // namespace apps
