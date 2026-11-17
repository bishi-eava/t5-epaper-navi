// マップ: 国土地理院タイルの表示と GPS の現在位置（m5-navi 相当、SPEC.md 4.3）。
//
// 電子ペーパーは書き換えが遅いので、指に追従するスクロールはせず、
// 指を離した時点で 1 回だけ描き直す。タイルは SD カードに保存し、
// m5-navi と同じ場所（/cyberjapandata.gsi.go.jp/xyz/std/{z}/{x}/{y}.png）を使う。
//
// ルート案内（SPEC.md 4.9）もここで扱う。経路の取得と保存は route.cpp が受け持ち、
// このファイルは目的地の決め方、経路の描画、案内の表示、スマホからの目的地の受信を受け持つ。
#include <HTTPClient.h>
#include <WebServer.h>
#include <WiFi.h>

#include <cmath>

#include "apps.h"
#include "gps.h"
#include "map_cert.h"
#include "net.h"
#include "route.h"
#include "safewrite.h"
#include "settings.h"
#include "webfiles.h"

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

const ui::Rect kZoomOutButton = {8, ui::kBottomBarTop, 84, 68};
const ui::Rect kZoomInButton = {150, ui::kBottomBarTop, 84, 68};
const ui::Rect kFollowButton = {244, ui::kBottomBarTop, 140, 68};
const ui::Rect kRouteButton = {394, ui::kBottomBarTop, 138, 68};

// ---- ルート案内 -------------------------------------------------------------

// 地図の上部に出す案内の帯。走行中に一目で読めるよう、矢印と距離を大きく出す。
//   1 段目: 曲がる向きの矢印と、そこまでの距離
//   2 段目: 曲がる向きの言葉と、進む先の道路名
//   3 段目: 目的地までの残り距離と、到着の見込み時刻
constexpr int kBandRow1 = 124;
constexpr int kBandRow2 = 52;
constexpr int kBandRow3 = 40;
constexpr int kBandHeight = kBandRow1 + kBandRow2 + kBandRow3;
constexpr uint32_t kBandIntervalMs = 2000;       // 帯だけを書き換える最短間隔
constexpr int kPickBannerHeight = 44;            // 目的地を選んでいるときの帯
constexpr float kOffRouteM = 50.0f;              // これ以上離れたら「ルートから外れた」とみなす
constexpr float kArriveM = 30.0f;                // 目的地にこれ以内まで近づいたら到着
constexpr uint32_t kProgressIntervalMs = 2000;   // 現在地を経路に当てはめる間隔
const ui::Rect kBandRefetch = {352, kMapTop + 16, 180, 92};

// ルートのメニュー
const ui::Rect kMenuPick = {60, 330, 420, 76};
const ui::Rect kMenuWhole = {60, 424, 420, 76};
const ui::Rect kMenuRefetch = {60, 518, 420, 76};
const ui::Rect kMenuStop = {60, 612, 420, 76};
const ui::Rect kMenuClose = {60, 852, 420, 76};

// スマホなどから渡された目的地の候補。マップを開いているときに、画面で確認してから使う。
struct Offer {
    bool pending = false;
    double lat = 0;
    double lon = 0;
    String name;
};
Offer g_offer;

// 目的地の受け口（SPEC.md 4.9）。マップを開いていて、WiFi でルーターやテザリングに
// つながっている間だけ動かす。認証はないので、受け取っただけでは何も変えない。
WebServer g_receiver(80);

const char kReceiverPage[] PROGMEM = R"HTML(<!doctype html>
<html lang="ja"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>t5-epaper-navi 目的地</title></head>
<body style="font-family:system-ui,sans-serif;max-width:480px;margin:0 auto;padding:16px">
<h1 style="font-size:19px">目的地を送る</h1>
<form method="post" action="/api/destination">
<p>緯度, 経度<br><input name="ll" required placeholder="35.658034, 139.701636" style="width:100%;font-size:17px;padding:8px"></p>
<p style="color:#555;font-size:14px">Google マップで地点を長押しして出る座標を、そのまま貼り付けられます。</p>
<p>名前（省略可）<br><input name="name" style="width:100%;font-size:17px;padding:8px"></p>
<p><button style="font-size:17px;padding:10px 24px">送る</button></p>
</form>
<p style="color:#555;font-size:14px">本体でマップを開いているあいだ受け付けます。送ったあと、本体の画面で確認してください。</p>
</body></html>
)HTML";

// 文字列全体が数値として読めたときだけ true。
bool parseDegrees(const String &text, double &value)
{
    if (text.length() == 0) {
        return false;
    }
    char *end = nullptr;
    value = strtod(text.c_str(), &end);
    return end != nullptr && *end == '\0';
}

// 「35.658034, 139.701636」のように、緯度と経度が並んだ文字列を読む。
// 地図アプリからコピーした形をそのまま受けられるよう、度の記号や方角（N/S/E/W）が付いていてもよい。
// 数値がちょうど 2 つのときだけ true（度分秒の表記などは受け付けない）。
bool parseLatLon(const String &text, double &lat, double &lon)
{
    double values[2] = {0, 0};
    int count = 0;
    const char *p = text.c_str();
    while (*p) {
        const bool numberStart = isdigit(static_cast<uint8_t>(*p)) ||
                                 ((*p == '-' || *p == '+' || *p == '.') &&
                                  isdigit(static_cast<uint8_t>(p[1])));
        if (numberStart) {
            if (count == 2) {
                return false;
            }
            char *end = nullptr;
            values[count++] = strtod(p, &end);
            p = end;
        } else {
            // 直前の数値に付いた方角。南と西は負にする。
            if (count > 0 && (*p == 'S' || *p == 's' || *p == 'W' || *p == 'w')) {
                values[count - 1] = -fabs(values[count - 1]);
            }
            ++p;
        }
    }
    lat = values[0];
    lon = values[1];
    return count == 2;
}

// URL の %XX と + を元の文字に戻す。
String urlDecode(const String &text)
{
    String out;
    out.reserve(text.length());
    for (size_t i = 0; i < text.length(); ++i) {
        const char c = text[i];
        if (c == '%' && i + 2 < text.length() && isxdigit(static_cast<uint8_t>(text[i + 1])) &&
            isxdigit(static_cast<uint8_t>(text[i + 2]))) {
            const char hex[3] = {text[i + 1], text[i + 2], '\0'};
            out += static_cast<char>(strtol(hex, nullptr, 16));
            i += 2;
        } else {
            out += c == '+' ? ' ' : c;
        }
    }
    return out;
}

