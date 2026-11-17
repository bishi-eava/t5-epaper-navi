// SD カードファイラー: フォルダの閲覧、画像を開く、ファイルの削除。
// 削除は、利用者が詳細画面で操作して確認に答えた場合にだけ行う。
#include <algorithm>
#include <vector>

#include "apps.h"
#include "webfiles.h"

namespace apps {
namespace {

using board::display;
using lgfx::textdatum_t;

constexpr int kPathTop = ui::kContentTop;
constexpr int kPathHeight = 48;
constexpr int kListTop = kPathTop + kPathHeight;
constexpr int kRowH = 64;
constexpr int kRowsPerPage = 12;
constexpr int kMenuLeft = ui::kWidth - 64;  // 行の右端: 詳細を開く領域
constexpr size_t kMaxEntries = 400;

const ui::Rect kUpButton = {12, ui::kBottomBarTop, 150, 68};
const ui::Rect kPrevButton = {250, ui::kBottomBarTop, 130, 68};
const ui::Rect kNextButton = {396, ui::kBottomBarTop, 130, 68};

const ui::Rect kTransferButton = {ui::kWidth - 134, kPathTop + 2, 126, kPathHeight - 8};
const ui::Rect kTransferStop = {120, 800, 300, 76};

const ui::Rect kDetailOpen = {60, 470, 420, 80};
const ui::Rect kDetailDelete = {60, 580, 420, 80};
const ui::Rect kConfirmYes = {60, 520, 200, 84};
const ui::Rect kConfirmNo = {280, 520, 200, 84};

struct Entry {
    String name;
    bool isDir = false;
    uint32_t size = 0;
};

String joinPath(const String &dir, const String &name)
{
    return dir.endsWith("/") ? dir + name : dir + "/" + name;
}

String formatSize(uint32_t bytes)
{
    char buf[20];
    if (bytes >= 1024 * 1024) {
        snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024));
    } else if (bytes >= 1024) {
        snprintf(buf, sizeof(buf), "%.0f KB", bytes / 1024.0);
    } else {
        snprintf(buf, sizeof(buf), "%u B", static_cast<unsigned>(bytes));
    }
    return buf;
}

void drawFolderIcon(int x, int cy)
{
    display.fillRoundRect(x, cy - 15, 18, 10, 3, TFT_BLACK);
    display.fillRoundRect(x, cy - 9, 36, 26, 3, TFT_BLACK);
}

void drawFileIcon(int x, int cy, bool image)
{
    display.drawRect(x + 4, cy - 17, 28, 34, TFT_BLACK);
    display.drawRect(x + 5, cy - 16, 26, 32, TFT_BLACK);
    if (image) {
        display.fillTriangle(x + 8, cy + 12, x + 17, cy - 2, x + 28, cy + 12, TFT_BLACK);
        display.fillCircle(x + 24, cy - 8, 3, TFT_BLACK);
    } else {
        for (int i = 0; i < 3; ++i) {
            display.drawFastHLine(x + 10, cy - 7 + i * 7, 16, TFT_BLACK);
        }
    }
}

class FilerApp : public ui::App {
public:
    const char *title() const override { return "ファイル"; }

    void enter() override
    {
        screen_ = Screen::List;
        // 画像ビューワーから戻ったときに同じ場所が出るよう、見ていたページを保つ。
        const int page = page_;
        load(path_.length() ? path_ : String("/"));
        page_ = std::max(0, std::min(page, pageCount() - 1));
    }

    void leave() override { stopTransfer(); }

    bool back() override
    {
        if (screen_ == Screen::Transfer) {
            stopTransfer();
            load(path_.length() ? path_ : String("/"));  // 転送で中身が変わっているので読み直す
            show(Screen::List);
            return true;
        }
        if (screen_ == Screen::Confirm) {
            show(Screen::Detail);
            return true;
        }
        if (screen_ == Screen::Detail) {
            show(Screen::List);
            return true;
        }
        if (path_ != "/" && path_.length()) {
            goUp();
            return true;
        }
        return false;
    }

