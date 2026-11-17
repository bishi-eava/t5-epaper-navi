// 画像ビューワー: SD カード上の JPEG / PNG / BMP を表示する。ロック画面への設定もここで行う。
#include <algorithm>
#include <vector>

#include "apps.h"
#include "settings.h"

namespace apps {
namespace {

using board::display;
using lgfx::textdatum_t;

constexpr int kImageTop = ui::kContentTop;
constexpr int kImageHeight = ui::kBottomBarTop - 8 - kImageTop;
constexpr size_t kMaxImages = 500;

const ui::Rect kPrevButton = {12, ui::kBottomBarTop, 110, 68};
const ui::Rect kLockButton = {134, ui::kBottomBarTop, 272, 68};
const ui::Rect kNextButton = {418, ui::kBottomBarTop, 110, 68};

String lowerExtension(const String &path)
{
    const int dot = path.lastIndexOf('.');
    if (dot < 0 || dot < path.lastIndexOf('/')) {
        return String();
    }
    String ext = path.substring(dot + 1);
    ext.toLowerCase();
    return ext;
}

String joinPath(const String &dir, const String &name)
{
    return dir.endsWith("/") ? dir + name : dir + "/" + name;
}

class ViewerApp : public ui::App {
public:
    const char *title() const override { return "画像"; }

    void enter() override
    {
        // ホームから開いたときは、画像のあるフォルダを探して開く。
        if (!openedWithPath_) {
            findImages();
            fromFiler_ = false;
        }
        openedWithPath_ = false;
        ui::requestCleanRefresh();  // 写真は階調が出る高品質モードで描く
    }

    void openPath(const String &path)
    {
        const int slash = path.lastIndexOf('/');
        const String dir = slash <= 0 ? String("/") : path.substring(0, slash);
        loadDirectory(dir, path.substring(slash + 1));
        openedWithPath_ = true;
        fromFiler_ = true;
    }

    // ファイラーから開いたときは、「戻る」でファイル一覧に戻る。
    bool back() override
    {
        if (!fromFiler_) {
            return false;
        }
        fromFiler_ = false;
        ui::open(filer());
        return true;
    }

    void draw() override
    {
        waitingForCard_ = !board::sdReady();
        if (waitingForCard_) {
            centerText("SD カードがありません", "カードを入れると自動で表示します");
            return;
        }
        if (images_.empty()) {
            centerText("画像が見つかりません", "SD カードに JPEG / PNG / BMP を入れてください");
            return;
        }

        const String path = joinPath(dir_, images_[index_]);
        if (!drawImageFit(path, 0, kImageTop, ui::kWidth, kImageHeight)) {
            centerText("この画像は表示できません", images_[index_]);
        }

        if (images_.size() > 1) {
            ui::button(kPrevButton, "前へ");
            ui::button(kNextButton, "次へ");
        }
        const bool isLock = settings::lockImage() == path;
        ui::button(kLockButton, isLock ? "ロック画面に設定済み" : "ロック画面に設定", isLock);
    }

    void tap(int x, int y) override
    {
        if (images_.empty()) {
            return;
        }
        if (kLockButton.contains(x, y)) {
            const String path = joinPath(dir_, images_[index_]);
            settings::setLockImage(settings::lockImage() == path ? String() : path);
            ui::requestRedraw();
        } else if (kPrevButton.contains(x, y) || (y < ui::kBottomBarTop && x < ui::kWidth / 3)) {
            step(-1);
        } else if (kNextButton.contains(x, y) || (y < ui::kBottomBarTop && x > ui::kWidth * 2 / 3)) {
            step(1);
        }
    }

    void drag(int x, int y, int dx, int dy) override
    {
        if (abs(dx) >= 80 && abs(dx) > abs(dy)) {
            step(dx > 0 ? -1 : 1);
        }
    }

