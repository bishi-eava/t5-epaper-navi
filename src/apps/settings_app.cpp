// 設定: WiFi（スキャンして選択）、WiFi 転送の名前とパスワード、フロントライト、スリープ時間、ロック画面、時刻、本体情報。
#include <WiFi.h>

#include <algorithm>
#include <vector>

#include "apps.h"
#include "gps.h"
#include "net.h"
#include "settings.h"
#include "webfiles.h"

namespace apps {
namespace {

using board::display;
using lgfx::textdatum_t;

constexpr int kRowH = 84;
constexpr int kListTop = ui::kContentTop + 8;
constexpr int kWifiRowH = 74;
constexpr int kWifiRowsPerPage = 10;

const uint8_t kLightSteps[] = {0, 25, 50, 75, 100};
const uint16_t kSleepSteps[] = {1, 3, 5, 10, 30};

const ui::Rect kBarLeft = {20, ui::kBottomBarTop, 150, 68};
const ui::Rect kBarCenter = {195, ui::kBottomBarTop, 150, 68};
const ui::Rect kBarRight = {370, ui::kBottomBarTop, 150, 68};

// VCOM 調整画面
constexpr int kVcomButtonTop = 250;
const ui::Rect kVcomDown100 = {12, kVcomButtonTop, 120, 76};
const ui::Rect kVcomDown10 = {144, kVcomButtonTop, 120, 76};
const ui::Rect kVcomUp10 = {276, kVcomButtonTop, 120, 76};
const ui::Rect kVcomUp100 = {408, kVcomButtonTop, 120, 76};
const ui::Rect kVcomReset = {120, kVcomButtonTop + 96, 300, 64};

const ui::Rect kActionConnect = {60, 330, 420, 80};
const ui::Rect kActionPassword = {60, 440, 420, 80};
const ui::Rect kActionForget = {60, 550, 420, 80};

// WiFi 転送の設定画面
const ui::Rect kTransferName = {60, 400, 420, 80};
const ui::Rect kTransferPassword = {60, 510, 420, 80};
const ui::Rect kTransferReset = {60, 620, 420, 80};

enum MainRow {
    RowWifi,
    RowTransfer,
    RowLight,
    RowSleep,
    RowLockImage,
    RowTime,
    RowVcom,
    RowRefresh,
    RowInfo,
    RowCount,
};

struct AccessPoint {
    String ssid;
    int rssi = 0;
    bool secure = false;
    bool inRange = true;
};

template <typename T, size_t N>
T nextStep(const T (&steps)[N], T current)
{
    for (size_t i = 0; i < N; ++i) {
        if (steps[i] == current) {
            return steps[(i + 1) % N];
        }
    }
    return steps[0];
}

String formatBytes(uint64_t bytes)
{
    char buf[24];
    if (bytes >= 1024ULL * 1024 * 1024) {
        snprintf(buf, sizeof(buf), "%.1f GB", bytes / (1024.0 * 1024 * 1024));
    } else {
        snprintf(buf, sizeof(buf), "%.0f MB", bytes / (1024.0 * 1024));
    }
    return buf;
}

class SettingsApp : public ui::App {
public:
    const char *title() const override
    {
        switch (screen_) {
            case Screen::Wifi:
            case Screen::WifiAction:
            case Screen::WifiPassword:
                return "WiFi";
            case Screen::Transfer:
            case Screen::TransferName:
            case Screen::TransferPassword:
                return "WiFi 転送";
            case Screen::Info:
                return "本体情報";
            case Screen::Vcom:
                return "画面の濃さ";
            default:
                return "設定";
        }
    }

    void enter() override { screen_ = Screen::Main; }

    bool back() override
    {
        switch (screen_) {
            case Screen::Main:
                return false;
            case Screen::WifiAction:
            case Screen::WifiPassword:
                show(Screen::Wifi);
                return true;
            case Screen::TransferName:
            case Screen::TransferPassword:
                show(Screen::Transfer);
                return true;
            default:
                show(Screen::Main);
                return true;
        }
    }

