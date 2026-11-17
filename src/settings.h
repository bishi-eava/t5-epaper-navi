// 本体フラッシュ (NVS) に保存する設定値。
#pragma once

#include <Arduino.h>

namespace settings {

void load();

// フロントライトの明るさ（0〜100%）
uint8_t frontlight();
void setFrontlight(uint8_t percent);

// スリープまでの無操作時間（分）
uint16_t sleepMinutes();
void setSleepMinutes(uint16_t minutes);

// ロック画面に表示する画像のパス（SD カード上）。空なら既定の画像。
String lockImage();
void setLockImage(const String &path);

// 画像ビューワーで最後に見ていたフォルダ（SD カード上）。空なら未設定。
String imageDir();
void setImageDir(const String &dir);

// WiFi 転送のアクセスポイントの名前とパスワード。空なら未設定（既定値を使う）。
// 名前は 1〜32 文字、パスワードは 8〜63 文字（WPA2 の決まり）。範囲外は保存せず false。
String transferSsid();
bool setTransferSsid(const String &ssid);
String transferPassword();
bool setTransferPassword(const String &password);
// 名前とパスワードを未設定（既定値）に戻す。
void resetTransfer();

// 電子ペーパーの VCOM 電圧（mV）
int vcom();
void setVcom(int millivolts);

// マップで最後に見ていた場所
struct MapView {
    double lat = 35.689440;  // 既定は東京（m5-navi と同じ）
    double lon = 139.691670;
    uint8_t zoom = 12;
};
MapView mapView();
void setMapView(const MapView &view);

}  // namespace settings
