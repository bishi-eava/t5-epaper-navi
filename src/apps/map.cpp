// マップ: 国土地理院タイルの表示と GPS の現在位置（m5-navi 相当、SPEC.md 4.3）。
//
// 電子ペーパーは書き換えが遅いので、指に追従するスクロールはせず、
// 指を離した時点で 1 回だけ描き直す。タイルは SD カードに保存し、
// m5-navi と同じ場所（/cyberjapandata.gsi.go.jp/xyz/std/{z}/{x}/{y}.png）を使う。
#include <HTTPClient.h>
#include <WiFi.h>

#include <cmath>

#include "apps.h"
#include "gps.h"
#include "map_cert.h"
#include "net.h"
#include "safewrite.h"
#include "settings.h"

namespace apps {
namespace {

using board::display;
using lgfx::textdatum_t;

constexpr int kTileSize = 256;
// 表示するズームレベル。7〜16 は m5-navi と同じ並びにして、SD カードのキャッシュをそのまま使う。
// 18 はタイルを取得せず、レベル 16 のタイルを 4 倍に拡大して表示する。
constexpr int kZoomLevels[] = {7, 8, 12, 14, 16, 18};
constexpr int kMaxTileZoom = 16;  // これより上の表示レベルは、このレベルのタイルを拡大して使う
constexpr int kZoomLevelCount = sizeof(kZoomLevels) / sizeof(kZoomLevels[0]);

// 指定したズームレベルに最も近い、許可されたレベルの番号を返す。
int nearestZoomIndex(int zoom)
{
    int best = 0;
    for (int i = 1; i < kZoomLevelCount; ++i) {
        if (abs(kZoomLevels[i] - zoom) < abs(kZoomLevels[best] - zoom)) {
            best = i;
        }
    }
    return best;
}

constexpr int kMapTop = ui::kContentTop;
constexpr int kMapHeight = ui::kBottomBarTop - 8 - kMapTop;

constexpr uint32_t kGpsRedrawIntervalMs = 10000;  // GPS による描き直しの最短間隔
constexpr int kGpsRedrawPixels = 30;              // これ以上動いたら描き直す
constexpr float kGoodHdop = 3.0f;                 // これ未満を高精度とみなす

constexpr char kTileHost[] = "cyberjapandata.gsi.go.jp";
constexpr char kTilePathFormat[] = "/cyberjapandata.gsi.go.jp/xyz/std/%d/%d/%d.png";

const ui::Rect kZoomOutButton = {12, ui::kBottomBarTop, 96, 68};
const ui::Rect kZoomInButton = {176, ui::kBottomBarTop, 96, 68};
const ui::Rect kFollowButton = {292, ui::kBottomBarTop, 236, 68};

// 緯度経度を、ズーム z での世界全体のピクセル座標に変換する。
void latLonToWorld(double lat, double lon, int zoom, double &x, double &y)
{
    const double size = static_cast<double>(kTileSize) * (1 << zoom);
    x = (lon / 180.0 + 1.0) * size / 2.0;
    y = (-log(tan((45.0 + lat / 2.0) * M_PI / 180.0)) + M_PI) * size / (2.0 * M_PI);
}

void worldToLatLon(double x, double y, int zoom, double &lat, double &lon)
{
    const double size = static_cast<double>(kTileSize) * (1 << zoom);
    lon = (x / size) * 360.0 - 180.0;
    const double mapY = (y / size) * 2.0 * M_PI - M_PI;
    lat = 2.0 * atan(exp(-mapY)) * 180.0 / M_PI - 90.0;
}

// ---- 階調変換 ---------------------------------------------------------------
// 地理院タイルの色を、電子ペーパーの 16 階調で道路が見やすくなるように変換する。
// 単純に明るさだけで変換すると、県道の黄色 (255,255,0) がほぼ白になって見えなくなるため、
// 黄色と赤系（道路の色）は濃くする。橙色（建物の色）まで濃くすると地図が黒く潰れるので除く。
// 変換の係数は、実際のタイルで変換結果を見比べて決めた。

uint8_t g_curve[256];  // 仕上げのトーンカーブ: 薄い地色を白に飛ばし、中間の灰色を濃くする

void initToneCurve()
{
    for (int i = 0; i < 256; ++i) {
        const float v = std::min(1.0f, i / 238.0f);
        g_curve[i] = static_cast<uint8_t>(255.0f * powf(v, 1.5f) + 0.5f);
    }
}

inline uint8_t toneMap(int r, int g, int b)
{
    int lum = (r * 77 + g * 150 + b * 29) >> 8;
    const int warmth = r - b;
    if (warmth > 60 && (r - g < 60 || g - b < 40)) {
        const int strength = std::min(80, warmth - 60);  // 0〜80
        lum -= lum * 58 * strength / (100 * 80);         // 最大で 58% 暗くする
    }
    return g_curve[lum];
}

// 保存先のフォルダを、上の階層から順に作る。
void makeParentDirs(const char *filePath)
{
    char path[96];
    strlcpy(path, filePath, sizeof(path));
    for (char *p = strchr(path + 1, '/'); p != nullptr; p = strchr(p + 1, '/')) {
        *p = '\0';
        if (!SD.exists(path)) {
            SD.mkdir(path);
        }
        *p = '/';
    }
}

// タイルを 1 枚ダウンロードして SD カードに保存する。
// 途中で失敗した不完全なファイルがキャッシュとして残らないよう、一時ファイルに書いてから改名する。
bool downloadTile(HTTPClient &http, int zoom, int tx, int ty, const char *savePath)
{
    char url[128];
    snprintf(url, sizeof(url), "https://%s/xyz/std/%d/%d/%d.png", kTileHost, zoom, tx, ty);

    http.setReuse(true);
    http.setConnectTimeout(6000);
    http.setTimeout(6000);
    if (!http.begin(url, kGsiRootCa)) {
        return false;
    }
    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[map] GET %s -> %d\n", url, code);
        http.end();
        // 404 は「その場所のタイルが無い」だけなので、通信失敗とは区別する。
        return code == HTTP_CODE_NOT_FOUND;
    }

