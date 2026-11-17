#include "ui.h"

#include <vector>

#include "gps.h"
#include "net.h"

namespace ui {

using board::display;
using board::epd_mode_t;
using lgfx::textdatum_t;

const lgfx::IFont *const fontSmall = &fonts::lgfxJapanGothicP_20;
const lgfx::IFont *const fontNormal = &fonts::lgfxJapanGothicP_24;
const lgfx::IFont *const fontMedium = &fonts::lgfxJapanGothicP_28;
const lgfx::IFont *const fontLarge = &fonts::lgfxJapanGothicP_36;

namespace {

constexpr int kBackWidth = 200;          // ステータスバー左側の「戻る」の反応幅
constexpr int kCleanRefreshEvery = 8;    // 全体描画の何回に 1 回を高品質モードにするか
constexpr uint32_t kBatteryIntervalMs = 10000;

App *g_home = nullptr;
App *g_current = nullptr;
bool g_fullDirty = true;
bool g_statusDirty = false;
bool g_cleanRequested = true;  // 起動直後は高品質モードで描く
int g_fullCount = 0;
int g_lastMinute = -1;
bool g_lastWifi = false;
int g_lastGps = -1;  // ステータスバーに出した GPS の状態（0: 停止, 1: 探索中, 2: 測位済み）
board::Battery g_battery;
uint32_t g_batteryReadMs = 0;

// 押したときの反転表示
std::vector<Rect> g_touchables;   // いま画面にある、タップに反応する領域
Rect g_pressed = {0, 0, 0, 0};    // 反転表示中の領域（w == 0 なら反転なし）

// 指定した領域の白黒を反転して、高速モードですぐ画面に出す。
void invertRect(const Rect &r)
{
    static uint8_t rgb[kWidth * 3];
    static uint8_t line[kWidth];
    const int x0 = std::max(r.x, 0), x1 = std::min(r.x + r.w, kWidth);
    const int y0 = std::max(r.y, 0), y1 = std::min(r.y + r.h, kHeight);
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    board::setDrawMode(epd_mode_t::epd_fast);
    for (int y = y0; y < y1; ++y) {
        display.readRectRGB(x0, y, x1 - x0, 1, rgb);
        for (int i = 0; i < x1 - x0; ++i) {
            line[i] = static_cast<uint8_t>(255 - rgb[i * 3]);
        }
        display.pushImage(x0, y, x1 - x0, 1, reinterpret_cast<const lgfx::grayscale_t *>(line));
    }
    board::present();
}

// タップ位置にある領域を反転表示する。
void showPress(int x, int y)
{
    // 後から描いたもの（手前にあるもの）を優先する。
    for (auto it = g_touchables.rbegin(); it != g_touchables.rend(); ++it) {
        if (it->contains(x, y)) {
            g_pressed = *it;
            invertRect(g_pressed);
            return;
        }
    }
}

void drawWifiIcon(int cx, int cy, bool on)
{
    const uint32_t color = on ? TFT_BLACK : gray(170);
    display.fillArc(cx, cy, 20, 23, 225, 315, color);
    display.fillArc(cx, cy, 12, 15, 225, 315, color);
    display.fillCircle(cx, cy - 3, 4, color);
    if (!on) {
        display.drawLine(cx - 14, cy - 22, cx + 14, cy + 2, TFT_BLACK);
        display.drawLine(cx - 13, cy - 22, cx + 15, cy + 2, TFT_BLACK);
    }
}

// 右端 x を基準に、左へ向かってバッテリー表示を描く。描いた幅を返す。
int drawBattery(int right, int cy)
{
    const int bodyW = 40, bodyH = 20;
    const int bodyX = right - bodyW - 4;
    const int bodyY = cy - bodyH / 2;
    display.drawRect(bodyX, bodyY, bodyW, bodyH, TFT_BLACK);
    display.drawRect(bodyX + 1, bodyY + 1, bodyW - 2, bodyH - 2, TFT_BLACK);
    display.fillRect(bodyX + bodyW, cy - 5, 4, 10, TFT_BLACK);

    String label = "--";
    if (g_battery.valid) {
        const int fill = (bodyW - 6) * g_battery.percent / 100;
        display.fillRect(bodyX + 3, bodyY + 3, fill, bodyH - 6, TFT_BLACK);
        label = String(g_battery.percent) + "%";
    }
    text(label, bodyX - 8, cy, fontSmall, textdatum_t::middle_right);
    display.setFont(fontSmall);
    int used = (right - bodyX) + 8 + display.textWidth(label);
    if (g_battery.charging) {
        // 充電中は稲妻のマークを添える
        const int bx = right - used - 14;
        display.fillTriangle(bx + 8, cy - 12, bx, cy + 2, bx + 7, cy + 2, TFT_BLACK);
        display.fillTriangle(bx + 4, cy + 12, bx + 12, cy - 2, bx + 5, cy - 2, TFT_BLACK);
        used += 22;
    }
    return used;
}

void drawStatusBar()
{
    display.fillRect(0, 0, kWidth, kStatusHeight, TFT_WHITE);
    display.drawFastHLine(0, kStatusHeight - 2, kWidth, TFT_BLACK);
    display.drawFastHLine(0, kStatusHeight - 1, kWidth, TFT_BLACK);
    const int cy = (kStatusHeight - 2) / 2;

    // 右: バッテリーと WiFi
    const int used = drawBattery(kWidth - 10, cy);
    drawWifiIcon(kWidth - 10 - used - 30, cy + 10, net::connected());
    if (gps::running()) {
        // GPS 使用中: 測位できていれば黒、探索中は灰色
        text("GPS", kWidth - 10 - used - 62, cy, fontSmall, textdatum_t::middle_right,
             gps::hasFix() ? TFT_BLACK : gray(150));
    }

    // 左: アプリ内では「戻る」とタイトル、ホームでは時刻
    char clock[8] = "--:--";
    if (net::timeValid()) {
        const time_t now = time(nullptr);
        struct tm t;
        localtime_r(&now, &t);
        snprintf(clock, sizeof(clock), "%02d:%02d", t.tm_hour, t.tm_min);
    }

    if (isHome()) {
        text(clock, 14, cy, fontMedium, textdatum_t::middle_left);
    } else {
        // 「<」の矢印
        for (int i = 0; i < 3; ++i) {
            display.drawLine(26 + i, cy - 12, 12 + i, cy, TFT_BLACK);
            display.drawLine(12 + i, cy, 26 + i, cy + 12, TFT_BLACK);
        }
        text(g_current ? g_current->title() : "", 40, cy, fontNormal, textdatum_t::middle_left);
        text(clock, kWidth / 2 - 30, cy, fontNormal, textdatum_t::middle_center);
    }
}

// 一定間隔でバッテリーを読む。表示内容（残量 %、充電中かどうか）が変わったら true。
bool refreshBattery()
{
    const uint32_t now = millis();
    if (g_batteryReadMs != 0 && now - g_batteryReadMs < kBatteryIntervalMs) {
        return false;
    }
    g_batteryReadMs = now ? now : 1;
    const board::Battery previous = g_battery;
    g_battery = board::readBattery();
    return previous.valid != g_battery.valid || previous.percent != g_battery.percent ||
           previous.charging != g_battery.charging;
}

}  // namespace

uint32_t gray(uint8_t level)
{
    return display.color888(level, level, level);
}

void text(const String &s, int x, int y, const lgfx::IFont *font, textdatum_t datum,
          uint32_t color)
{
    display.setFont(font);
    display.setTextSize(1);
    display.setTextDatum(datum);
    display.setTextColor(color);
    display.drawString(s, x, y);
}

String fitText(const String &s, int maxWidth, const lgfx::IFont *font)
{
    display.setFont(font);
    display.setTextSize(1);
    if (display.textWidth(s) <= maxWidth) {
        return s;
    }
    String out = s;
    while (out.length() > 0) {
        // UTF-8 の文字境界まで 1 文字ぶん削る。
        int cut = out.length() - 1;
        while (cut > 0 && (static_cast<uint8_t>(out[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        out.remove(cut);
        if (display.textWidth(out + "…") <= maxWidth) {
            break;
        }
    }
    return out + "…";
}

void touchable(const Rect &r)
{
    // 部分的な描き直しで同じ領域が何度も登録されないようにする。
    for (const Rect &known : g_touchables) {
        if (known.x == r.x && known.y == r.y && known.w == r.w && known.h == r.h) {
            return;
        }
    }
    g_touchables.push_back(r);
}

void button(const Rect &r, const String &label, bool filled, const lgfx::IFont *font)
{
    touchable(r);
    if (font == nullptr) {
        font = fontNormal;
    }
    if (filled) {
        display.fillRoundRect(r.x, r.y, r.w, r.h, 12, TFT_BLACK);
    } else {
        display.fillRoundRect(r.x, r.y, r.w, r.h, 12, TFT_WHITE);
        display.drawRoundRect(r.x, r.y, r.w, r.h, 12, TFT_BLACK);
        display.drawRoundRect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, 11, TFT_BLACK);
    }
    text(label, r.x + r.w / 2, r.y + r.h / 2, font, textdatum_t::middle_center,
         filled ? kWhite : TFT_BLACK);
}

void message(const String &line1, const String &line2)
{
    beginPartial();
    const Rect box = {40, kHeight / 2 - 90, kWidth - 80, 180};
    display.fillRoundRect(box.x, box.y, box.w, box.h, 16, TFT_WHITE);
    for (int i = 0; i < 3; ++i) {
        display.drawRoundRect(box.x + i, box.y + i, box.w - i * 2, box.h - i * 2, 16 - i,
                              TFT_BLACK);
    }
    const int cx = kWidth / 2;
    if (line2.length() == 0) {
        text(fitText(line1, box.w - 40, fontMedium), cx, kHeight / 2, fontMedium,
             textdatum_t::middle_center);
    } else {
        text(fitText(line1, box.w - 40, fontMedium), cx, kHeight / 2 - 26, fontMedium,
             textdatum_t::middle_center);
        text(fitText(line2, box.w - 40, fontNormal), cx, kHeight / 2 + 28, fontNormal,
             textdatum_t::middle_center);
    }
    flush();
}

// ---- アプリ切り替え ---------------------------------------------------------

void setHome(App &home)
{
    g_home = &home;
}

void open(App &app)
{
    if (g_current == &app) {
        requestRedraw();
        return;
    }
    if (g_current) {
        g_current->leave();
    }
    g_current = &app;
    g_current->enter();
    requestRedraw();
}

void goHome()
{
    if (g_home) {
        open(*g_home);
    }
}

bool isHome()
{
    return g_current == g_home;
}

void requestRedraw()
{
    g_fullDirty = true;
}

void requestStatusRedraw()
{
    g_statusDirty = true;
}

void requestCleanRefresh()
{
    g_cleanRequested = true;
    g_fullDirty = true;
}

void beginPartial(epd_mode_t mode)
{
    board::setDrawMode(mode);
}

void flush()
{
    board::present();
}

namespace {
bool g_activity = false;
}

void noteActivity()
{
    g_activity = true;
}

bool takeActivity()
{
    const bool active = g_activity;
    g_activity = false;
    return active;
}

void dispatch(const board::TouchEvent &ev)
{
    if (!g_current || ev.type == board::TouchEvent::None) {
        return;
    }
    if (ev.type == board::TouchEvent::Tap) {
        // 押された場所をまず反転表示してから、処理に進む。
        showPress(ev.x, ev.y);
        if (ev.y < kStatusHeight) {
            if (!isHome() && ev.x < kBackWidth && !g_current->back()) {
                goHome();
            }
            return;
        }
        g_current->tap(ev.x, ev.y);
    } else if (ev.y >= kStatusHeight) {
        g_current->drag(ev.x, ev.y, ev.dx, ev.dy);
    }
}

void update()
{
    if (g_current) {
        g_current->tick();
    }

    // 分が変わったとき、WiFi の接続状態が変わったときにステータスバーを更新する。
    const time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_min != g_lastMinute) {
        g_lastMinute = t.tm_min;
        g_statusDirty = true;
    }
    const bool wifi = net::connected();
    if (wifi != g_lastWifi) {
        g_lastWifi = wifi;
        g_statusDirty = true;
    }
    if (refreshBattery()) {
        g_statusDirty = true;
    }
    const int gpsState = !gps::running() ? 0 : (gps::hasFix() ? 2 : 1);
    if (gpsState != g_lastGps) {
        g_lastGps = gpsState;
        g_statusDirty = true;
    }

    if (g_fullDirty) {
        g_fullDirty = false;
        g_statusDirty = false;

        // 更新モードは描画の前に決める（board::setDrawMode の説明を参照）。
        epd_mode_t mode = epd_mode_t::epd_text;
        if (g_cleanRequested || ++g_fullCount >= kCleanRefreshEvery) {
            mode = epd_mode_t::epd_quality;
            g_cleanRequested = false;
            g_fullCount = 0;
        }
        board::setDrawMode(mode);

        g_touchables.clear();
        g_pressed.w = 0;  // 全体を描き直すので、反転表示は自然に消える
        display.fillScreen(TFT_WHITE);
        drawStatusBar();
        if (!isHome()) {
            touchable({0, 0, kBackWidth, kStatusHeight - 2});  // 「戻る」
        }
        if (g_current) {
            g_current->draw();
        }
        board::present();
    } else if (g_pressed.w > 0) {
        // 押しても画面が変わらなかった場合（ズームの上限など）は、反転を元に戻す。
        invertRect(g_pressed);
        g_pressed.w = 0;
    } else if (g_statusDirty) {
        g_statusDirty = false;
        beginPartial();
        drawStatusBar();
        flush();
    }
}

// ---- ソフトウェアキーボード -------------------------------------------------

namespace {

constexpr int kKeyW = 54;
constexpr int kKeyH = 78;
constexpr int kKeysTop = kHeight - kKeyH * 5 - 6;
constexpr int kFieldTop = kContentTop + 70;
constexpr int kFieldHeight = 70;

// 各ページの 1〜4 段目。4 段目は左に切替キー、右に削除キーが付く（最大 7 文字）。
const char *const kPages[3][4] = {
    {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"},
    {"1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"},
    {"!@#$%^&*()", "-_=+[]{}\\|", ";:'\",.<>/", "?`~"},
};

int rowLeft(const char *row, int rowIndex)
{
    const int count = static_cast<int>(strlen(row));
    if (rowIndex == 3) {
        return 81 + (7 - count) * kKeyW / 2;
    }
    return (kWidth - count * kKeyW) / 2;
}

void drawKey(const Rect &r, const String &label, const lgfx::IFont *font)
{
    display.drawRoundRect(r.x + 2, r.y + 2, r.w - 4, r.h - 4, 8, TFT_BLACK);
    text(label, r.x + r.w / 2, r.y + r.h / 2, font, textdatum_t::middle_center);
}

// 最下段のキー配置
const Rect kKeySymbol = {0, kKeysTop + kKeyH * 4, 108, kKeyH};
const Rect kKeySpace = {108, kKeysTop + kKeyH * 4, 196, kKeyH};
const Rect kKeyCancel = {304, kKeysTop + kKeyH * 4, 128, kKeyH};
const Rect kKeyDone = {432, kKeysTop + kKeyH * 4, 108, kKeyH};
const Rect kKeyShift = {0, kKeysTop + kKeyH * 3, 81, kKeyH};
const Rect kKeyBackspace = {459, kKeysTop + kKeyH * 3, 81, kKeyH};

}  // namespace

void Keyboard::begin(const String &label, const String &initial)
{
    label_ = label;
    value_ = initial;
    page_ = 0;
}

void Keyboard::drawField()
{
    display.fillRect(0, kFieldTop - 4, kWidth, kFieldHeight + 8, TFT_WHITE);
    display.drawRect(20, kFieldTop, kWidth - 40, kFieldHeight, TFT_BLACK);
    display.drawRect(21, kFieldTop + 1, kWidth - 42, kFieldHeight - 2, TFT_BLACK);

    // 長い入力は末尾側を見せる。
    display.setFont(fontMedium);
    display.setTextSize(1);
    String shown = value_ + "_";
    while (shown.length() > 1 && display.textWidth(shown) > kWidth - 70) {
        shown.remove(0, 1);
    }
    text(shown, 34, kFieldTop + kFieldHeight / 2, fontMedium, textdatum_t::middle_left);
}

void Keyboard::drawKeys()
{
    display.fillRect(0, kKeysTop, kWidth, kHeight - kKeysTop, TFT_WHITE);
    for (int row = 0; row < 4; ++row) {
        const char *chars = kPages[page_][row];
        const int left = rowLeft(chars, row);
        for (int i = 0; chars[i] != '\0'; ++i) {
            const Rect r = {left + i * kKeyW, kKeysTop + row * kKeyH, kKeyW, kKeyH};
            drawKey(r, String(chars[i]), fontMedium);
        }
    }
    if (page_ != 2) {
        drawKey(kKeyShift, page_ == 1 ? "小" : "大", fontNormal);
    }
    drawKey(kKeyBackspace, "削除", fontSmall);
    drawKey(kKeySymbol, page_ == 2 ? "英字" : "記号", fontNormal);
    drawKey(kKeySpace, "空白", fontNormal);
    drawKey(kKeyCancel, "キャンセル", fontSmall);
    button({kKeyDone.x + 2, kKeyDone.y + 2, kKeyDone.w - 4, kKeyDone.h - 4}, "決定", true);
}

void Keyboard::draw()
{
    text(fitText(label_, kWidth - 40, fontNormal), 20, kContentTop + 24, fontNormal);
    drawField();
    drawKeys();
}

Keyboard::Result Keyboard::tap(int x, int y)
{
    if (y < kKeysTop) {
        return None;
    }
    if (kKeyDone.contains(x, y)) {
        return Done;
    }
    if (kKeyCancel.contains(x, y)) {
        return Cancel;
    }
    if (kKeySymbol.contains(x, y)) {
        page_ = page_ == 2 ? 0 : 2;
        beginPartial();
        drawKeys();
        flush();
        return None;
    }
    if (kKeyShift.contains(x, y) && page_ != 2) {
        page_ = page_ == 1 ? 0 : 1;
        beginPartial();
        drawKeys();
        flush();
        return None;
    }

    bool changed = false;
    if (kKeyBackspace.contains(x, y)) {
        if (value_.length() > 0) {
            value_.remove(value_.length() - 1);
            changed = true;
        }
    } else if (kKeySpace.contains(x, y)) {
        value_ += ' ';
        changed = true;
    } else {
        const int row = (y - kKeysTop) / kKeyH;
        if (row >= 0 && row < 4) {
            const char *chars = kPages[page_][row];
            const int index = (x - rowLeft(chars, row)) / kKeyW;
            if (x >= rowLeft(chars, row) && index < static_cast<int>(strlen(chars))) {
                value_ += chars[index];
                changed = true;
            }
        }
    }
    if (value_.length() > 64) {  // WPA パスフレーズの上限を超える入力は受け付けない
        value_.remove(64);
        changed = false;
    }
    if (changed) {
        beginPartial();
        drawField();
        flush();
    }
    return None;
}

}  // namespace ui
