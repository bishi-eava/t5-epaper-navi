// ルート案内の経路: 経路サービスからの取得、SD カードへの保存、現在地との照合（SPEC.md 4.9）。
//
// 経路は本体では計算せず、経路サービス（Stadia Maps）に問い合わせて受け取る。
// 経路サービスとのやり取りは route.cpp の fetch() だけにまとめてあり、ほかのサービスに
// 差し替えるときはそこを書き換える。
#pragma once

#include <Arduino.h>
#include <vector>

namespace route {

// 経路上の 1 点。メルカトル図法の世界全体を 2^32 x 2^32 に割った座標で持つ
// （地図のズーム z での画素は、右に (24 - z) ビットずらすと得られる）。
struct Point {
    uint32_t x;
    uint32_t y;
};

// 曲がり角などの案内 1 件。
struct Maneuver {
    uint8_t type = 0;    // 経路サービス（Valhalla）の種別番号
    uint32_t index = 0;  // この案内の地点が、経路の何番目の点か
    String name;         // 進む先の道路名（分からなければ空）
};

struct Info {
    double destLat = 0;
    double destLon = 0;
    String destName;
    time_t fetchedAt = 0;  // 取得した日時。時計が合っていなかったときは 0
    float lengthM = 0;     // 経路の長さ
    float timeSec = 0;     // 所要時間（経路サービスの見積もり）
};

// 現在地を経路に当てはめた結果。
struct Progress {
    bool valid = false;
    float offRouteM = 0;   // 経路からの距離
    float remainingM = 0;  // 目的地までの残り距離
    float remainingSec = 0;
    int nextManeuver = -1; // 次の案内（maneuvers() の番号）。なければ -1
    float toNextM = 0;     // 次の案内の地点までの距離
};

// SD カードに保存してある経路を読み込む。取得から 7 日を過ぎていたら削除する。
// すでに読み込んであれば何もしない。
void load();

bool active();
const Info &info();
const std::vector<Point> &points();
const std::vector<Maneuver> &maneuvers();

// 経路サービスから経路を取得し、SD カードに保存する。完了までブロックする。
// 失敗したら false を返し、error に画面へ出す文を入れる（それまでの経路は残る）。
bool fetch(double fromLat, double fromLon, double toLat, double toLon, const String &destName,
           String &error);

// 案内をやめる。保存してある経路も削除する。
void clear();

// 現在地を経路に当てはめる。
Progress locate(double lat, double lon);

// 最後に locate() で当てはめた位置から、経路に沿って meters だけ先の地点を返す。
// 経路がないときは false。目的地より先は目的地を返す。
bool pointAhead(float meters, double &lat, double &lon);

// 案内の種別を画面に出す言葉にする（例: 「右折」）。案内として出さない種別は空文字列。
const char *maneuverLabel(uint8_t type);

// 緯度経度と Point の変換。
Point toPoint(double lat, double lon);
void toLatLon(const Point &p, double &lat, double &lon);

}  // namespace route