    void draw() override
    {
        switch (screen_) {
            case Screen::Main:
                drawMain();
                break;
            case Screen::Wifi:
                drawWifi();
                break;
            case Screen::WifiAction:
                drawWifiAction();
                break;
            case Screen::WifiPassword:
            case Screen::TransferName:
            case Screen::TransferPassword:
                keyboard_.draw();
                break;
            case Screen::Transfer:
                drawTransfer();
                break;
            case Screen::Info:
                drawInfo();
                break;
            case Screen::Vcom:
                drawVcom();
                break;
        }
    }

    void tap(int x, int y) override
    {
        switch (screen_) {
            case Screen::Main:
                tapMain(y);
                break;
            case Screen::Wifi:
                tapWifi(x, y);
                break;
            case Screen::WifiAction:
                tapWifiAction(x, y);
                break;
            case Screen::WifiPassword:
                tapPassword(x, y);
                break;
            case Screen::Transfer:
                tapTransfer(x, y);
                break;
            case Screen::TransferName:
            case Screen::TransferPassword:
                tapTransferKeyboard(x, y);
                break;
            case Screen::Info:
                break;
            case Screen::Vcom:
                tapVcom(x, y);
                break;
        }
    }

private:
    enum class Screen {
        Main,
        Wifi,
        WifiAction,
        WifiPassword,
        Transfer,
        TransferName,
        TransferPassword,
        Info,
        Vcom,
    };

    void show(Screen screen)
    {
        screen_ = screen;
        ui::requestRedraw();
    }

    // ---- メインの一覧 ----

    static void drawRow(int index, const String &label, const String &value)
    {
        const int y = kListTop + index * kRowH;
        ui::touchable({0, y, ui::kWidth, kRowH - 1});
        ui::text(label, 24, y + kRowH / 2, ui::fontMedium, textdatum_t::middle_left);
        ui::text(ui::fitText(value, 250, ui::fontNormal), ui::kWidth - 24, y + kRowH / 2,
                 ui::fontNormal, textdatum_t::middle_right, ui::gray(60));
        display.drawFastHLine(16, y + kRowH - 1, ui::kWidth - 32, ui::gray(150));
    }