    makeParentDirs(savePath);
    const String tempPath = String(savePath) + ".tmp";
    SafeWriter writer;
    if (!writer.begin(tempPath)) {
        http.end();
        return false;
    }

    static uint8_t buffer[4096];
    WiFiClient *stream = http.getStreamPtr();
    const int total = http.getSize();
    int received = 0;
    uint32_t lastDataMs = millis();
    while (http.connected() && (total < 0 || received < total) && millis() - lastDataMs < 6000) {
        const size_t available = stream->available();
        if (available == 0) {
            delay(1);
            continue;
        }
        const int n = stream->readBytes(buffer, std::min(available, sizeof(buffer)));
        if (n <= 0 || !writer.write(buffer, n)) {
            break;
        }
        received += n;
        lastDataMs = millis();
    }
    http.end();

    // 受信が最後まで終わり、カードへの書き出しも確認できたときだけキャッシュにする。
    const bool complete = received > 0 && (total < 0 || received == total);
    if (writer.finish() && complete && SD.rename(tempPath, savePath)) {
        return true;
    }
    writer.abort();
    SD.remove(tempPath);  // 自分で作った一時ファイルだけを消す
    return false;
}

class MapApp : public ui::App {
public:
    const char *title() const override { return "マップ"; }

    void enter() override
    {
        const settings::MapView view = settings::mapView();
        lat_ = view.lat;
        lon_ = view.lon;
        zoom_ = kZoomLevels[nearestZoomIndex(view.zoom)];
        follow_ = true;
        shownFix_ = false;

        // タイル 1 枚ぶんの作業領域（デコード用と、階調変換後の画素）を PSRAM に確保する。
        initToneCurve();
        tile_.setPsram(true);
        tile_.setColorDepth(24);
        tile_.createSprite(kTileSize, kTileSize);
        if (gray_ == nullptr) {
            gray_ = static_cast<uint8_t *>(ps_malloc(kTileSize * kTileSize));
        }
        gps::start();
        viewChanged();
    }

    void leave() override
    {
        gps::stop();
        tile_.deleteSprite();
        free(gray_);
        gray_ = nullptr;
        settings::MapView view;
        view.lat = lat_;
        view.lon = lon_;
        view.zoom = static_cast<uint8_t>(zoom_);
        settings::setMapView(view);
    }

