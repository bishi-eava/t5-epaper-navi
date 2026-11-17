#include "route.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>

#include <algorithm>
#include <cmath>

#include "board.h"
#include "net.h"
#include "route_cert.h"
#include "safewrite.h"
#include "settings.h"

namespace route {
namespace {

constexpr char kDir[] = "/t5-epaper";
constexpr char kPath[] = "/t5-epaper/route.json";
constexpr char kTempPath[] = "/t5-epaper/route.json.tmp";

// 経路サービス。差し替えるときは、ここと fetch() を書き換える。
constexpr char kServiceUrl[] = "https://api.stadiamaps.com/route/v1?api_key=";
// 50cc の原付向けの設定（SPEC.md 4.9）。値は実走して調整する。
constexpr int kTopSpeedKmh = 30;       // 所要時間をこの速度で見積もらせる
constexpr float kUsePrimary = 0.2f;    // 幹線道路の使いやすさ（0 = 避ける 〜 1 = 好む）

// 保存してよい期間。Stadia Maps の規約が、取得した結果の端末内への保存を 7 日までとしている。
constexpr time_t kKeepSeconds = 7 * 24 * 60 * 60;

constexpr size_t kMaxResponseBytes = 2 * 1024 * 1024;
constexpr double kWorld = 4294967296.0;            // 2^32
constexpr float kEarthCircumferenceM = 40075016.686f;

// 大きな JSON を PSRAM に置くための割り当て。
struct PsramAllocator : ArduinoJson::Allocator {
    void *allocate(size_t size) override { return ps_malloc(size); }
    void deallocate(void *pointer) override { free(pointer); }
    void *reallocate(void *pointer, size_t size) override { return ps_realloc(pointer, size); }
};
PsramAllocator g_allocator;

bool g_loaded = false;  // SD カードからの読み込みを済ませたか
bool g_active = false;
Info g_info;
std::vector<Point> g_points;
std::vector<float> g_along;  // 各点までの道のり（m）
std::vector<Maneuver> g_maneuvers;
size_t g_lastSegment = 0;    // 前回、現在地に最も近かった区間
float g_lastAlong = 0;       // 前回の現在地までの道のり（m）

float distanceM(double lat1, double lon1, double lat2, double lon2)
{
    const float dy = static_cast<float>(lat2 - lat1) * 110574.0f;
    const float dx = static_cast<float>(lon2 - lon1) * 111320.0f *
                     cosf(static_cast<float>((lat1 + lat2) * 0.5 * M_PI / 180.0));
    return sqrtf(dx * dx + dy * dy);
}

// 経路の線（Google の polyline 形式、小数 6 桁）を点の並びに展開する。
bool decodeShape(const char *shape, std::vector<Point> &points, std::vector<float> &along)
{
    const size_t length = strlen(shape);
    points.reserve(length / 3 + 16);
    along.reserve(length / 3 + 16);

    int32_t lat = 0, lon = 0;
    double prevLat = 0, prevLon = 0;
    size_t i = 0;
    while (i < length) {
        for (int k = 0; k < 2; ++k) {
            uint32_t result = 0;
            int shift = 0;
            int b;
            do {
                if (i >= length || shift > 30) {
                    return false;
                }
                b = static_cast<uint8_t>(shape[i++]) - 63;
                if (b < 0 || b > 63) {
                    return false;
                }
                result |= static_cast<uint32_t>(b & 0x1f) << shift;
                shift += 5;
            } while (b >= 0x20);
            const int32_t delta = (result & 1) ? ~static_cast<int32_t>(result >> 1)
                                               : static_cast<int32_t>(result >> 1);
            (k == 0 ? lat : lon) += delta;
        }
        const double la = lat / 1e6;
        const double lo = lon / 1e6;
        if (la < -85.0 || la > 85.0 || lo < -180.0 || lo > 180.0) {
            return false;
        }
        along.push_back(points.empty() ? 0.0f : along.back() + distanceM(prevLat, prevLon, la, lo));
        points.push_back(toPoint(la, lo));
        prevLat = la;
        prevLon = lo;
    }
    return points.size() >= 2;
}

// 道路名の候補（番号や英語名が混ざる）から、日本語の名前を 1 つ選ぶ。
String pickName(JsonArrayConst names)
{
    for (JsonVariantConst item : names) {
        const char *s = item.as<const char *>();
        if (s == nullptr) {
            continue;
        }
        bool japanese = false;
        for (const char *c = s; *c; ++c) {
            if (static_cast<uint8_t>(*c) >= 0x80) {
                japanese = true;
                break;
            }
        }
        if (japanese) {
            String name = s;
            const int slash = name.indexOf('/');  // 「東名入口/Tomei」のような併記は前半だけ使う
            if (slash > 0) {
                name.remove(slash);
            }
            return name;
        }
    }
    return String();
}

bool expired(time_t fetchedAt)
{
    if (fetchedAt == 0) {
        return true;  // 取得した日時が分からないものは、期限を判断できないので残さない
    }
    if (!net::timeValid()) {
        return false;  // 今の時刻が分からないあいだは判断を保留する
    }
    return time(nullptr) - fetchedAt > kKeepSeconds;
}

void removeFile()
{
    if (!board::sdReady()) {
        return;
    }
    // 本ファームウェアが自分で作るファイルだけを消す。
    for (const char *path : {kPath, kTempPath}) {
        if (SD.exists(path)) {
            SD.remove(path);
        }
    }
}

// SafeWriter を ArduinoJson の出力先にする。
struct WriterAdapter {
    SafeWriter &writer;
    size_t write(uint8_t c) { return writer.write(&c, 1) ? 1 : 0; }
    size_t write(const uint8_t *data, size_t length) { return writer.write(data, length) ? length : 0; }
};

bool save(const char *shape)
{
    if (!board::sdReady()) {
        return false;
    }
    if (!SD.exists(kDir)) {
        SD.mkdir(kDir);
    }
    JsonDocument doc(&g_allocator);
    doc["v"] = 1;
    doc["dest"]["lat"] = g_info.destLat;
    doc["dest"]["lon"] = g_info.destLon;
    doc["dest"]["name"] = g_info.destName;
    doc["fetched"] = static_cast<int64_t>(g_info.fetchedAt);
    doc["length"] = g_info.lengthM;
    doc["time"] = g_info.timeSec;
    doc["shape"] = shape;
    JsonArray list = doc["maneuvers"].to<JsonArray>();
    for (const Maneuver &m : g_maneuvers) {
        JsonObject o = list.add<JsonObject>();
        o["t"] = m.type;
        o["i"] = m.index;
        if (m.name.length()) {
            o["n"] = m.name;
        }
    }

    // 書きかけのファイルが残らないよう、一時ファイルに書いてから改名する。
    SafeWriter writer;
    if (!writer.begin(kTempPath)) {
        return false;
    }
    WriterAdapter adapter{writer};
    serializeJson(doc, adapter);
    if (!writer.finish()) {
        writer.abort();
        SD.remove(kTempPath);
        return false;
    }
    if (SD.exists(kPath)) {
        SD.remove(kPath);
    }
    return SD.rename(kTempPath, kPath);
}

// 受信した HTTP の本文を、まとめて読んで ArduinoJson に渡す。
uint8_t g_readBuffer[2048];
struct HttpReader {
    explicit HttpReader(WiFiClient *client) : stream(client) {}

