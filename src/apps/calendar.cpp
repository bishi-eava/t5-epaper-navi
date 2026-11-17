// カレンダー: 月表示のみ。
#include "apps.h"
#include "net.h"

namespace apps {
namespace {

using board::display;
using lgfx::textdatum_t;

constexpr int kHeaderTop = ui::kContentTop + 20;
constexpr int kWeekdayTop = kHeaderTop + 90;
constexpr int kGridTop = kWeekdayTop + 50;
constexpr int kCellW = ui::kWidth / 7;
constexpr int kCellH = 96;

const ui::Rect kPrevButton = {20, ui::kBottomBarTop, 150, 68};
const ui::Rect kTodayButton = {195, ui::kBottomBarTop, 150, 68};
const ui::Rect kNextButton = {370, ui::kBottomBarTop, 150, 68};

int daysInMonth(int year, int month)
{
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
        return 29;
    }
    return kDays[month - 1];
}

// 曜日を求める（0 = 日曜）。
int weekday(int year, int month, int day)
{
    static const int kOffset[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (month < 3) {
        year -= 1;
    }
    return (year + year / 4 - year / 100 + year / 400 + kOffset[month - 1] + day) % 7;
}

class CalendarApp : public ui::App {
public:
    const char *title() const override { return "カレンダー"; }

    void enter() override { showToday(); }

    void draw() override
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d年 %d月", year_, month_);
        ui::text(buf, ui::kWidth / 2, kHeaderTop + 30, ui::fontLarge, textdatum_t::middle_center);

        static const char *const kWeekdays[] = {"日", "月", "火", "水", "木", "金", "土"};
        for (int i = 0; i < 7; ++i) {
            ui::text(kWeekdays[i], i * kCellW + kCellW / 2, kWeekdayTop + 20, ui::fontNormal,
                     textdatum_t::middle_center, (i == 0 || i == 6) ? ui::gray(90) : TFT_BLACK);
        }
        display.drawFastHLine(10, kGridTop - 6, ui::kWidth - 20, TFT_BLACK);

        int todayYear = 0, todayMonth = 0, todayDay = 0;
        today(todayYear, todayMonth, todayDay);

        const int first = weekday(year_, month_, 1);
        const int days = daysInMonth(year_, month_);
        for (int day = 1; day <= days; ++day) {
            const int cell = first + day - 1;
            const int cx = (cell % 7) * kCellW + kCellW / 2;
            const int cy = kGridTop + (cell / 7) * kCellH + kCellH / 2;
            const bool isToday = year_ == todayYear && month_ == todayMonth && day == todayDay;
            if (isToday) {
                display.fillCircle(cx, cy, 34, TFT_BLACK);
            }
            const int column = cell % 7;
            const uint32_t color =
                isToday ? ui::kWhite : ((column == 0 || column == 6) ? ui::gray(90) : TFT_BLACK);
            ui::text(String(day), cx, cy, ui::fontMedium, textdatum_t::middle_center, color);
        }

        ui::button(kPrevButton, "前の月");
        ui::button(kTodayButton, "今月");
        ui::button(kNextButton, "次の月");
    }

    void tap(int x, int y) override
    {
        if (kPrevButton.contains(x, y)) {
            if (--month_ < 1) {
                month_ = 12;
                --year_;
            }
        } else if (kNextButton.contains(x, y)) {
            if (++month_ > 12) {
                month_ = 1;
                ++year_;
            }
        } else if (kTodayButton.contains(x, y)) {
            showToday();
        } else {
            return;
        }
        ui::requestRedraw();
    }

    // 左右のスワイプでも月を送る。
    void drag(int x, int y, int dx, int dy) override
    {
        if (abs(dx) < 80 || abs(dx) < abs(dy)) {
            return;
        }
        if (dx > 0) {
            tap(kPrevButton.x + 1, kPrevButton.y + 1);
        } else {
            tap(kNextButton.x + 1, kNextButton.y + 1);
        }
    }

private:
    static void today(int &year, int &month, int &day)
    {
        const time_t now = time(nullptr);
        struct tm t;
        localtime_r(&now, &t);
        year = t.tm_year + 1900;
        month = t.tm_mon + 1;
        day = t.tm_mday;
    }

    void showToday()
    {
        int day = 0;
        today(year_, month_, day);
        if (!net::timeValid()) {
            year_ = 2026;
            month_ = 1;
        }
    }

    int year_ = 2026;
    int month_ = 1;
};

CalendarApp g_calendar;

}  // namespace

ui::App &calendar() { return g_calendar; }

}  // namespace apps