    void draw() override
    {
        lastDrawMs_ = millis();
        shownFix_ = gps::hasFix();
        if (shownFix_) {
            shownLat_ = gps::latitude();
            shownLon_ = gps::longitude();
        }

        drawTiles();
        drawMarker();
        drawOverlay();

        ui::button(kZoomOutButton, "－", false, ui::fontLarge);
        ui::button(kZoomInButton, "＋", false, ui::fontLarge);
        ui::text("Z" + String(zoom_), (kZoomOutButton.x + kZoomOutButton.w + kZoomInButton.x) / 2,
                 ui::kBottomBarTop + 34, ui::fontNormal, textdatum_t::middle_center);
        // 追従中は黒地に白文字、追従していないときは白地に黒文字で区別する。
        ui::button(kFollowButton, "現在地", follow_);
    }

    void tap(int x, int y) override
    {
        if (kZoomOutButton.contains(x, y)) {
            stepZoom(-1);
        } else if (kZoomInButton.contains(x, y)) {
            stepZoom(1);
        } else if (kFollowButton.contains(x, y)) {
            follow_ = !follow_;
            if (follow_ && gps::hasFix()) {
                lat_ = gps::latitude();
                lon_ = gps::longitude();
            }
            viewChanged();
        }
    }

    // ドラッグ: 指を離した時点で、動かした分だけ地図をずらして 1 回描き直す。
    void drag(int x, int y, int dx, int dy) override
    {
        if (y >= kMapTop + kMapHeight) {
            return;
        }
        double wx, wy;
        latLonToWorld(lat_, lon_, zoom_, wx, wy);
        const double size = static_cast<double>(kTileSize) * (1 << zoom_);
        wx = fmod(wx - dx + size, size);
        wy = std::min(std::max(wy - dy, 0.0), size - 1.0);
        worldToLatLon(wx, wy, zoom_, lat_, lon_);
        follow_ = false;
        viewChanged();
    }

    void tick() override
    {
        gps::tick();

        const bool fix = gps::hasFix();
        if (millis() - lastDrawMs_ < kGpsRedrawIntervalMs) {
            return;
        }
        if (fix != shownFix_) {
            // 測位できた / 見失った
            if (fix && follow_) {
                lat_ = gps::latitude();
                lon_ = gps::longitude();
            }
            viewChanged();
            return;
        }
        if (!fix) {
            return;
        }
        // 表示中のマーカーから一定以上動いたら描き直す。
        double oldX, oldY, newX, newY;
        latLonToWorld(shownLat_, shownLon_, zoom_, oldX, oldY);
        latLonToWorld(gps::latitude(), gps::longitude(), zoom_, newX, newY);
        if (fabs(newX - oldX) >= kGpsRedrawPixels || fabs(newY - oldY) >= kGpsRedrawPixels) {
            if (follow_) {
                lat_ = gps::latitude();
                lon_ = gps::longitude();
            }
            viewChanged();
        }
    }

private:
    // 実際に読み込むタイルのズームレベル、その拡大率、画面上での 1 枚の大きさ。
    int tileZoom() const { return std::min(zoom_, kMaxTileZoom); }
    int tileScale() const { return 1 << (zoom_ - tileZoom()); }
    int tileSpan() const { return kTileSize * tileScale(); }

    // 許可されたズームレベルの中で、1 段階だけ拡大・縮小する。
    void stepZoom(int delta)
    {
        const int index = constrain(nearestZoomIndex(zoom_) + delta, 0, kZoomLevelCount - 1);
        if (kZoomLevels[index] != zoom_) {
            zoom_ = kZoomLevels[index];
            viewChanged();
        }
    }

    // 表示範囲が変わったとき: 足りないタイルを取得してから、階調の出る高品質モードで描き直す。
    void viewChanged()
    {
        fetchMissingTiles();
        ui::requestCleanRefresh();
    }