    void draw() override
    {
        switch (screen_) {
            case Screen::List:
                drawList();
                break;
            case Screen::Detail:
                drawDetail();
                break;
            case Screen::Confirm:
                drawDetail();
                drawConfirm();
                break;
            case Screen::Transfer:
                drawTransfer();
                break;
        }
    }

    void tap(int x, int y) override
    {
        switch (screen_) {
            case Screen::List:
                tapList(x, y);
                break;
            case Screen::Detail:
                if (kDetailOpen.contains(x, y) && isImagePath(selected_.name)) {
                    openViewer(joinPath(path_, selected_.name));
                } else if (kDetailDelete.contains(x, y)) {
                    show(Screen::Confirm);
                }
                break;
            case Screen::Confirm:
                if (kConfirmYes.contains(x, y)) {
                    deleteSelected();
                } else if (kConfirmNo.contains(x, y)) {
                    show(Screen::Detail);
                }
                break;
            case Screen::Transfer:
                if (kTransferStop.contains(x, y)) {
                    back();
                }
                break;
        }
    }

    // SD カードが見つからないあいだは定期的に探し直し、見つかったら自動で一覧を出す。
    void tick() override
    {
        if (screen_ == Screen::Transfer) {
            // つながっている端末の数が変わったら表示を更新する。
            if (webfiles::clientCount() != shownClients_) {
                ui::requestRedraw();
            }
            return;
        }
        if (mounted_ || screen_ != Screen::List || millis() - lastRetryMs_ < 1500) {
            return;
        }
        lastRetryMs_ = millis();
        if (board::sdReady()) {
            load(path_.length() ? path_ : String("/"));
            ui::requestRedraw();
        }
    }

private:
    enum class Screen { List, Detail, Confirm, Transfer };

    void stopTransfer()
    {
        if (webfiles::running()) {
            ui::message("WiFi 転送を終了しています…");
            webfiles::stop();
        }
    }

    // WiFi 転送の画面。本体がアクセスポイントになり、PC のブラウザからファイルを出し入れできる。
    void drawTransfer()
    {
        const int cx = ui::kWidth / 2;
        if (!webfiles::running()) {
            ui::text("WiFi 転送を開始できませんでした", cx, 300, ui::fontMedium,
                     textdatum_t::middle_center);
            ui::text("SD カードが入っているか確認してください", cx, 350, ui::fontNormal,
                     textdatum_t::middle_center);
            ui::button(kTransferStop, "戻る", false, ui::fontMedium);
            return;
        }
        shownClients_ = webfiles::clientCount();

        // 左: WiFi につなぐ QR コード、右: ブラウザで開く QR コード。
        // スマホはカメラで順に読めばつながる。PC は下の文字を見て入力する。
        const int qrSize = 216;
        const int qrTop = 150;
        const int leftX = 36, rightX = ui::kWidth - 36 - qrSize;
        ui::text("① WiFi につなぐ", leftX + qrSize / 2, qrTop - 34, ui::fontNormal,
                 textdatum_t::middle_center);
        ui::text("② ブラウザで開く", rightX + qrSize / 2, qrTop - 34, ui::fontNormal,
                 textdatum_t::middle_center);
        display.qrcode(webfiles::wifiQrText().c_str(), leftX, qrTop, qrSize, 3);
        display.qrcode(webfiles::url().c_str(), rightX, qrTop, qrSize, 3);

        int y = qrTop + qrSize + 44;
        auto row = [&](const char *label, const String &value) {
            ui::text(label, 36, y, ui::fontNormal, textdatum_t::middle_left, ui::gray(60));
            ui::text(ui::fitText(value, ui::kWidth - 172 - 20, ui::fontLarge), 172, y,
                     ui::fontLarge, textdatum_t::middle_left);
            y += 62;
        };
        row("WiFi 名", webfiles::ssid());
        row("パスワード", webfiles::password());
        row("アドレス", webfiles::url());

        display.drawFastHLine(20, y - 14, ui::kWidth - 40, TFT_BLACK);
        ui::text("つながっている端末: " + String(shownClients_) + " 台", cx, y + 22,
                 ui::fontNormal, textdatum_t::middle_center);
        ui::text("この画面を閉じると転送は終了します。", cx, y + 62, ui::fontSmall,
                 textdatum_t::middle_center);

        ui::button(kTransferStop, "転送を終了する", true, ui::fontMedium);
    }