// URL の中の「名前=値」の値を返す（名前の前が ? か & のものだけ）。なければ空。
String queryValue(const String &url, const char *name)
{
    const String key = String(name) + "=";
    int from = 0;
    while (true) {
        const int at = url.indexOf(key, from);
        if (at < 0) {
            return String();
        }
        if (at > 0 && (url[at - 1] == '?' || url[at - 1] == '&')) {
            int end = at + key.length();
            while (end < static_cast<int>(url.length()) && url[end] != '&' && url[end] != '#') {
                ++end;
            }
            return url.substring(at + key.length(), end);
        }
        from = at + 1;
    }
}

// 数字、小数点、符号、カンマ、空白だけでできた「緯度,経度」か。
bool parsePlainPair(const String &text, double &lat, double &lon)
{
    for (size_t i = 0; i < text.length(); ++i) {
        const char c = text[i];
        if (!isdigit(static_cast<uint8_t>(c)) && c != '.' && c != ',' && c != '-' && c != '+' && c != ' ') {
            return false;
        }
    }
    return parseLatLon(text, lat, lon);
}

// 地図アプリの共有リンク（短縮リンクを展開したあとのもの）から場所を読み取る。
// 対応している形:
//   Google マップ   ...?q=36.29,139.81 / .../@36.29,139.81,17z / ...!3d36.29!4d139.81
//   Apple マップ    ...?ll=36.29,139.81 / ...?coordinate=36.29,139.81
//   Yahoo! マップ   ...?lat=36.29&lon=139.81
// 場所の名前がリンクに入っていれば name に入れる。
bool parseMapLink(const String &link, double &lat, double &lon, String &name)
{
    const String url = urlDecode(link);

    // Google マップの場所のページ: 「!3d緯度!4d経度」がその場所の正確な位置（@ のほうは地図の中心）。
    bool found = false;
    const int d3 = url.indexOf("!3d");
    if (d3 >= 0) {
        char *end = nullptr;
        lat = strtod(url.c_str() + d3 + 3, &end);
        if (end != nullptr && strncmp(end, "!4d", 3) == 0) {
            lon = strtod(end + 3, nullptr);
            found = true;
        }
    }
    for (const char *key : {"q", "ll", "coordinate", "query", "destination", "daddr", "center"}) {
        if (!found) {
            found = parsePlainPair(queryValue(url, key), lat, lon);
        }
    }
    if (!found) {
        const String la = queryValue(url, "lat");
        String lo = queryValue(url, "lon");
        if (lo.length() == 0) {
            lo = queryValue(url, "lng");
        }
        found = parseDegrees(la, lat) && parseDegrees(lo, lon);
    }
    if (!found) {
        const int at = url.indexOf("/@");
        if (at >= 0) {
            char *end = nullptr;
            lat = strtod(url.c_str() + at + 2, &end);
            if (end != nullptr && *end == ',') {
                char *end2 = nullptr;
                lon = strtod(end + 1, &end2);
                found = end2 != end + 1;
            }
        }
    }
    if (!found) {
        return false;
    }

    // 名前: Google マップの /place/名前/ か、Apple マップの name=、q=（座標でないとき）。
    const int place = url.indexOf("google.") >= 0 ? url.indexOf("/place/") : -1;
    if (place >= 0) {
        int end = place + 7;
        while (end < static_cast<int>(url.length()) && url[end] != '/' && url[end] != '?' &&
               url[end] != '#') {
            ++end;
        }
        name = url.substring(place + 7, end);
    } else {
        name = queryValue(url, "name");
        if (name.length() == 0) {
            double a, b;
            const String q = queryValue(url, "q");
            if (q.length() && !parsePlainPair(q, a, b)) {
                name = q;
            }
        }
    }
    return true;
}

void handleReceiverPage()
{
    ui::noteActivity();
    g_receiver.send_P(200, "text/html; charset=utf-8", kReceiverPage);
}