    // 画面に入るタイルを順に処理する。fn(タイル x, タイル y, 画面 x, 画面 y)。
    // タイル y が範囲外（地図の上下端の外）のときは ty に -1 を渡す。
    template <typename F>
    void forEachVisibleTile(F fn) const
    {
        double centerX, centerY;
        latLonToWorld(lat_, lon_, zoom_, centerX, centerY);
        const int64_t left = llround(centerX) - ui::kWidth / 2;
        const int64_t top = llround(centerY) - kMapHeight / 2;
        const int count = 1 << tileZoom();
        const int span = tileSpan();

        const int64_t firstX = floorDiv(left, span);
        const int64_t lastX = floorDiv(left + ui::kWidth - 1, span);
        const int64_t firstY = floorDiv(top, span);
        const int64_t lastY = floorDiv(top + kMapHeight - 1, span);
        for (int64_t ty = firstY; ty <= lastY; ++ty) {
            for (int64_t tx = firstX; tx <= lastX; ++tx) {
                const int screenX = static_cast<int>(tx * span - left);
                const int screenY = kMapTop + static_cast<int>(ty * span - top);
                const int wrappedX = static_cast<int>(((tx % count) + count) % count);
                const int tileY = (ty < 0 || ty >= count) ? -1 : static_cast<int>(ty);
                fn(wrappedX, tileY, screenX, screenY);
            }
        }
    }

    static int64_t floorDiv(int64_t a, int64_t b)
    {
        return a >= 0 ? a / b : -((-a + b - 1) / b);
    }

    void fetchMissingTiles()
    {
        if (!board::sdReady() || !net::connected()) {
            return;
        }
        int missing = 0;
        char path[96];
        forEachVisibleTile([&](int tx, int ty, int, int) {
            if (ty < 0) {
                return;
            }
            snprintf(path, sizeof(path), kTilePathFormat, tileZoom(), tx, ty);
            if (!SD.exists(path)) {
                ++missing;
                Serial.printf("[map] missing %s\n", path);
            }
        });
        if (missing == 0) {
            return;
        }

        ui::message("地図をダウンロードしています…", String(missing) + " 枚");
        HTTPClient http;
        bool failed = false;
        forEachVisibleTile([&](int tx, int ty, int, int) {
            if (ty < 0 || failed) {
                return;
            }
            snprintf(path, sizeof(path), kTilePathFormat, tileZoom(), tx, ty);
            if (!SD.exists(path) && !downloadTile(http, tileZoom(), tx, ty, path)) {
                failed = true;  // 通信できないときは、残りの枚数ぶん待たされないよう打ち切る
            }
            delay(1);  // 他のタスクに処理を譲る
        });
        http.end();
    }

    void drawTiles()
    {
        display.setClipRect(0, kMapTop, ui::kWidth, kMapHeight);
        const bool sd = board::sdReady();
        char path[96];
        forEachVisibleTile([&](int tx, int ty, int sx, int sy) {
            bool drawn = false;
            if (sd && ty >= 0) {
                snprintf(path, sizeof(path), kTilePathFormat, tileZoom(), tx, ty);
                drawn = SD.exists(path) && drawTile(path, sx, sy);
            }
            if (!drawn) {
                // 未取得のタイル: 薄い灰色の枠だけ描く
                display.fillRect(sx, sy, tileSpan(), tileSpan(), ui::gray(235));
                display.drawRect(sx, sy, tileSpan(), tileSpan(), ui::gray(190));
            }
            delay(1);  // 他のタスクに処理を譲る
        });
        display.clearClipRect();
    }

    // タイル 1 枚を読み込み、階調変換して画面の (sx, sy) に描く。拡大表示にも対応する。
    bool drawTile(const char *path, int sx, int sy)
    {
        auto *pixels = static_cast<lgfx::bgr888_t *>(tile_.getBuffer());
        if (pixels == nullptr || gray_ == nullptr) {
            return false;
        }
        tile_.fillScreen(TFT_WHITE);
        if (!tile_.drawPngFile(SD, path, 0, 0)) {
            return false;
        }
        for (int i = 0; i < kTileSize * kTileSize; ++i) {
            gray_[i] = toneMap(pixels[i].r, pixels[i].g, pixels[i].b);
        }

        const int scale = tileScale();
        if (scale == 1) {
            display.pushImage(sx, sy, kTileSize, kTileSize,
                              reinterpret_cast<const lgfx::grayscale_t *>(gray_));
            return true;
        }

        // 拡大表示: 画面に入る範囲だけを、1 行ずつ引き伸ばして描く。
        static uint8_t row[ui::kWidth];
        const int span = tileSpan();
        const int x0 = std::max(sx, 0);
        const int x1 = std::min(sx + span, ui::kWidth);
        const int y0 = std::max(sy, kMapTop);
        const int y1 = std::min(sy + span, kMapTop + kMapHeight);
        int lastSourceY = -1;
        for (int y = y0; y < y1; ++y) {
            const int sourceY = (y - sy) / scale;
            if (sourceY != lastSourceY) {
                lastSourceY = sourceY;
                const uint8_t *source = gray_ + sourceY * kTileSize;
                for (int x = x0; x < x1; ++x) {
                    row[x - x0] = source[(x - sx) / scale];
                }
            }
            display.pushImage(x0, y, x1 - x0, 1, reinterpret_cast<const lgfx::grayscale_t *>(row));
        }
        return true;
    }