    void show(Screen screen)
    {
        screen_ = screen;
        ui::requestRedraw();
    }

    int pageCount() const
    {
        return std::max(1, static_cast<int>((entries_.size() + kRowsPerPage - 1) / kRowsPerPage));
    }

    void load(const String &path)
    {
        path_ = path;
        entries_.clear();
        page_ = 0;
        truncated_ = false;
        mounted_ = board::sdReady();
        if (!mounted_) {
            return;
        }
        File dir = SD.open(path);
        if (!dir || !dir.isDirectory()) {
            // フォルダが消えていた場合などは直下に戻る。直下も開けないならカードが読めていない。
            if (path != "/") {
                load("/");
            } else {
                mounted_ = false;
            }
            return;
        }
        for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
            if (entries_.size() >= kMaxEntries) {
                truncated_ = true;
                f.close();
                break;
            }
            Entry e;
            e.name = f.name();
            e.isDir = f.isDirectory();
            e.size = e.isDir ? 0 : f.size();
            f.close();
            if (!e.name.startsWith(".")) {
                entries_.push_back(e);
            }
        }
        dir.close();
        std::sort(entries_.begin(), entries_.end(), [](const Entry &a, const Entry &b) {
            if (a.isDir != b.isDir) {
                return a.isDir;
            }
            return a.name < b.name;
        });
    }

    void goUp()
    {
        const int slash = path_.lastIndexOf('/');
        load(slash <= 0 ? String("/") : path_.substring(0, slash));
        ui::requestRedraw();
    }

    void drawList()
    {
        // 現在のフォルダ
        display.fillRect(0, kPathTop, ui::kWidth, kPathHeight - 4, ui::gray(225));
        String shown = path_;
        display.setFont(ui::fontNormal);
        while (shown.length() > 1 && display.textWidth("…" + shown) > ui::kWidth - 170) {
            int cut = 1;
            while (cut < static_cast<int>(shown.length()) &&
                   (static_cast<uint8_t>(shown[cut]) & 0xC0) == 0x80) {
                ++cut;
            }
            shown.remove(0, cut);
        }
        ui::text(shown == path_ ? shown : "…" + shown, 16, kPathTop + (kPathHeight - 4) / 2,
                 ui::fontNormal, textdatum_t::middle_left);

        ui::button(kTransferButton, "WiFi 転送", false, ui::fontSmall);

        if (!mounted_) {
            ui::text("SD カードがありません", ui::kWidth / 2, 400, ui::fontMedium,
                     textdatum_t::middle_center);
            ui::text("カードを入れると自動で表示します", ui::kWidth / 2, 450, ui::fontNormal,
                     textdatum_t::middle_center, ui::gray(60));
            return;
        }
        if (entries_.empty()) {
            ui::text("このフォルダは空です", ui::kWidth / 2, 400, ui::fontMedium,
                     textdatum_t::middle_center);
        }

        const int start = page_ * kRowsPerPage;
        for (int i = 0; i < kRowsPerPage && start + i < static_cast<int>(entries_.size()); ++i) {
            const Entry &e = entries_[start + i];
            const int y = kListTop + i * kRowH;
            const int cy = y + kRowH / 2;
            ui::touchable({0, y, ui::kWidth, kRowH - 1});
            if (e.isDir) {
                drawFolderIcon(14, cy);
                ui::text(ui::fitText(e.name, ui::kWidth - 90, ui::fontNormal), 64, cy,
                         ui::fontNormal, textdatum_t::middle_left);
            } else {
                drawFileIcon(14, cy, isImagePath(e.name));
                ui::text(ui::fitText(e.name, 300, ui::fontNormal), 64, cy, ui::fontNormal,
                         textdatum_t::middle_left);
                ui::text(formatSize(e.size), kMenuLeft - 8, cy, ui::fontSmall,
                         textdatum_t::middle_right, ui::gray(60));
                // 詳細を開く「︙」
                for (int dot = -1; dot <= 1; ++dot) {
                    display.fillCircle(kMenuLeft + 32, cy + dot * 12, 3, TFT_BLACK);
                }
            }
            display.drawFastHLine(12, y + kRowH - 1, ui::kWidth - 24, ui::gray(170));
        }

        if (path_ != "/") {
            ui::button(kUpButton, "上の階層へ");
        }
        if (pageCount() > 1) {
            ui::button(kPrevButton, "前へ");
            ui::button(kNextButton, "次へ");
            char buf[40];
            snprintf(buf, sizeof(buf), "%d / %d%s", page_ + 1, pageCount(), truncated_ ? "+" : "");
            ui::text(buf, 206, ui::kBottomBarTop + 34, ui::fontSmall, textdatum_t::middle_center);
        }
    }

    void tapList(int x, int y)
    {
        if (kTransferButton.contains(x, y)) {
            ui::message("WiFi 転送を準備しています…");
            webfiles::start();
            show(Screen::Transfer);
            return;
        }
        if (path_ != "/" && kUpButton.contains(x, y)) {
            goUp();
            return;
        }
        if (pageCount() > 1 && kPrevButton.contains(x, y)) {
            page_ = (page_ + pageCount() - 1) % pageCount();
            ui::requestRedraw();
            return;
        }
        if (pageCount() > 1 && kNextButton.contains(x, y)) {
            page_ = (page_ + 1) % pageCount();
            ui::requestRedraw();
            return;
        }
        if (y < kListTop || y >= kListTop + kRowsPerPage * kRowH) {
            return;
        }
        const int index = page_ * kRowsPerPage + (y - kListTop) / kRowH;
        if (index >= static_cast<int>(entries_.size())) {
            return;
        }
        selected_ = entries_[index];
        if (selected_.isDir) {
            load(joinPath(path_, selected_.name));
            ui::requestRedraw();
        } else if (x < kMenuLeft && isImagePath(selected_.name)) {
            openViewer(joinPath(path_, selected_.name));
        } else {
            show(Screen::Detail);
        }
    }

    void drawDetail()
    {
        drawFileIcon(ui::kWidth / 2 - 18, 150, isImagePath(selected_.name));
        ui::text(ui::fitText(selected_.name, ui::kWidth - 40, ui::fontMedium), ui::kWidth / 2, 220,
                 ui::fontMedium, textdatum_t::middle_center);
        ui::text("サイズ: " + formatSize(selected_.size), ui::kWidth / 2, 280, ui::fontNormal,
                 textdatum_t::middle_center, ui::gray(60));
        ui::text(ui::fitText("場所: " + path_, ui::kWidth - 40, ui::fontNormal), ui::kWidth / 2,
                 326, ui::fontNormal, textdatum_t::middle_center, ui::gray(60));

        if (screen_ == Screen::Detail) {
            if (isImagePath(selected_.name)) {
                ui::button(kDetailOpen, "画像を開く", true, ui::fontMedium);
            }
            ui::button(kDetailDelete, "このファイルを削除する", false, ui::fontMedium);
        }
    }

    void drawConfirm()
    {
        ui::text("このファイルを削除しますか？", ui::kWidth / 2, 420, ui::fontMedium,
                 textdatum_t::middle_center);
        ui::text("削除すると元に戻せません", ui::kWidth / 2, 466, ui::fontNormal,
                 textdatum_t::middle_center, ui::gray(60));
        ui::button(kConfirmYes, "削除する", true, ui::fontMedium);
        ui::button(kConfirmNo, "やめる", false, ui::fontMedium);
    }

    void deleteSelected()
    {
        const String target = joinPath(path_, selected_.name);
        const bool ok = board::sdReady() && SD.remove(target);
        ui::message(ok ? "削除しました" : "削除できませんでした", selected_.name);
        delay(1500);
        const int page = page_;
        load(path_);
        page_ = std::min(page, pageCount() - 1);
        show(Screen::List);
    }

    Screen screen_ = Screen::List;
    String path_;
    std::vector<Entry> entries_;
    Entry selected_;
    int page_ = 0;
    bool mounted_ = false;
    bool truncated_ = false;
    uint32_t lastRetryMs_ = 0;
    int shownClients_ = -1;
};

FilerApp g_filer;

}  // namespace

ui::App &filer() { return g_filer; }

}  // namespace apps