void handleDestination()
{
    ui::noteActivity();
    // 場所の指定は 3 通り: url（地図アプリの共有リンク）、ll（「緯度, 経度」をまとめた文字列）、
    // lat と lon。ll にリンクが貼られたときも、リンクとして読む。
    double lat = 0, lon = 0;
    String linkName;
    String link = g_receiver.arg("url");
    if (link.length()) {
        // アドレスの後ろにリンクをそのまま付けて送られた場合（...?url=https://...?q=..&ll=..）、
        // リンクの中の & 以降が別の項目として届く。順番どおりにつなぎ直して元のリンクに戻す。
        bool after = false;
        for (int i = 0; i < g_receiver.args(); ++i) {
            const String key = g_receiver.argName(i);
            if (after && key != "name" && key != "plain") {
                link += "&" + key + "=" + g_receiver.arg(i);
            } else if (key == "url") {
                after = true;
            }
        }
    }
    const String pair = g_receiver.arg("ll");
    if (link.length() == 0 && pair.indexOf("://") >= 0) {
        link = pair;
    }
    bool parsed;
    if (link.length()) {
        parsed = parseMapLink(link, lat, lon, linkName);
    } else if (pair.length()) {
        parsed = parseLatLon(pair, lat, lon);
    } else {
        parsed = parseDegrees(g_receiver.arg("lat"), lat) && parseDegrees(g_receiver.arg("lon"), lon);
    }
    if (!parsed || lat < -85.0 || lat > 85.0 || lon < -180.0 || lon > 180.0) {
        g_receiver.send(400, "text/plain; charset=utf-8",
                        link.length() ? "このリンクから場所を読み取れませんでした。短縮リンクは、展開してから送ってください"
                                      : "緯度と経度を十進の度で指定してください（例: 35.658034, 139.701636）");
        return;
    }
    String name = g_receiver.arg("name");
    if (name.length() == 0) {
        name = linkName;
    }
    name.trim();
    if (name.length() > 90) {
        // 長すぎる名前は、UTF-8 の文字の途中で切らないように縮める。
        int cut = 90;
        while (cut > 0 && (static_cast<uint8_t>(name[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        name.remove(cut);
    }
    offerDestination(lat, lon, name);
    g_receiver.send(200, "text/plain; charset=utf-8",
                    "目的地を受け取りました。本体の画面で確認してください。");
}

String formatDistance(float meters)
{
    char buf[16];
    if (meters < 1000.0f) {
        snprintf(buf, sizeof(buf), "%dm", static_cast<int>(lroundf(meters / 10.0f)) * 10);
    } else if (meters < 10000.0f) {
        snprintf(buf, sizeof(buf), "%.1fkm", meters / 1000.0f);
    } else {
        snprintf(buf, sizeof(buf), "%.0fkm", meters / 1000.0f);
    }
    return buf;
}

String formatDuration(float seconds)
{
    const int minutes = static_cast<int>(lroundf(seconds / 60.0f));
    if (minutes < 60) {
        return "約 " + String(minutes) + " 分";
    }
    return "約 " + String(minutes / 60) + " 時間 " + String(minutes % 60) + " 分";
}

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
        mode_ = Mode::Normal;
        hasCandidate_ = false;
        arrived_ = false;
        offCount_ = 0;
        progress_ = route::Progress();
        route::load();

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
        if (receiving_) {
            g_receiver.stop();
            receiving_ = false;
        }
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

    // 「戻る」: メニューや確認の表示中は、それを閉じて地図に戻る。
    bool back() override
    {
        if (mode_ == Mode::Normal) {
            return false;
        }
        closeToMap();
        return true;
    }

    void draw() override
    {
        if (mode_ == Mode::Menu) {
            drawMenu();
            return;
        }

        lastDrawMs_ = millis();
        shownFix_ = gps::hasFix();
        if (shownFix_) {
            shownLat_ = gps::latitude();
            shownLon_ = gps::longitude();
            if (route::active() && !progress_.valid) {
                progress_ = route::locate(shownLat_, shownLon_);
            }
        }
        drawTiles();
        drawRoute();
        drawMarker();
        drawOverlay();

        ui::button(kZoomOutButton, "－", false, ui::fontLarge);
        ui::button(kZoomInButton, "＋", false, ui::fontLarge);
        ui::text("Z" + String(zoom_), (kZoomOutButton.x + kZoomOutButton.w + kZoomInButton.x) / 2,
                 ui::kBottomBarTop + 34, ui::fontNormal, textdatum_t::middle_center);
        // 追従中は黒地に白文字、追従していないときは白地に黒文字で区別する。
        ui::button(kFollowButton, "現在地", follow_);
        // 案内中は黒地に白文字にする。目的地を選んでいる間は、選ぶのをやめるボタンになる。
        if (mode_ == Mode::PickDest) {
            ui::button(kRouteButton, "やめる");
        } else {
            ui::button(kRouteButton, "ルート", route::active());
        }

        if (mode_ == Mode::ConfirmDest) {
            drawDialog("ここを目的地にしますか？",
                       candidateName_.length() ? candidateName_ : String("現在地からの経路を取得します"),
                       "経路を引く");
        } else if (mode_ == Mode::ConfirmOrigin) {
            drawDialog("GPS が測位できていません", "直前に見ていた場所を出発地にしますか？", "はい");
        }
    }

    void tap(int x, int y) override
    {
        if (mode_ == Mode::Menu) {
            tapMenu(x, y);
            return;
        }
        if (mode_ == Mode::ConfirmDest || mode_ == Mode::ConfirmOrigin) {
            tapDialog(x, y);
            return;
        }
        if (kRouteButton.contains(x, y)) {
            if (mode_ == Mode::PickDest) {
                closeToMap();
            } else {
                // 測位できていないときの出発地の候補として、今見ている場所を覚えておく。
                fallbackLat_ = lat_;
                fallbackLon_ = lon_;
                mode_ = Mode::Menu;
                ui::requestRedraw();
            }
            return;
        }
        if (mode_ == Mode::PickDest && y >= kMapTop + kPickBannerHeight && y < kMapTop + kMapHeight) {
            // タップした地点を目的地の候補にする。
            double centerX, centerY;
            latLonToWorld(lat_, lon_, zoom_, centerX, centerY);
            worldToLatLon(llround(centerX) + (x - ui::kWidth / 2),
                          llround(centerY) + (y - (kMapTop + kMapHeight / 2)), zoom_, candidateLat_,
                          candidateLon_);
            candidateName_ = String();
            hasCandidate_ = true;
            dialogTop_ = y > 500 ? kMapTop + 50 : 540;  // 印が隠れない側に出す
            mode_ = Mode::ConfirmDest;
            ui::requestRedraw();
            return;
        }
        if (bandRefetchVisible_ && kBandRefetch.contains(x, y)) {
            refetch();
            return;
        }
        if (kZoomOutButton.contains(x, y)) {
            stepZoom(-1);
        } else if (kZoomInButton.contains(x, y)) {
            stepZoom(1);
        } else if (kFollowButton.contains(x, y)) {
            follow_ = !follow_;
            if (follow_ && gps::hasFix()) {
                centerOnPosition(gps::latitude(), gps::longitude());
            }
            viewChanged();
        }
    }

    // ドラッグ: 指を離した時点で、動かした分だけ地図をずらして 1 回描き直す。
    void drag(int x, int y, int dx, int dy) override
    {
        if (y >= kMapTop + kMapHeight || (mode_ != Mode::Normal && mode_ != Mode::PickDest)) {
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
        updateReceiver();
        takeOffer();
        if (mode_ != Mode::Normal && mode_ != Mode::PickDest) {
            return;  // メニューや確認の表示中は、地図を描き直さない
        }
        updateProgress();
        refreshBand();
        if (route::active() && follow_) {
            // 案内中で現在地を追っている間は、無操作でもスリープしない（SPEC.md 4.9）。
            ui::noteActivity();
        }

        const bool fix = gps::hasFix();
        if (millis() - lastDrawMs_ < kGpsRedrawIntervalMs) {
            return;
        }
        if (fix != shownFix_) {
            // 測位できた / 見失った
            if (fix && follow_) {
                centerOnPosition(gps::latitude(), gps::longitude());
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
                centerOnPosition(gps::latitude(), gps::longitude());
            }
            viewChanged();
        }
    }

private:
    enum class Mode {
        Normal,
        Menu,           // ルートのメニュー
        PickDest,       // 地図をタップして目的地を選んでいる
        ConfirmDest,    // 目的地の候補を確認している
        ConfirmOrigin,  // 測位できていないときの出発地を確認している
    };

    // ---- ルート案内 ----

    // メニューや確認を閉じて、ふつうの地図に戻る。
    void closeToMap()
    {
        mode_ = Mode::Normal;
        hasCandidate_ = false;
        ui::requestCleanRefresh();
    }

    // 目的地の受け口を、WiFi の状態に合わせて動かす / 止める。
    void updateReceiver()
    {
        const bool want = net::connected() && !webfiles::running();
        if (want && !receiving_) {
            static bool registered = false;
            if (!registered) {
                registered = true;
                g_receiver.on("/", HTTP_GET, handleReceiverPage);
                // スマホのショートカットから作りやすいよう、GET でも受ける。受け取っても
                // 画面で確認するまで何も変えないので、GET で受けても害はない。
                g_receiver.on("/api/destination", HTTP_ANY, handleDestination);
                g_receiver.onNotFound(
                    []() { g_receiver.send(404, "text/plain; charset=utf-8", "Not found"); });
            }
            g_receiver.begin();
            receiving_ = true;
        } else if (!want && receiving_) {
            g_receiver.stop();
            receiving_ = false;
        }
        if (receiving_) {
            g_receiver.handleClient();
        }
    }

    // 渡された目的地の候補があれば、地図の中心に表示して確認を求める。
    void takeOffer()
    {
        if (!g_offer.pending) {
            return;
        }
        g_offer.pending = false;
        if (mode_ == Mode::Normal) {
            fallbackLat_ = lat_;
            fallbackLon_ = lon_;
        }
        candidateLat_ = g_offer.lat;
        candidateLon_ = g_offer.lon;
        candidateName_ = g_offer.name;
        hasCandidate_ = true;
        lat_ = candidateLat_;
        lon_ = candidateLon_;
        follow_ = false;
        dialogTop_ = 540;
        mode_ = Mode::ConfirmDest;
        viewChanged();
    }

    // 候補の地点までの経路を取得する。成功したら、確かめられるよう経路の全体を表示する。
    void runFetch(double fromLat, double fromLon, bool showWhole)
    {
        ui::message("経路を取得しています…");
        String error;
        if (route::fetch(fromLat, fromLon, candidateLat_, candidateLon_, candidateName_, error)) {
            arrived_ = false;
            offCount_ = 0;
            progress_ = route::Progress();
            if (showWhole) {
                fitWhole();
            }
        } else {
            ui::message("経路を取得できません", error);
            delay(3000);
        }
        mode_ = Mode::Normal;
        hasCandidate_ = false;
        viewChanged();
    }

    // 今の目的地までの経路を、現在地から引き直す。
    void refetch()
    {
        if (!route::active()) {
            return;
        }
        if (!gps::hasFix()) {
            ui::message("GPS が測位できていません");
            delay(2000);
            closeToMap();
            return;
        }
        candidateLat_ = route::info().destLat;
        candidateLon_ = route::info().destLon;
        candidateName_ = route::info().destName;
        runFetch(gps::latitude(), gps::longitude(), false);
    }

    // 経路の全体が収まる位置と縮尺にする。
    void fitWhole()
    {
        const auto &points = route::points();
        if (points.empty()) {
            return;
        }
        uint32_t minX = points[0].x, maxX = points[0].x, minY = points[0].y, maxY = points[0].y;
        for (const route::Point &p : points) {
            minX = std::min(minX, p.x);
            maxX = std::max(maxX, p.x);
            minY = std::min(minY, p.y);
            maxY = std::max(maxY, p.y);
        }
        const route::Point center = {minX + (maxX - minX) / 2, minY + (maxY - minY) / 2};
        route::toLatLon(center, lat_, lon_);
        zoom_ = kZoomLevels[0];
        for (int i = kZoomLevelCount - 1; i >= 0; --i) {
            const int shift = 24 - kZoomLevels[i];
            if (static_cast<int>((maxX - minX) >> shift) <= ui::kWidth - 60 &&
                static_cast<int>((maxY - minY) >> shift) <= kMapHeight - kBandHeight - 100) {
                zoom_ = kZoomLevels[i];
                break;
            }
        }
        follow_ = false;
    }

    // 現在地を経路に当てはめ、外れたか、着いたかを判断する。
    void updateProgress()
    {
        if (!route::active() || !gps::hasFix() || millis() - lastProgressMs_ < kProgressIntervalMs) {
            return;
        }
        lastProgressMs_ = millis();
        progress_ = route::locate(gps::latitude(), gps::longitude());
        if (!progress_.valid) {
            return;
        }
        // 1 回の測位のぶれで「外れた」と出ないよう、続けて離れていたときだけ外れたとみなす。
        offCount_ = progress_.offRouteM > kOffRouteM ? std::min(offCount_ + 1, 10) : 0;
        if (offCount_ == 0 && progress_.remainingM < kArriveM) {
            arrived_ = true;
            route::clear();
            progress_ = route::Progress();
            viewChanged();
        }
    }

    // 現在地を追うときの地図の中心を決める。案内の帯が出ている間は、帯に隠れない範囲の中で、
    // 現在地を進む向きの反対側に寄せて、進む先を広く見せる（地図は北が上のまま回さないので、
    // 南へ進むときは現在地が上寄り、北へ進むときは下寄りになる）。
    // 進む向きは、経路に沿って少し先（画面で 260 ピクセルぶん）の地点の方向で決める。
    void centerOnPosition(double lat, double lon)
    {
        if (!route::active() && !arrived_) {
            lat_ = lat;
            lon_ = lon;
            return;
        }
        // 帯に隠れない範囲の中心。進む向きが分からないとき（経路から外れている、到着後）は
        // ここに現在地を置く。
        const float visibleTop = kMapTop + kBandHeight;
        const float visibleBottom = kMapTop + kMapHeight;
        const float halfWidth = ui::kWidth / 2.0f;
        const float halfHeight = (visibleBottom - visibleTop) / 2.0f;
        float markerX = halfWidth;
        float markerY = (visibleTop + visibleBottom) / 2.0f;

        double worldX, worldY;
        latLonToWorld(lat, lon, zoom_, worldX, worldY);
        const route::Progress here = route::locate(lat, lon);
        const float metersPerPixel = 40075016.686f * cosf(static_cast<float>(lat * M_PI / 180.0)) /
                                     (static_cast<float>(kTileSize) * (1 << zoom_));
        double aheadLat, aheadLon;
        if (here.valid && here.offRouteM <= kOffRouteM &&
            route::pointAhead(260.0f * metersPerPixel, aheadLat, aheadLon)) {
            double aheadX, aheadY;
            latLonToWorld(aheadLat, aheadLon, zoom_, aheadX, aheadY);
            const float dx = static_cast<float>(aheadX - worldX);
            const float dy = static_cast<float>(aheadY - worldY);
            const float length = sqrtf(dx * dx + dy * dy);
            if (length >= 8.0f) {
                const float ux = dx / length, uy = dy / length;
                // その向きに、見える範囲の端までの 6 割だけ、反対側へ寄せる。
                const float reach = std::min(fabsf(ux) > 0.01f ? halfWidth / fabsf(ux) : 1e9f,
                                             fabsf(uy) > 0.01f ? halfHeight / fabsf(uy) : 1e9f);
                markerX -= ux * reach * 0.6f;
                markerY -= uy * reach * 0.6f;
            }
        }
        const float offsetX = markerX - ui::kWidth / 2.0f;
        const float offsetY = markerY - (kMapTop + kMapHeight / 2.0f);
        worldToLatLon(worldX - offsetX, std::max(worldY - offsetY, 0.0), zoom_, lat_, lon_);
    }

    // 緯度経度を画面上の位置にする。
    void toScreen(double lat, double lon, int &x, int &y) const
    {
        double centerX, centerY, worldX, worldY;
        latLonToWorld(lat_, lon_, zoom_, centerX, centerY);
        latLonToWorld(lat, lon, zoom_, worldX, worldY);
        x = ui::kWidth / 2 + static_cast<int>(llround(worldX) - llround(centerX));
        y = kMapTop + kMapHeight / 2 + static_cast<int>(llround(worldY) - llround(centerY));
    }

    // 目的地の印。先端が (lat, lon) を指す。
    void drawPin(double lat, double lon)
    {
        int x, y;
        toScreen(lat, lon, x, y);
        if (x < -20 || x >= ui::kWidth + 20 || y < kMapTop || y >= kMapTop + kMapHeight + 44) {
            return;
        }
        display.fillCircle(x, y - 28, 17, TFT_WHITE);
        display.fillTriangle(x - 15, y - 21, x + 15, y - 21, x, y + 5, TFT_WHITE);
        display.fillCircle(x, y - 28, 14, TFT_BLACK);
        display.fillTriangle(x - 12, y - 22, x + 12, y - 22, x, y, TFT_BLACK);
        display.fillCircle(x, y - 28, 5, TFT_WHITE);
    }

    // 線分 (ax, ay)-(bx, by) を、四角形の内側に切り詰める。全体が外なら false。
    static bool clipSegment(float &ax, float &ay, float &bx, float &by, float left, float top,
                            float right, float bottom)
    {
        float t0 = 0.0f, t1 = 1.0f;
        const float dx = bx - ax, dy = by - ay;
        const float p[4] = {-dx, dx, -dy, dy};
        const float q[4] = {ax - left, right - ax, ay - top, bottom - ay};
        for (int i = 0; i < 4; ++i) {
            if (p[i] == 0.0f) {
                if (q[i] < 0.0f) {
                    return false;
                }
            } else {
                const float t = q[i] / p[i];
                if (p[i] < 0.0f) {
                    t0 = std::max(t0, t);
                } else {
                    t1 = std::min(t1, t);
                }
            }
        }
        if (t0 > t1) {
            return false;
        }
        bx = ax + dx * t1;
        by = ay + dy * t1;
        ax = ax + dx * t0;
        ay = ay + dy * t0;
        return true;
    }

    // 経路の線と、目的地の印を描く。線は地図の道路と見分けがつくよう、白いふち取りを付ける。
    void drawRoute()
    {
        display.setClipRect(0, kMapTop, ui::kWidth, kMapHeight);
        const auto &points = route::points();
        if (route::active() && points.size() >= 2) {
            double centerX, centerY;
            latLonToWorld(lat_, lon_, zoom_, centerX, centerY);
            const int64_t left = llround(centerX) - ui::kWidth / 2;
            const int64_t top = llround(centerY) - kMapHeight / 2;
            const int shift = 24 - zoom_;
            constexpr int64_t kFar = 20000;  // 画面から遠い点は、この範囲に丸めて扱う
            for (int pass = 0; pass < 2; ++pass) {
                const float radius = pass == 0 ? 7.0f : 4.0f;
                const uint32_t color = pass == 0 ? ui::kWhite : 0x000000u;
                int prevX = 0, prevY = 0;
                for (size_t i = 0; i < points.size(); ++i) {
                    const int x = static_cast<int>(std::min(
                        std::max(static_cast<int64_t>(points[i].x >> shift) - left, -kFar), kFar));
                    const int y = kMapTop + static_cast<int>(std::min(
                        std::max(static_cast<int64_t>(points[i].y >> shift) - top, -kFar), kFar));
                    if (i == 0) {
                        prevX = x;
                        prevY = y;
                        continue;
                    }
                    // 縮小表示では点が密になるので、ほとんど動かない点は飛ばす。
                    if (abs(x - prevX) < 3 && abs(y - prevY) < 3 && i + 1 < points.size()) {
                        continue;
                    }
                    // 太い線は、描画範囲の指定（setClipRect）を少しはみ出して描かれることがある
                    // （ステータスバーや下の操作バーに線の端が出るのを実機で確認）。
                    // そこで、線を地図の枠の内側に切り詰めてから描く。
                    float ax = prevX, ay = prevY, bx = x, by = y;
                    if (clipSegment(ax, ay, bx, by, -10.0f, kMapTop + 8.0f, ui::kWidth + 10.0f,
                                    kMapTop + kMapHeight - 9.0f)) {
                        display.drawWideLine(lroundf(ax), lroundf(ay), lroundf(bx), lroundf(by), radius,
                                             color);
                    }
                    prevX = x;
                    prevY = y;
                }
                delay(1);  // 他のタスクに処理を譲る
            }
            drawPin(route::info().destLat, route::info().destLon);
        }
        if (hasCandidate_) {
            drawPin(candidateLat_, candidateLon_);
        }
        display.clearClipRect();
    }

    // ---- 案内の帯 ----

    // 曲がる向きの矢印の種類。
    enum class Arrow { Straight, Turn, UTurn, Destination };

    // 案内の種別（Valhalla の番号）を、矢印の種類と角度（上が 0、右回りが正）にする。
    static Arrow arrowFor(uint8_t type, int &angle)
    {
        angle = 0;
        switch (type) {
            case 4:
            case 5:
            case 6:
                return Arrow::Destination;
            case 12:
            case 13:
                return Arrow::UTurn;
            case 9:
            case 18:
            case 20:
            case 23:
                angle = 45;
                return Arrow::Turn;
            case 10:
                angle = 90;
                return Arrow::Turn;
            case 11:
                angle = 135;
                return Arrow::Turn;
            case 16:
            case 19:
            case 21:
            case 24:
                angle = -45;
                return Arrow::Turn;
            case 15:
                angle = -90;
                return Arrow::Turn;
            case 14:
                angle = -135;
                return Arrow::Turn;
            default:
                return Arrow::Straight;
        }
    }

    // 矢じり。先端が (x, y) から dir の向きに出る。
    static void drawArrowHead(float x, float y, float dirX, float dirY)
    {
        const float tipX = x + dirX * 26, tipY = y + dirY * 26;
        const float baseX = x - dirX * 6, baseY = y - dirY * 6;
        display.fillTriangle(lroundf(tipX), lroundf(tipY), lroundf(baseX - dirY * 27),
                             lroundf(baseY + dirX * 27), lroundf(baseX + dirY * 27),
                             lroundf(baseY - dirX * 27), TFT_BLACK);
    }

    // 曲がる向きの大きな矢印を、(cx, cy) を中心にした 120 ピクセル四方に描く。
    static void drawArrow(Arrow arrow, int angle, int cx, int cy)
    {
        constexpr float kStroke = 10.0f;  // 線の太さの半分
        if (arrow == Arrow::Destination) {
            // 目的地: 地図上の印と同じ形を大きく描く。
            display.fillCircle(cx, cy - 18, 34, TFT_BLACK);
            display.fillTriangle(cx - 30, cy - 4, cx + 30, cy - 4, cx, cy + 54, TFT_BLACK);
            display.fillCircle(cx, cy - 18, 13, TFT_WHITE);
            return;
        }
        if (arrow == Arrow::UTurn) {
            display.drawWideLine(cx + 28, cy + 50, cx + 28, cy - 28, kStroke, TFT_BLACK);
            display.drawWideLine(cx + 28, cy - 28, cx - 28, cy - 28, kStroke, TFT_BLACK);
            display.drawWideLine(cx - 28, cy - 28, cx - 28, cy + 14, kStroke, TFT_BLACK);
            drawArrowHead(cx - 28, cy + 14, 0, 1);
            return;
        }
        if (arrow == Arrow::Straight) {
            display.drawWideLine(cx, cy + 50, cx, cy - 20, kStroke, TFT_BLACK);
            drawArrowHead(cx, cy - 20, 0, -1);
            return;
        }
        // 下から来て、角で向きを変える矢印。曲がる側に腕が伸びるぶん、軸を反対側に寄せる。
        const float rad = angle * static_cast<float>(M_PI) / 180.0f;
        const float dirX = sinf(rad), dirY = -cosf(rad);
        const float stemX = cx - dirX * 22;
        const float elbowY = cy + (abs(angle) > 90 ? -22 : (abs(angle) < 90 ? 14 : -4));
        const float length = abs(angle) == 90 ? 40 : 44;
        const float endX = stemX + dirX * length, endY = elbowY + dirY * length;
        display.drawWideLine(lroundf(stemX), cy + 50, lroundf(stemX), lroundf(elbowY), kStroke,
                             TFT_BLACK);
        display.drawWideLine(lroundf(stemX), lroundf(elbowY), lroundf(endX), lroundf(endY), kStroke,
                             TFT_BLACK);
        drawArrowHead(endX, endY, dirX, dirY);
    }

    // 帯に出す内容。描く前にまとめて決めておき、前回と同じなら書き換えない。
    struct Band {
        enum Kind { Arrived, NoFix, OffRoute, Guide } kind = NoFix;
        Arrow arrow = Arrow::Straight;
        int angle = 0;
        String number;  // 距離の数字（「150」「1.2」）。空なら「すぐ」
        String unit;    // 「m」「km」
        String line2;   // 2 段目
        String line3;   // 3 段目

        String key() const
        {
            return String(static_cast<int>(kind)) + "|" + String(static_cast<int>(arrow)) + "|" +
                   String(angle) + "|" + number + unit + "|" + line2 + "|" + line3;
        }
    };

    Band currentBand() const
    {
        Band band;
        if (arrived_) {
            band.kind = Band::Arrived;
            band.line2 = "目的地に到着しました";
            return band;
        }
        if (!gps::hasFix() || !progress_.valid) {
            band.kind = Band::NoFix;
            band.line2 = "GPS 衛星を探しています";
            band.line3 = "全長 " + formatDistance(route::info().lengthM);
            return band;
        }

        band.line3 = "残り " + formatDistance(progress_.remainingM);
        if (net::timeValid()) {
            const time_t eta = time(nullptr) + static_cast<time_t>(progress_.remainingSec);
            struct tm t;
            localtime_r(&eta, &t);
            char buf[24];
            snprintf(buf, sizeof(buf), "　到着 %d:%02d ごろ", t.tm_hour, t.tm_min);
            band.line3 += buf;
        }
        if (offCount_ >= 2) {
            band.kind = Band::OffRoute;
            band.line2 = "ルートから外れています";
            return band;
        }

        band.kind = Band::Guide;
        const auto &maneuvers = route::maneuvers();
        if (progress_.nextManeuver < 0 || progress_.nextManeuver >= static_cast<int>(maneuvers.size())) {
            band.line2 = "道なり";
            return band;
        }
        const route::Maneuver &m = maneuvers[progress_.nextManeuver];
        band.arrow = arrowFor(m.type, band.angle);
        band.line2 = route::maneuverLabel(m.type);
        if (m.name.length()) {
            band.line2 += "　" + m.name;
        }
        // 距離は、遠いうちは粗く、近づいたら細かく刻む（書き換えの回数を抑えるため）。
        const float meters = progress_.toNextM;
        char buf[12];
        if (meters < 15.0f) {
            // 数字は出さず「すぐ」と表示する
        } else if (meters < 1000.0f) {
            const int step = meters < 300.0f ? 10 : 50;
            snprintf(buf, sizeof(buf), "%d", static_cast<int>(lroundf(meters / step)) * step);
            band.number = buf;
            band.unit = "m";
        } else {
            snprintf(buf, sizeof(buf), meters < 10000.0f ? "%.1f" : "%.0f", meters / 1000.0f);
            band.number = buf;
            band.unit = "km";
        }
        return band;
    }

    // 大きな文字を描き、描いた幅を返す。size は文字の拡大率。
    static int drawBig(const String &text, int x, int bottom, const lgfx::IFont *font, float size)
    {
        display.setFont(font);
        display.setTextSize(size);
        display.setTextDatum(textdatum_t::bottom_left);
        display.setTextColor(TFT_BLACK);
        display.drawString(text, x, bottom);
        const int width = display.textWidth(text);
        display.setTextSize(1);
        return width;
    }

    // 地図の上部に出す案内。帯だけを書き換えるとき（白黒 2 値の高速な書き換え）にも使うので、
    // 灰色は使わない。
    void drawGuidance()
    {
        const Band band = currentBand();
        shownBandKey_ = band.key();
        bandRefetchVisible_ = false;

        display.fillRect(0, kMapTop, ui::kWidth, kBandHeight, TFT_WHITE);
        display.fillRect(0, kMapTop + kBandHeight, ui::kWidth, 3, TFT_BLACK);
        const int iconX = 68;
        const int iconY = kMapTop + kBandRow1 / 2;
        const int textX = 144;
        const int bigBottom = kMapTop + kBandRow1 - 14;  // 1 段目の大きな文字の下端
        const lgfx::IFont *const bigJapanese = &fonts::lgfxJapanGothicP_40;

        switch (band.kind) {
            case Band::Arrived:
                drawArrow(Arrow::Destination, 0, iconX, iconY);
                drawBig("到着", textX, bigBottom + 6, bigJapanese, 2.0f);
                break;
            case Band::NoFix:
                drawBig("GPS 探索中", 16, bigBottom + 6, bigJapanese, 2.0f);
                break;
            case Band::OffRoute:
                drawBig("ルート外", 16, bigBottom + 6, bigJapanese, 2.0f);
                ui::button(kBandRefetch, "再検索", false, ui::fontLarge);
                bandRefetchVisible_ = true;
                break;
            case Band::Guide: {
                drawArrow(band.arrow, band.angle, iconX, iconY);
                if (band.number.length() == 0) {
                    drawBig(band.line2 == "道なり" ? "直進" : "すぐ", textX, bigBottom + 6, bigJapanese,
                            2.0f);
                } else {
                    // 数字は時計と同じ大きな数字用のフォント、単位は日本語フォントを拡大して並べる。
                    const int width = drawBig(band.number, textX, bigBottom + 6, &fonts::Font8, 1.0f);
                    drawBig(band.unit, textX + width + 10, bigBottom + 4, bigJapanese, 1.5f);
                }
                break;
            }
        }

        const int row2 = kMapTop + kBandRow1 + kBandRow2 / 2;
        const int row3 = kMapTop + kBandRow1 + kBandRow2 + kBandRow3 / 2 - 2;
        ui::text(ui::fitText(band.line2, ui::kWidth - 32, bigJapanese), 16, row2, bigJapanese,
                 textdatum_t::middle_left);
        ui::text(band.line3, 16, row3, ui::fontMedium, textdatum_t::middle_left);
    }

    // 帯の内容が変わっていたら、帯だけを書き換える。地図全体の描き直し（最短 10 秒おき）を
    // 待つと、「150m 先」の表示が実際より遅れるため。
    void refreshBand()
    {
        if (mode_ != Mode::Normal || !(route::active() || arrived_) ||
            millis() - lastBandMs_ < kBandIntervalMs) {
            return;
        }
        if (currentBand().key() == shownBandKey_) {
            return;
        }
        lastBandMs_ = millis();
        ui::beginPartial();
        drawGuidance();
        ui::flush();
    }

    // 地図に重ねる確認の表示。
    void drawDialog(const String &line1, const String &line2, const char *yesLabel)
    {
        const ui::Rect box = {30, dialogTop_, 480, 250};
        display.fillRoundRect(box.x, box.y, box.w, box.h, 16, TFT_WHITE);
        for (int i = 0; i < 3; ++i) {
            display.drawRoundRect(box.x + i, box.y + i, box.w - i * 2, box.h - i * 2, 16 - i,
                                  TFT_BLACK);
        }
        const int cx = ui::kWidth / 2;
        ui::text(line1, cx, box.y + 52, ui::fontMedium, textdatum_t::middle_center);
        ui::text(ui::fitText(line2, box.w - 40, ui::fontSmall), cx, box.y + 100, ui::fontSmall,
                 textdatum_t::middle_center, ui::gray(60));
        ui::button(dialogYes(), yesLabel, true, ui::fontMedium);
        ui::button(dialogNo(), "やめる", false, ui::fontMedium);
    }

    ui::Rect dialogYes() const { return {60, dialogTop_ + 150, 200, 76}; }
    ui::Rect dialogNo() const { return {280, dialogTop_ + 150, 200, 76}; }

    void tapDialog(int x, int y)
    {
        if (dialogNo().contains(x, y)) {
            closeToMap();
        } else if (dialogYes().contains(x, y)) {
            if (mode_ == Mode::ConfirmOrigin) {
                runFetch(fallbackLat_, fallbackLon_, true);
            } else if (gps::hasFix()) {
                runFetch(gps::latitude(), gps::longitude(), true);
            } else {
                mode_ = Mode::ConfirmOrigin;
                ui::requestRedraw();
            }
        }
    }

    void drawMenu()
    {
        const int cx = ui::kWidth / 2;
        const bool active = route::active();
        if (active) {
            const route::Info &info = route::info();
            ui::text(ui::fitText("目的地: " + (info.destName.length() ? info.destName
                                                                      : String("地図で選んだ地点")),
                                 ui::kWidth - 40, ui::fontMedium),
                     cx, 110, ui::fontMedium, textdatum_t::middle_center);
            ui::text("距離 " + formatDistance(info.lengthM) + "　" + formatDuration(info.timeSec), cx,
                     160, ui::fontNormal, textdatum_t::middle_center);
            String fetched = "取得した日時は不明";
            if (info.fetchedAt != 0) {
                struct tm t;
                localtime_r(&info.fetchedAt, &t);
                char buf[48];
                snprintf(buf, sizeof(buf), "%d/%d %d:%02d に取得（7 日で消えます）", t.tm_mon + 1,
                         t.tm_mday, t.tm_hour, t.tm_min);
                fetched = buf;
            }
            ui::text(fetched, cx, 204, ui::fontSmall, textdatum_t::middle_center, ui::gray(60));
            ui::text("経路は目安です。標識と交通規制を優先してください", cx, 256, ui::fontSmall,
                     textdatum_t::middle_center, ui::gray(60));
        } else {
            ui::text("ルートはありません", cx, 130, ui::fontMedium, textdatum_t::middle_center);
            ui::text("目的地を決めると、現在地からの経路を引きます", cx, 184, ui::fontSmall,
                     textdatum_t::middle_center, ui::gray(60));
        }

        ui::button(kMenuPick, "地図で目的地を選ぶ", false, ui::fontMedium);
        if (active) {
            ui::button(kMenuWhole, "全体を表示", false, ui::fontMedium);
            ui::button(kMenuRefetch, "現在地から再検索", false, ui::fontMedium);
            ui::button(kMenuStop, "案内をやめる", false, ui::fontMedium);
        }

        // 経路の取得とスマホ連携に必要なものの状態
        String note1, note2;
        if (settings::routeKey().length() == 0) {
            note1 = "API キーが未設定です";
            note2 = "設定 →「ルート案内」で入力してください";
        } else if (receiving_) {
            note1 = "スマホから目的地を送る先";
            note2 = "http://" + WiFi.localIP().toString() + "/";
        } else {
            note1 = "WiFi に接続されていません";
            note2 = "経路の取得と、スマホからの受信には WiFi が要ります";
        }
        ui::text(note1, cx, 744, ui::fontSmall, textdatum_t::middle_center, ui::gray(60));
        ui::text(note2, cx, 784, receiving_ && settings::routeKey().length() ? ui::fontMedium : ui::fontSmall,
                 textdatum_t::middle_center);

        ui::button(kMenuClose, "閉じる", false, ui::fontMedium);
    }

    void tapMenu(int x, int y)
    {
        const bool active = route::active();
        if (kMenuClose.contains(x, y)) {
            closeToMap();
        } else if (kMenuPick.contains(x, y)) {
            mode_ = Mode::PickDest;
            hasCandidate_ = false;
            ui::requestCleanRefresh();
        } else if (active && kMenuWhole.contains(x, y)) {
            fitWhole();
            mode_ = Mode::Normal;
            viewChanged();
        } else if (active && kMenuRefetch.contains(x, y)) {
            refetch();
        } else if (active && kMenuStop.contains(x, y)) {
            route::clear();
            progress_ = route::Progress();
            arrived_ = false;
            closeToMap();
        }
    }

    // ---- 地図 ----

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
            if (follow_ && gps::hasFix()) {
                // 現在地を画面上の同じ位置に保つ（縮尺が変わると、中心からのずれも変わるため）。
                centerOnPosition(gps::latitude(), gps::longitude());
            }
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
        int x, y;
        toScreen(shownLat_, shownLon_, x, y);
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
        bandRefetchVisible_ = false;
        shownBandKey_ = String();
        if (mode_ == Mode::PickDest) {
            // 目的地を選んでいる間の案内
            display.fillRect(0, kMapTop, ui::kWidth, kPickBannerHeight, TFT_BLACK);
            ui::text("目的地の場所をタップしてください", ui::kWidth / 2,
                     kMapTop + kPickBannerHeight / 2, ui::fontNormal, textdatum_t::middle_center,
                     ui::kWhite);
        } else if (route::active() || arrived_) {
            drawGuidance();
        } else {
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
        }

        if (!board::sdReady()) {
            ui::text("SD カードがありません（地図を保存できません）", ui::kWidth / 2,
                     kMapTop + kMapHeight / 2, ui::fontNormal, textdatum_t::middle_center);
        }

        // 右下: 出典。経路を表示している間は、経路の出典も並べる。
        const char *credit =
            route::active() ? "地理院タイル　経路: Stadia Maps, OpenStreetMap" : "地理院タイル";
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

    // ルート案内
    Mode mode_ = Mode::Normal;
    bool receiving_ = false;          // 目的地の受け口を動かしているか
    bool hasCandidate_ = false;       // 確認中の目的地の候補があるか
    double candidateLat_ = 0;
    double candidateLon_ = 0;
    String candidateName_;
    double fallbackLat_ = 0;          // 測位できていないときに出発地にする場所
    double fallbackLon_ = 0;
    int dialogTop_ = 540;
    route::Progress progress_;
    uint32_t lastProgressMs_ = 0;
    int offCount_ = 0;                // 経路から続けて離れていた回数
    bool arrived_ = false;
    bool bandRefetchVisible_ = false;
    String shownBandKey_;             // 最後に描いた帯の内容（書き換えるかどうかの判断に使う）
    uint32_t lastBandMs_ = 0;

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

void offerDestination(double lat, double lon, const String &name)
{
    if (lat < -85.0 || lat > 85.0 || lon < -180.0 || lon > 180.0) {
        return;
    }
    g_offer.lat = lat;
    g_offer.lon = lon;
    g_offer.name = name;
    g_offer.pending = true;
    ui::noteActivity();
}

}  // namespace apps