    void drawMain()
    {
        drawRow(RowWifi, "WiFi", net::connected() ? net::currentSsid() : String("未接続"));
        drawRow(RowTransfer, "WiFi 転送", webfiles::ssid());
        drawRow(RowLight, "フロントライト",
                settings::frontlight() == 0 ? String("オフ") : String(settings::frontlight()) + "%");
        drawRow(RowSleep, "スリープまでの時間", String(settings::sleepMinutes()) + "分");

        const String lock = settings::lockImage();
        drawRow(RowLockImage, "ロック画面の画像",
                lock.length() ? lock.substring(lock.lastIndexOf('/') + 1) : String("既定"));

        String now = "未設定";
        if (net::timeValid()) {
            const time_t t = time(nullptr);
            struct tm tmv;
            localtime_r(&t, &tmv);
            char buf[32];
            snprintf(buf, sizeof(buf), "%d/%d/%d %02d:%02d", tmv.tm_year + 1900, tmv.tm_mon + 1,
                     tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
            now = buf;
        }
        drawRow(RowTime, "時刻合わせ", now);
        drawRow(RowVcom, "画面の濃さ (VCOM)", formatVcom(settings::vcom()));
        drawRow(RowRefresh, "画面の残像を消す", "");
        drawRow(RowInfo, "本体情報", "");
    }

    void tapMain(int y)
    {
        const int row = (y - kListTop) / kRowH;
        if (y < kListTop || row >= RowCount) {
            return;
        }
        switch (row) {
            case RowWifi:
                screen_ = Screen::Wifi;
                scan();
                break;
            case RowTransfer:
                screen_ = Screen::Transfer;
                break;
            case RowLight:
                settings::setFrontlight(nextStep(kLightSteps, settings::frontlight()));
                board::setFrontlight(settings::frontlight());
                break;
            case RowSleep:
                settings::setSleepMinutes(nextStep(kSleepSteps, settings::sleepMinutes()));
                break;
            case RowLockImage:
                if (settings::lockImage().length()) {
                    settings::setLockImage("");
                } else {
                    ui::message("画像ビューワーで設定します", "画像を開いて「ロック画面に設定」");
                    delay(2500);
                }
                break;
            case RowTime:
                if (net::syncTime()) {
                    ui::message("時刻を合わせています…");
                    delay(2500);
                } else {
                    ui::message("WiFi に接続されていません");
                    delay(2000);
                }
                break;
            case RowVcom:
                screen_ = Screen::Vcom;
                ui::requestCleanRefresh();
                break;
            case RowRefresh:
                ui::requestCleanRefresh();
                break;
            case RowInfo:
                screen_ = Screen::Info;
                break;
        }
        ui::requestRedraw();
    }

    // ---- WiFi ----

    void scan()
    {
        ui::message("WiFi を検索しています…");
        aps_.clear();
        page_ = 0;

        const int n = WiFi.scanNetworks();
        for (int i = 0; i < n; ++i) {
            const String ssid = WiFi.SSID(i);
            if (ssid.length() == 0) {
                continue;
            }
            // 同じ SSID は電波の強いものだけ残す。
            auto it = std::find_if(aps_.begin(), aps_.end(),
                                   [&](const AccessPoint &a) { return a.ssid == ssid; });
            if (it != aps_.end()) {
                it->rssi = std::max(it->rssi, static_cast<int>(WiFi.RSSI(i)));
                continue;
            }
            AccessPoint ap;
            ap.ssid = ssid;
            ap.rssi = WiFi.RSSI(i);
            ap.secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
            aps_.push_back(ap);
        }
        WiFi.scanDelete();
        std::sort(aps_.begin(), aps_.end(),
                  [](const AccessPoint &a, const AccessPoint &b) { return a.rssi > b.rssi; });

        // 保存済みで今は見つからないものも、削除できるよう末尾に並べる。
        for (const auto &saved : net::saved()) {
            const bool found = std::any_of(aps_.begin(), aps_.end(), [&](const AccessPoint &a) {
                return a.ssid == saved.ssid;
            });
            if (!found) {
                AccessPoint ap;
                ap.ssid = saved.ssid;
                ap.inRange = false;
                ap.secure = saved.password.length() > 0;
                aps_.push_back(ap);
            }
        }
    }

    int pageCount() const
    {
        return std::max(1, static_cast<int>((aps_.size() + kWifiRowsPerPage - 1) / kWifiRowsPerPage));
    }

    void drawWifi()
    {
        if (aps_.empty()) {
            ui::text("ネットワークが見つかりません", ui::kWidth / 2, 300, ui::fontMedium,
                     textdatum_t::middle_center);
        }
        const String current = net::currentSsid();
        const int start = page_ * kWifiRowsPerPage;
        for (int i = 0; i < kWifiRowsPerPage && start + i < static_cast<int>(aps_.size()); ++i) {
            const AccessPoint &ap = aps_[start + i];
            const int y = kListTop + i * kWifiRowH;
            ui::touchable({0, y, ui::kWidth, kWifiRowH - 1});

            String note;
            if (ap.ssid == current && current.length()) {
                note = "接続中";
            } else if (!ap.inRange) {
                note = "圏外";
            } else if (net::isSaved(ap.ssid)) {
                note = "保存済み";
            } else if (!ap.secure) {
                note = "パスワードなし";
            }

            ui::text(ui::fitText(ap.ssid, 300, ui::fontMedium), 24, y + kWifiRowH / 2,
                     ui::fontMedium, textdatum_t::middle_left);
            ui::text(note, ui::kWidth - 70, y + kWifiRowH / 2, ui::fontSmall,
                     textdatum_t::middle_right, ui::gray(60));
            if (ap.inRange) {
                drawSignal(ui::kWidth - 52, y + kWifiRowH / 2 + 14, ap.rssi);
            }
            display.drawFastHLine(16, y + kWifiRowH - 1, ui::kWidth - 32, ui::gray(150));
        }

        if (pageCount() > 1) {
            ui::button(kBarLeft, "前へ");
            ui::button(kBarRight, "次へ");
        }
        ui::button(kBarCenter, "再検索");
    }

    // 電波強度を 4 本のバーで描く。
    static void drawSignal(int x, int bottom, int rssi)
    {
        const int level = rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -78 ? 2 : 1;
        for (int i = 0; i < 4; ++i) {
            const int h = 8 + i * 7;
            if (i < level) {
                display.fillRect(x + i * 9, bottom - h, 6, h, TFT_BLACK);
            } else {
                display.drawRect(x + i * 9, bottom - h, 6, h, ui::gray(140));
            }
        }
    }

    void tapWifi(int x, int y)
    {
        if (kBarCenter.contains(x, y)) {
            scan();
            ui::requestRedraw();
            return;
        }
        if (pageCount() > 1 && kBarLeft.contains(x, y)) {
            page_ = (page_ + pageCount() - 1) % pageCount();
            ui::requestRedraw();
            return;
        }
        if (pageCount() > 1 && kBarRight.contains(x, y)) {
            page_ = (page_ + 1) % pageCount();
            ui::requestRedraw();
            return;
        }
        if (y < kListTop || y >= kListTop + kWifiRowsPerPage * kWifiRowH) {
            return;
        }
        const int index = page_ * kWifiRowsPerPage + (y - kListTop) / kWifiRowH;
        if (index >= static_cast<int>(aps_.size())) {
            return;
        }
        selected_ = aps_[index];

        if (net::isSaved(selected_.ssid)) {
            show(Screen::WifiAction);
        } else if (selected_.secure) {
            keyboard_.begin(selected_.ssid + " のパスワード");
            show(Screen::WifiPassword);
        } else {
            connect(String());
        }
    }

    void drawWifiAction()
    {
        ui::text(ui::fitText(selected_.ssid, ui::kWidth - 60, ui::fontLarge), ui::kWidth / 2, 180,
                 ui::fontLarge, textdatum_t::middle_center);
        const bool isCurrent = net::currentSsid() == selected_.ssid;
        ui::text(isCurrent ? "接続中" : (selected_.inRange ? "保存済み" : "保存済み（圏外）"),
                 ui::kWidth / 2, 240, ui::fontNormal, textdatum_t::middle_center, ui::gray(60));

        if (selected_.inRange && !isCurrent) {
            ui::button(kActionConnect, "接続する", true, ui::fontMedium);
        }
        if (selected_.inRange) {
            ui::button(kActionPassword, "パスワードを入れ直す", false, ui::fontMedium);
        }
        ui::button(kActionForget, "この設定を削除する", false, ui::fontMedium);
    }

    void tapWifiAction(int x, int y)
    {
        const bool isCurrent = net::currentSsid() == selected_.ssid;
        if (kActionConnect.contains(x, y) && selected_.inRange && !isCurrent) {
            connect(net::savedPassword(selected_.ssid));
        } else if (kActionPassword.contains(x, y) && selected_.inRange) {
            keyboard_.begin(selected_.ssid + " のパスワード");
            show(Screen::WifiPassword);
        } else if (kActionForget.contains(x, y)) {
            net::forget(selected_.ssid);
            // 圏外だった項目は一覧からも外す。
            aps_.erase(std::remove_if(aps_.begin(), aps_.end(),
                                      [&](const AccessPoint &a) {
                                          return !a.inRange && a.ssid == selected_.ssid;
                                      }),
                       aps_.end());
            page_ = std::min(page_, pageCount() - 1);
            show(Screen::Wifi);
        }
    }

    void tapPassword(int x, int y)
    {
        switch (keyboard_.tap(x, y)) {
            case ui::Keyboard::Done:
                connect(keyboard_.value());
                break;
            case ui::Keyboard::Cancel:
                show(Screen::Wifi);
                break;
            default:
                break;
        }
    }

    void connect(const String &password)
    {
        ui::message("接続しています…", selected_.ssid);
        const bool ok = net::connect(selected_.ssid, password);
        ui::message(ok ? "接続しました" : "接続できませんでした",
                    ok ? selected_.ssid : String("パスワードを確認してください"));
        delay(1800);
        show(Screen::Wifi);
    }

    // ---- WiFi 転送（アクセスポイントの名前とパスワード）----

    void drawTransfer()
    {
        const int cx = ui::kWidth / 2;
        ui::text("ファイル画面の「WiFi 転送」で使う", cx, 100, ui::fontNormal,
                 textdatum_t::middle_center, ui::gray(60));
        ui::text("WiFi の名前とパスワード", cx, 134, ui::fontNormal, textdatum_t::middle_center,
                 ui::gray(60));

        const bool isDefault =
            settings::transferSsid().length() == 0 && settings::transferPassword().length() == 0;
        ui::text("WiFi 名", cx, 200, ui::fontNormal, textdatum_t::middle_center, ui::gray(60));
        ui::text(ui::fitText(webfiles::ssid(), ui::kWidth - 40, ui::fontLarge), cx, 242,
                 ui::fontLarge, textdatum_t::middle_center);
        ui::text("パスワード", cx, 298, ui::fontNormal, textdatum_t::middle_center, ui::gray(60));
        ui::text(ui::fitText(webfiles::password(), ui::kWidth - 40, ui::fontLarge), cx, 340,
                 ui::fontLarge, textdatum_t::middle_center);

        ui::button(kTransferName, "名前を変える", false, ui::fontMedium);
        ui::button(kTransferPassword, "パスワードを変える", false, ui::fontMedium);
        if (!isDefault) {
            ui::button(kTransferReset, "既定に戻す", false, ui::fontMedium);
        }
    }

    void tapTransfer(int x, int y)
    {
        if (kTransferName.contains(x, y)) {
            keyboard_.begin("WiFi 転送の名前（1〜32 文字）", webfiles::ssid());
            show(Screen::TransferName);
        } else if (kTransferPassword.contains(x, y)) {
            keyboard_.begin("WiFi 転送のパスワード（8〜63 文字）", webfiles::password());
            show(Screen::TransferPassword);
        } else if (kTransferReset.contains(x, y) &&
                   (settings::transferSsid().length() || settings::transferPassword().length())) {
            settings::resetTransfer();
            ui::requestRedraw();
        }
    }

    void tapTransferKeyboard(int x, int y)
    {
        switch (keyboard_.tap(x, y)) {
            case ui::Keyboard::Done: {
                String value = keyboard_.value();
                bool ok;
                if (screen_ == Screen::TransferName) {
                    value.trim();  // 前後の空白は、つなぐ側で入力しづらいので取り除く
                    ok = settings::setTransferSsid(value);
                } else {
                    ok = settings::setTransferPassword(value);
                }
                if (ok) {
                    show(Screen::Transfer);
                } else {
                    // 長さが合わないときは保存せず、入力を続けられるようにする。
                    ui::message("保存できません", screen_ == Screen::TransferName
                                                      ? "名前は 1〜32 文字にしてください"
                                                      : "パスワードは 8〜63 文字にしてください");
                    delay(2000);
                    ui::requestRedraw();
                }
                break;
            }
            case ui::Keyboard::Cancel:
                show(Screen::Transfer);
                break;
            default:
                break;
        }
    }

    // ---- VCOM（画面の濃さ）の調整 ----

    static String formatVcom(int millivolts)
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "-%d.%02d V", millivolts / 1000, (millivolts % 1000) / 10);
        return buf;
    }