    // SD カードが見つからないあいだは定期的に探し直す。
    void tick() override
    {
        if (!waitingForCard_ || millis() - lastRetryMs_ < 1500) {
            return;
        }
        lastRetryMs_ = millis();
        if (board::sdReady()) {
            loadDirectory(dir_.length() ? dir_ : String("/"), String());
            ui::requestCleanRefresh();
        }
    }

private:
    // ホームから開いたときに表示するフォルダを決める。次の順に探し、画像が見つかった所を開く。
    //   1. 前回見ていたフォルダ（電源を切っても覚えている）
    //   2. ロック画面に設定した画像のあるフォルダ
    //   3. SD カードの直下
    //   4. SD カードの直下にある各フォルダ（1 階層だけ）
    void findImages()
    {
        const String previous = current();
        std::vector<String> candidates;
        if (dir_.length()) {
            candidates.push_back(dir_);
        }
        if (settings::imageDir().length()) {
            candidates.push_back(settings::imageDir());
        }
        const String lock = settings::lockImage();
        if (lock.length()) {
            const int slash = lock.lastIndexOf('/');
            candidates.push_back(slash <= 0 ? String("/") : lock.substring(0, slash));
        }
        candidates.push_back("/");
        if (board::sdReady()) {
            File root = SD.open("/");
            for (File f = root.openNextFile(); f && candidates.size() < 40; f = root.openNextFile()) {
                const String name = f.name();
                if (f.isDirectory() && !name.startsWith(".")) {
                    candidates.push_back("/" + name);
                }
                f.close();
            }
            root.close();
        }
        for (const String &dir : candidates) {
            loadDirectory(dir, previous);
            if (!images_.empty()) {
                return;
            }
        }
        loadDirectory("/", String());  // どこにも画像がない
    }

    String current() const
    {
        return images_.empty() ? String() : images_[index_];
    }

    void step(int delta)
    {
        if (images_.size() < 2) {
            return;
        }
        const int count = static_cast<int>(images_.size());
        index_ = (index_ + delta + count) % count;
        ui::requestCleanRefresh();  // 写真は階調が出る高品質モードで描く
    }

    void loadDirectory(const String &dir, const String &selectName)
    {
        dir_ = dir;
        images_.clear();
        index_ = 0;
        if (!board::sdReady()) {
            return;
        }
        File d = SD.open(dir);
        if (!d || !d.isDirectory()) {
            return;
        }
        for (File f = d.openNextFile(); f && images_.size() < kMaxImages; f = d.openNextFile()) {
            const String name = f.name();
            if (!f.isDirectory() && !name.startsWith(".") && isImagePath(name)) {
                images_.push_back(name);
            }
            f.close();
        }
        d.close();
        std::sort(images_.begin(), images_.end());
        if (!images_.empty()) {
            settings::setImageDir(dir_);  // 次回のために覚えておく
        }
        for (size_t i = 0; i < images_.size(); ++i) {
            if (images_[i] == selectName) {
                index_ = static_cast<int>(i);
                break;
            }
        }
    }

    static void centerText(const String &line1, const String &line2 = String())
    {
        const int cy = kImageTop + kImageHeight / 2;
        ui::text(line1, ui::kWidth / 2, cy - 20, ui::fontMedium, textdatum_t::middle_center);
        ui::text(ui::fitText(line2, ui::kWidth - 40, ui::fontNormal), ui::kWidth / 2, cy + 30,
                 ui::fontNormal, textdatum_t::middle_center, ui::gray(60));
    }