    // 現在位置のマーカー。高精度は塗りつぶし、低精度は白抜きで区別する。
    void drawMarker()
    {
        if (!shownFix_) {
            return;
        }
        double centerX, centerY, markerX, markerY;
        latLonToWorld(lat_, lon_, zoom_, centerX, centerY);
        latLonToWorld(shownLat_, shownLon_, zoom_, markerX, markerY);
        const int x = ui::kWidth / 2 + static_cast<int>(llround(markerX) - llround(centerX));
        const int y = kMapTop + kMapHeight / 2 + static_cast<int>(llround(markerY) - llround(centerY));
        if (x < 0 || x >= ui::kWidth || y < kMapTop || y >= kMapTop + kMapHeight) {
            return;
        }
        display.setClipRect(0, kMapTop, ui::kWidth, kMapHeight);
        display.fillCircle(x, y, 17, TFT_WHITE);
        display.fillCircle(x, y, 14, TFT_BLACK);
        if (gps::hdop() >= kGoodHdop) {
            display.fillCircle(x, y, 8, TFT_WHITE);
        }
        display.clearClipRect();
    }

    void drawOverlay()
    {
        // 左上: GPS の状態
        String status;
        if (!gps::detected()) {
            status = "GPS 起動中";
        } else if (gps::hasFix()) {
            status = "GPS 測位済み  衛星 " + String(gps::satellites());
        } else {
            status = "GPS 衛星を探しています";
        }
        display.setFont(ui::fontSmall);
        const int statusWidth = display.textWidth(status) + 20;
        display.fillRect(0, kMapTop, statusWidth, 34, TFT_WHITE);
        display.drawRect(-1, kMapTop - 1, statusWidth + 1, 36, TFT_BLACK);
        ui::text(status, 10, kMapTop + 17, ui::fontSmall, textdatum_t::middle_left);

        if (!board::sdReady()) {
            ui::text("SD カードがありません（地図を保存できません）", ui::kWidth / 2,
                     kMapTop + kMapHeight / 2, ui::fontNormal, textdatum_t::middle_center);
        }

        // 右下: 出典
        const char *credit = "地理院タイル";
        display.setFont(ui::fontSmall);
        const int creditWidth = display.textWidth(credit) + 16;
        const int creditTop = kMapTop + kMapHeight - 30;
        display.fillRect(ui::kWidth - creditWidth, creditTop, creditWidth, 30, TFT_WHITE);
        ui::text(credit, ui::kWidth - 8, creditTop + 15, ui::fontSmall, textdatum_t::middle_right);

        display.drawFastHLine(0, kMapTop + kMapHeight, ui::kWidth, TFT_BLACK);
    }

    double lat_ = 35.689440;
    double lon_ = 139.691670;
    int zoom_ = kZoomLevels[2];
    bool follow_ = true;

    LGFX_Sprite tile_;         // タイルのデコード先
    uint8_t *gray_ = nullptr;  // 階調変換後の 1 枚ぶんの画素

    // 最後に描いたときの GPS の状態（描き直すかどうかの判断に使う）
    bool shownFix_ = false;
    double shownLat_ = 0;
    double shownLon_ = 0;
    uint32_t lastDrawMs_ = 0;
};

MapApp g_map;

}  // namespace

ui::App &map() { return g_map; }

}  // namespace apps