    void drawVcom()
    {
        ui::text("VCOM 電圧", ui::kWidth / 2, 100, ui::fontNormal, textdatum_t::middle_center);
        ui::text(formatVcom(settings::vcom()), ui::kWidth / 2, 170, &fonts::Font7,
                 textdatum_t::middle_center);
        if (settings::vcom() == board::kVcomDefaultMv) {
            ui::text("（既定値）", ui::kWidth - 70, 170, ui::fontSmall, textdatum_t::middle_center);
        }

        ui::button(kVcomDown100, "-0.10", false, ui::fontMedium);
        ui::button(kVcomDown10, "-0.01", false, ui::fontMedium);
        ui::button(kVcomUp10, "+0.01", false, ui::fontMedium);
        ui::button(kVcomUp100, "+0.10", false, ui::fontMedium);
        ui::button(kVcomReset, "既定値 (-1.56 V) に戻す");

        // 見え方を確かめるための見本: 黒と白のベタ、16 階調、細い線と小さい文字
        const int top = kVcomButtonTop + 190;
        display.fillRect(12, top, 258, 110, TFT_BLACK);
        ui::text("白い文字", 141, top + 55, ui::fontMedium, textdatum_t::middle_center, ui::kWhite);
        display.drawRect(270, top, 258, 110, TFT_BLACK);
        ui::text("黒い文字", 399, top + 55, ui::fontMedium, textdatum_t::middle_center);

        const int rampTop = top + 126;
        for (int level = 0; level < 16; ++level) {
            const int x = 12 + level * 516 / 16;
            const int w = 12 + (level + 1) * 516 / 16 - x;
            display.fillRect(x, rampTop, w, 70, ui::gray(static_cast<uint8_t>(level * 17)));
        }
        display.drawRect(12, rampTop, 516, 70, TFT_BLACK);

        const int lineTop = rampTop + 90;
        for (int i = 0; i < 6; ++i) {
            display.fillRect(12, lineTop + i * 12, 250, 1 + i / 2, TFT_BLACK);
        }
        ui::text("文字 ABC abc 0123", 280, lineTop + 16, ui::fontSmall,
                 textdatum_t::middle_left);
        ui::text("細い線と階調の見え方", 280, lineTop + 50, ui::fontSmall,
                 textdatum_t::middle_left);

        const int helpTop = lineTop + 100;
        ui::text("ケーブルのラベルに値があれば、それに合わせます。", 16, helpTop,
                 ui::fontSmall);
        ui::text("なければ、黒が濃く白が白く見える値を探します。", 16, helpTop + 30,
                 ui::fontSmall);
        ui::text("値を変えるたびに画面全体を描き直します。", 16, helpTop + 60, ui::fontSmall);
    }