    String dir_;
    std::vector<String> images_;
    int index_ = 0;
    bool openedWithPath_ = false;
    bool fromFiler_ = false;  // ファイラーから開かれた（「戻る」の行き先がファイラー）
    bool waitingForCard_ = false;
    uint32_t lastRetryMs_ = 0;
};

ViewerApp g_viewer;

}  // namespace

ui::App &viewer() { return g_viewer; }

void openViewer(const String &path)
{
    g_viewer.openPath(path);
    ui::open(g_viewer);
}

bool isImagePath(const String &path)
{
    const String ext = lowerExtension(path);
    return ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "bmp";
}

namespace {

// 画像を target に、枠 (w x h) に収まるよう縦横比を保って拡大縮小し、中央に寄せて描く。
// 倍率 0 が「枠に合わせる」、middle_center が「中央に寄せる」の指定。
template <typename Target>
bool decodeFit(Target &target, const String &path, int x, int y, int w, int h)
{
    const String ext = lowerExtension(path);
    if (ext == "jpg" || ext == "jpeg") {
        return target.drawJpgFile(SD, path.c_str(), x, y, w, h, 0, 0, 0.0f, 0.0f,
                                  lgfx::datum_t::middle_center);
    }
    if (ext == "png") {
        return target.drawPngFile(SD, path.c_str(), x, y, w, h, 0, 0, 0.0f, 0.0f,
                                  lgfx::datum_t::middle_center);
    }
    if (ext == "bmp") {
        return target.drawBmpFile(SD, path.c_str(), x, y, w, h, 0, 0, 0.0f, 0.0f,
                                  lgfx::datum_t::middle_center);
    }
    return false;
}

// 電子ペーパー向けの補正。pixels は 8 ビットのグレースケール (w x h)。
//   1. 明るさの分布を調べ、暗い側 1% と明るい側 1% を切り捨てて全体を白〜黒いっぱいに広げる
//   2. 中間調のコントラストを少し強める
//   3. 誤差拡散で 16 階調に落とす（規則的な網点より、なだらかな階調がきれいに出る）
// 余白（画像の外側の白）は分布の計算から除く。
void enhanceForEpaper(uint8_t *pixels, int w, int h)
{
    // 余白を除いた、画像が描かれている範囲を求める。
    int left = w, right = -1, top = h, bottom = -1;
    for (int y = 0; y < h; ++y) {
        const uint8_t *row = pixels + y * w;
        for (int x = 0; x < w; ++x) {
            if (row[x] != 255) {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
    }
    if (right < left) {
        return;  // 真っ白
    }

    uint32_t histogram[256] = {0};
    for (int y = top; y <= bottom; ++y) {
        const uint8_t *row = pixels + y * w;
        for (int x = left; x <= right; ++x) {
            ++histogram[row[x]];
        }
    }
    const uint32_t total = static_cast<uint32_t>(right - left + 1) * (bottom - top + 1);
    const uint32_t cut = total / 100;
    int low = 0, high = 255;
    for (uint32_t sum = 0; low < 255 && (sum += histogram[low]) <= cut; ++low) {
    }
    for (uint32_t sum = 0; high > 0 && (sum += histogram[high]) <= cut; --high) {
    }
    if (high - low < 24) {  // ほぼ単色の画像は広げない
        low = 0;
        high = 255;
    }

    uint8_t curve[256];
    for (int i = 0; i < 256; ++i) {
        float v = static_cast<float>(i - low) / static_cast<float>(high - low);
        v = std::min(1.0f, std::max(0.0f, v));
        const float sCurve = v * v * (3.0f - 2.0f * v);
        v = v + (sCurve - v) * 0.5f;  // S 字カーブを半分の強さで掛ける
        curve[i] = static_cast<uint8_t>(v * 255.0f + 0.5f);
    }

    // 誤差拡散（フロイド・スタインバーグ法）。
    // 出力値は「階調 x 16 + 8」にする。画面側の階調化でちょうどその階調になる値。
    std::vector<int16_t> errorRow(w + 2, 0), nextRow(w + 2, 0);
    for (int y = top; y <= bottom; ++y) {
        uint8_t *row = pixels + y * w;
        std::fill(nextRow.begin(), nextRow.end(), 0);
        int carry = 0;
        for (int x = left; x <= right; ++x) {
            const int value = curve[row[x]] + carry + errorRow[x + 1];
            const int level = std::min(15, std::max(0, (value * 15 + 127) / 255));
            const int error = value - level * 17;
            row[x] = static_cast<uint8_t>(level * 16 + 8);
            carry = error * 7 / 16;
            nextRow[x] += error * 3 / 16;
            nextRow[x + 1] += error * 5 / 16;
            nextRow[x + 2] += error / 16;
        }
        errorRow.swap(nextRow);
    }
}

}  // namespace

bool drawImageFit(const String &path, int x, int y, int w, int h)
{
    if (!board::sdReady() || !SD.exists(path)) {
        return false;
    }

    // いったんメモリ上にグレースケールで展開し、補正してから画面に描く。
    LGFX_Sprite canvas;
    canvas.setPsram(true);
    canvas.setColorDepth(lgfx::color_depth_t::grayscale_8bit);
    if (canvas.createSprite(w, h) == nullptr) {
        // メモリが足りないときは、補正なしで直接描く。
        return decodeFit(display, path, x, y, w, h);
    }
    canvas.fillScreen(TFT_WHITE);
    if (!decodeFit(canvas, path, 0, 0, w, h)) {
        return false;
    }
    auto *pixels = static_cast<uint8_t *>(canvas.getBuffer());
    enhanceForEpaper(pixels, w, h);
    display.pushImage(x, y, w, h, reinterpret_cast<const lgfx::grayscale_t *>(pixels));
    return true;
}

}  // namespace apps
