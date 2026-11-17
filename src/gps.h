// 内蔵 GPS。電源は本体が起きている間ずっと入っている（board::setRadioPower を参照）。
// ここでは受信（UART）の開始と停止だけを行う。
// 搭載モジュールは MIA-M10Q か L76K のどちらかで、既定のボーレートが違うため自動で判別する。
#pragma once

#include <Arduino.h>

namespace gps {

// 電源を入れて受信を始める / 止めて電源を切る。
void start();
void stop();
bool running();

// 毎ループ呼ぶ。受信データの解析、ボーレートの判別、時刻合わせを行う。
void tick();

// モジュールからの受信を確認できたか。
bool detected();
// 判別したモジュール名。未判別なら空文字列。
const char *modelName();

// 測位できているか（直近 10 秒以内に位置を受信）。
bool hasFix();
double latitude();
double longitude();
// 水平精度低下率。小さいほど高精度。不明なら 99。
float hdop();
int satellites();

// 調査用: 測位した位置を、指定した緯度経度に置き換える（屋内で案内の表示を確かめるため）。
// 起動し直すか、stopSimulation() を呼ぶまで続く。
void simulate(double lat, double lon);
void stopSimulation();

}  // namespace gps