    void tapVcom(int x, int y)
    {
        int value = settings::vcom();
        if (kVcomDown100.contains(x, y)) {
            value -= 100;
        } else if (kVcomDown10.contains(x, y)) {
            value -= 10;
        } else if (kVcomUp10.contains(x, y)) {
            value += 10;
        } else if (kVcomUp100.contains(x, y)) {
            value += 100;
        } else if (kVcomReset.contains(x, y)) {
            value = board::kVcomDefaultMv;
        } else {
            return;
        }
        settings::setVcom(value);
        board::setVcom(settings::vcom());
        ui::requestCleanRefresh();  // 新しい電圧で、高品質モードで描き直す
    }

    // ---- 本体情報 ----

    void drawInfo()
    {
        int line = 0;
        auto row = [&](const String &label, const String &value) {
            const int y = kListTop + 10 + line++ * 62;
            ui::text(label, 24, y + 31, ui::fontNormal, textdatum_t::middle_left, ui::gray(60));
            ui::text(ui::fitText(value, 320, ui::fontNormal), ui::kWidth - 24, y + 31,
                     ui::fontNormal, textdatum_t::middle_right);
            display.drawFastHLine(16, y + 61, ui::kWidth - 32, ui::gray(150));
        };

        row("ファームウェア", FIRMWARE_VERSION);

        const board::Battery battery = board::readBattery();
        if (battery.valid) {
            row("バッテリー残量", String(battery.percent) + "%");
            row("バッテリー電圧", String(battery.millivolts / 1000.0f, 2) + " V");
            row("バッテリー電流", String(battery.milliamps) + " mA" +
                                      (battery.milliamps > 0 ? "（充電）" : "（放電）"));
        } else {
            row("バッテリー", "読み取れません");
        }
        row("電源", battery.charging ? "充電中" : (battery.usbPower ? "USB 給電" : "バッテリー"));

        if (board::sdReady()) {
            row("SD カード容量", formatBytes(SD.totalBytes()));
            row("SD カード使用量", formatBytes(SD.usedBytes()));
        } else {
            row("SD カード", "なし");
        }

        row("WiFi", net::connected() ? net::currentSsid() : String("未接続"));
        if (net::connected()) {
            row("IP アドレス", WiFi.localIP().toString());
        }
        row("MAC アドレス", WiFi.macAddress());
        row("GPS", strlen(gps::modelName()) ? gps::modelName() : "未判別");
        row("空きメモリ", String(ESP.getFreeHeap() / 1024) + " KB / PSRAM " +
                              String(ESP.getFreePsram() / 1024) + " KB");
    }

    Screen screen_ = Screen::Main;
    std::vector<AccessPoint> aps_;
    AccessPoint selected_;
    int page_ = 0;
    ui::Keyboard keyboard_;
};

SettingsApp g_settings;

}  // namespace

ui::App &settings() { return g_settings; }

}  // namespace apps
