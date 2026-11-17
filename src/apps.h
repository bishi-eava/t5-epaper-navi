// 各アプリの入口。
#pragma once

#include <Arduino.h>

#include "ui.h"

#define FIRMWARE_VERSION "0.1.0"

namespace apps {

ui::App &home();
ui::App &map();
ui::App &calendar();
ui::App &settings();
ui::App &filer();
ui::App &viewer();

// 指定した画像を画像ビューワーで開く。
void openViewer(const String &path);

// 拡張子が対応画像（JPEG / PNG / BMP）か。
bool isImagePath(const String &path);

// SD カード上の画像を、指定した枠に収まるよう縮小して中央に描く。
bool drawImageFit(const String &path, int x, int y, int w, int h);

// マップに目的地の候補を渡す（スマホからの受信や、調査用コンソールから使う）。
// マップを開いているときに、本体の画面で確認してから目的地にする。
void offerDestination(double lat, double lon, const String &name);

// ロック画面（画像のみ）を表示する。反映まで行う。
void showLockScreen();

}  // namespace apps