    WiFiClient *stream;
    size_t length = 0;
    size_t position = 0;
    size_t total = 0;
    uint32_t lastDataMs = millis();

    bool fill()
    {
        while (total < kMaxResponseBytes && millis() - lastDataMs < 15000) {
            const int available = stream->available();
            if (available > 0) {
                const int n = stream->read(
                    g_readBuffer, std::min(static_cast<size_t>(available), sizeof(g_readBuffer)));
                if (n > 0) {
                    length = n;
                    position = 0;
                    total += n;
                    lastDataMs = millis();
                    return true;
                }
            } else if (!stream->connected()) {
                return false;  // 相手が送り終えて接続を閉じた
            }
            delay(1);
        }
        return false;
    }

    int read()
    {
        if (position >= length && !fill()) {
            return -1;
        }
        return g_readBuffer[position++];
    }

    size_t readBytes(char *out, size_t count)
    {
        size_t done = 0;
        while (done < count) {
            if (position >= length && !fill()) {
                break;
            }
            const size_t take = std::min(count - done, length - position);
            memcpy(out + done, g_readBuffer + position, take);
            position += take;
            done += take;
        }
        return done;
    }
};

String httpErrorText(int code, const String &detail)
{
    if (code < 0) {
        return "経路サービスに接続できません";
    }
    if (code == 401 || code == 403) {
        return "API キーが正しくありません";
    }
    if (code == 429) {
        return "利用回数の上限に達しました";
    }
    if (detail.indexOf("\"error_code\":154") >= 0) {
        return "目的地が遠すぎます";
    }
    if (code == 400) {
        return "経路が見つかりませんでした";
    }
    return "経路サービスのエラー (" + String(code) + ")";
}

}  // namespace

Point toPoint(double lat, double lon)
{
    const double x = (lon / 360.0 + 0.5) * kWorld;
    const double s = log(tan((45.0 + lat / 2.0) * M_PI / 180.0));
    const double y = (0.5 - s / (2.0 * M_PI)) * kWorld;
    Point p;
    p.x = static_cast<uint32_t>(std::min(std::max(x, 0.0), kWorld - 1.0));
    p.y = static_cast<uint32_t>(std::min(std::max(y, 0.0), kWorld - 1.0));
    return p;
}

void toLatLon(const Point &p, double &lat, double &lon)
{
    lon = p.x / kWorld * 360.0 - 180.0;
    lat = atan(sinh(M_PI * (1.0 - 2.0 * p.y / kWorld))) * 180.0 / M_PI;
}

void load()
{
    if (g_loaded || !board::sdReady()) {
        return;
    }
    g_loaded = true;
    if (!SD.exists(kPath)) {
        return;
    }

    JsonDocument doc(&g_allocator);
    File file = SD.open(kPath);
    const DeserializationError err = deserializeJson(doc, file);
    file.close();
    const char *shape = doc["shape"] | static_cast<const char *>(nullptr);
    const time_t fetchedAt = static_cast<time_t>(doc["fetched"] | static_cast<int64_t>(0));
    if (err || shape == nullptr || expired(fetchedAt)) {
        Serial.printf("[route] saved route dropped (%s)\n", err ? err.c_str() : "expired");
        removeFile();
        return;
    }

    std::vector<Point> points;
    std::vector<float> along;
    if (!decodeShape(shape, points, along)) {
        removeFile();
        return;
    }
    g_info.destLat = doc["dest"]["lat"] | 0.0;
    g_info.destLon = doc["dest"]["lon"] | 0.0;
    g_info.destName = doc["dest"]["name"] | "";
    g_info.fetchedAt = fetchedAt;
    g_info.lengthM = doc["length"] | along.back();
    g_info.timeSec = doc["time"] | 0.0f;
    g_maneuvers.clear();
    for (JsonObjectConst o : doc["maneuvers"].as<JsonArrayConst>()) {
        Maneuver m;
        m.type = o["t"] | 0;
        m.index = o["i"] | 0u;
        m.name = o["n"] | "";
        if (m.index < points.size()) {
            g_maneuvers.push_back(m);
        }
    }
    g_points.swap(points);
    g_along.swap(along);
    g_lastSegment = 0;
    g_lastAlong = 0;
    g_active = true;
    Serial.printf("[route] loaded %u points, %.1f km\n", static_cast<unsigned>(g_points.size()),
                  g_info.lengthM / 1000.0f);
}

bool active()
{
    return g_active;
}

const Info &info()
{
    return g_info;
}

const std::vector<Point> &points()
{
    return g_points;
}

const std::vector<Maneuver> &maneuvers()
{
    return g_maneuvers;
}

bool fetch(double fromLat, double fromLon, double toLat, double toLon, const String &destName,
           String &error)
{
    const String key = settings::routeKey();
    if (key.length() == 0) {
        error = "API キーが未設定です";
        return false;
    }
    if (!net::connected()) {
        error = "WiFi に接続されていません";
        return false;
    }

    // 案内の文は受け取らず（directions_type = maneuvers）、種別と道路名から本体で作る。
    char body[420];
    snprintf(body, sizeof(body),
             "{\"locations\":[{\"lat\":%.6f,\"lon\":%.6f},{\"lat\":%.6f,\"lon\":%.6f}],"
             "\"costing\":\"motor_scooter\","
             "\"costing_options\":{\"motor_scooter\":{\"top_speed\":%d,\"use_primary\":%.2f}},"
             "\"directions_options\":{\"units\":\"kilometers\"},"
             "\"directions_type\":\"maneuvers\"}",
             fromLat, fromLon, toLat, toLon, kTopSpeedKmh, kUsePrimary);

    HTTPClient http;
    http.useHTTP10(true);  // 本文を区切りなしで受け取り、そのまま読めるようにする
    http.setConnectTimeout(10000);
    http.setTimeout(20000);
    if (!http.begin(String(kServiceUrl) + key, kRootCa)) {
        error = "経路サービスに接続できません";
        return false;
    }
    http.addHeader("Content-Type", "application/json");
    const int code = http.POST(reinterpret_cast<uint8_t *>(body), strlen(body));
    if (code != HTTP_CODE_OK) {
        const String detail = code > 0 ? http.getString() : String();
        http.end();
        Serial.printf("[route] POST -> %d %s\n", code, detail.substring(0, 200).c_str());
        error = httpErrorText(code, detail);
        return false;
    }

    JsonDocument filter;
    filter["trip"]["summary"]["length"] = true;
    filter["trip"]["summary"]["time"] = true;
    filter["trip"]["legs"][0]["shape"] = true;
    filter["trip"]["legs"][0]["maneuvers"][0]["type"] = true;
    filter["trip"]["legs"][0]["maneuvers"][0]["begin_shape_index"] = true;
    filter["trip"]["legs"][0]["maneuvers"][0]["street_names"] = true;

    JsonDocument doc(&g_allocator);
    HttpReader reader(http.getStreamPtr());
    const DeserializationError err =
        deserializeJson(doc, reader, DeserializationOption::Filter(filter));
    http.end();

    JsonObjectConst leg = doc["trip"]["legs"][0];
    const char *shape = leg["shape"] | static_cast<const char *>(nullptr);
    if (err || shape == nullptr) {
        Serial.printf("[route] parse failed: %s\n", err.c_str());
        error = "経路を読み取れませんでした";
        return false;
    }

    std::vector<Point> points;
    std::vector<float> along;
    if (!decodeShape(shape, points, along)) {
        error = "経路を読み取れませんでした";
        return false;
    }

    g_maneuvers.clear();
    for (JsonObjectConst o : leg["maneuvers"].as<JsonArrayConst>()) {
        Maneuver m;
        m.type = o["type"] | 0;
        m.index = o["begin_shape_index"] | 0u;
        m.name = pickName(o["street_names"].as<JsonArrayConst>());
        if (m.index < points.size()) {
            g_maneuvers.push_back(m);
        }
    }
    g_info.destLat = toLat;
    g_info.destLon = toLon;
    g_info.destName = destName;
    g_info.fetchedAt = net::timeValid() ? time(nullptr) : 0;
    g_info.lengthM = (doc["trip"]["summary"]["length"] | 0.0f) * 1000.0f;
    g_info.timeSec = doc["trip"]["summary"]["time"] | 0.0f;
    if (g_info.lengthM <= 0) {
        g_info.lengthM = along.back();
    }
    g_points.swap(points);
    g_along.swap(along);
    g_lastSegment = 0;
    g_lastAlong = 0;
    g_active = true;
    g_loaded = true;

    // SD カードに保存できなくても、起きている間は案内できる。
    if (!save(shape)) {
        Serial.println("[route] could not save the route to the SD card");
    }
    Serial.printf("[route] fetched %u points, %.1f km, %.0f min\n",
                  static_cast<unsigned>(g_points.size()), g_info.lengthM / 1000.0f,
                  g_info.timeSec / 60.0f);
    return true;
}

void clear()
{
    g_active = false;
    std::vector<Point>().swap(g_points);
    std::vector<float>().swap(g_along);
    g_maneuvers.clear();
    g_info = Info();
    removeFile();
}

Progress locate(double lat, double lon)
{
    Progress progress;
    const size_t count = g_points.size();
    if (!g_active || count < 2) {
        return progress;
    }

    // 現在地を原点にして、各区間までの距離を m で比べる（単精度で足りる）。
    const Point here = toPoint(lat, lon);
    const float scale =
        kEarthCircumferenceM * cosf(static_cast<float>(lat * M_PI / 180.0)) / static_cast<float>(kWorld);
    float bestD2 = 1e30f;
    float bestT = 0;
    size_t best = 0;
    auto search = [&](size_t from, size_t to) {
        for (size_t i = from; i < to; ++i) {
            const float ax = static_cast<int32_t>(g_points[i].x - here.x) * scale;
            const float ay = static_cast<int32_t>(g_points[i].y - here.y) * scale;
            const float dx = static_cast<int32_t>(g_points[i + 1].x - g_points[i].x) * scale;
            const float dy = static_cast<int32_t>(g_points[i + 1].y - g_points[i].y) * scale;
            const float len2 = dx * dx + dy * dy;
            float t = len2 > 0 ? -(ax * dx + ay * dy) / len2 : 0;
            t = std::min(std::max(t, 0.0f), 1.0f);
            const float cx = ax + dx * t;
            const float cy = ay + dy * t;
            const float d2 = cx * cx + cy * cy;
            if (d2 < bestD2) {
                bestD2 = d2;
                bestT = t;
                best = i;
            }
        }
    };
    // まず前回の位置の近くを探す。往復する経路で、反対向きの区間に飛びつかないようにするため。
    const size_t last = std::min(g_lastSegment, count - 2);
    search(last > 20 ? last - 20 : 0, std::min(last + 200, count - 1));
    if (bestD2 > 40.0f * 40.0f) {
        search(0, count - 1);
    }
    g_lastSegment = best;

    const float along = g_along[best] + (g_along[best + 1] - g_along[best]) * bestT;
    const float total = g_along.back();
    g_lastAlong = along;
    progress.valid = true;
    progress.offRouteM = sqrtf(bestD2);
    progress.remainingM = std::max(0.0f, total - along);
    progress.remainingSec = total > 0 ? g_info.timeSec * progress.remainingM / total : 0;
    for (size_t i = 0; i < g_maneuvers.size(); ++i) {
        const Maneuver &m = g_maneuvers[i];
        if (m.index > best && maneuverLabel(m.type)[0] != '\0') {
            progress.nextManeuver = static_cast<int>(i);
            progress.toNextM = std::max(0.0f, g_along[m.index] - along);
            break;
        }
    }
    return progress;
}

bool pointAhead(float meters, double &lat, double &lon)
{
    const size_t count = g_points.size();
    if (!g_active || count < 2) {
        return false;
    }
    const float target = std::min(g_lastAlong + meters, g_along.back());
    size_t i = std::min(g_lastSegment, count - 2);
    while (i + 2 < count && g_along[i + 1] < target) {
        ++i;
    }
    const float span = g_along[i + 1] - g_along[i];
    const float t = span > 0 ? std::min(std::max((target - g_along[i]) / span, 0.0f), 1.0f) : 0.0f;
    // 2 点の間を、座標の差（符号付き）で補間する。
    Point p;
    p.x = g_points[i].x + static_cast<int32_t>(static_cast<int32_t>(g_points[i + 1].x - g_points[i].x) * t);
    p.y = g_points[i].y + static_cast<int32_t>(static_cast<int32_t>(g_points[i + 1].y - g_points[i].y) * t);
    toLatLon(p, lat, lon);
    return true;
}

const char *maneuverLabel(uint8_t type)
{
    // 種別番号は Valhalla のもの。出発、道なり、道路名が変わるだけの案内は出さない。
    switch (type) {
        case 4:
        case 5:
        case 6:
            return "目的地";
        case 9:
            return "斜め右";
        case 10:
            return "右折";
        case 11:
            return "鋭く右折";
        case 12:
        case 13:
            return "U ターン";
        case 14:
            return "鋭く左折";
        case 15:
            return "左折";
        case 16:
            return "斜め左";
        case 18:
        case 20:
        case 23:
            return "右へ";
        case 19:
        case 21:
        case 24:
            return "左へ";
        case 26:
        case 27:
            return "環状交差点";
        case 28:
            return "フェリー";
        default:
            return "";
    }
}

}  // namespace route
