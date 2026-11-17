// ボード層: LilyGO T5 E-Paper S3 Pro (H752-01) のハードウェア制御。
// ピン配置は公式リポジトリ T5S3-4.7-e-paper-PRO の docs/pinmap.md による。
#pragma once

#include <Arduino.h>
#include <SD.h>  // M5GFX より先に読み込むと、SD からの画像描画が有効になる
#include <M5GFX.h>
#include <time.h>

namespace board {

using lgfx::epd_mode_t;

// 縦向き 540x960。rotation 0 がタッチパネルの座標系と一致する。
extern lgfx::LGFX_Device &display;

// 画面と I2C デバイスを初期化する。画面の初期化に失敗したら false。
bool begin();

// これから描く内容の更新モードを選ぶ。描画の前に呼ぶこと。
// 高速モード (epd_fast) では描画時点で白黒 2 値に変換されるため、後から変えても階調は戻らない。
void setDrawMode(epd_mode_t mode);

// 描画済みの内容を、選択中の更新モードで電子ペーパーに反映する。反映後は画面電源を切る。
void present();

// フロントライトの明るさ（0〜100%）。
void setFrontlight(uint8_t percent);

// 電子ペーパーの VCOM 電圧（mV、負電圧の大きさ）。パネルごとに最適値が違い、
// 合っていないとコントラストが落ちる。次の画面更新から反映される。
constexpr int kVcomDefaultMv = 1560;
constexpr int kVcomMinMv = 500;
constexpr int kVcomMaxMv = 3000;
void setVcom(int millivolts);
// 直近の画面更新で、電源 IC から読み戻した VCOM 設定値（mV）。読めていなければ -1。
int vcomReadback();

struct Battery {
    bool valid = false;     // 残量計から読めたか
    int percent = 0;        // 残量 %
    int millivolts = 0;     // 電圧 mV
    int milliamps = 0;      // 電流 mA（正が充電、負が放電）
    bool charging = false;  // 充電中
    bool usbPower = false;  // USB 給電あり
};
Battery readBattery();

// RTC (PCF8563)。時刻は UTC で保持する。未設定・読み取り失敗時は 0 を返す。
time_t rtcRead();
bool rtcWrite(time_t utc);

// 西暦の日時 (UTC) を 1970-01-01 起点の秒数に変換する。
time_t utcFromCivil(int year, int month, int day, int hour, int minute, int second);

// GPS と LoRa の共用電源。
//
// 起きている間は常に入れておく（begin() で入れ、deepSleep() で切る）。LoRa チップは SD カードと
// SPI バスを共有していて、電源を切ったままだと通信中の SD カードの読み書きが失敗するため
// （実機で確認: 電源オフではアップロード中に書き込みエラーが頻発し、オンでは 0 件）。
// LoRa チップ自体は使わないので、電源を入れた直後にスリープさせる。
void setRadioPower(bool on);

// GPS の UART ピン
constexpr int kPinGpsRx = 44;  // GPS の TX を受ける
constexpr int kPinGpsTx = 43;  // GPS の RX へ送る

// SD カード。未マウントなら必要に応じてマウントを試みる。
bool sdReady();

// 調査用: SD カードの通信速度を変える（次のマウントから有効）。
void sdSetFrequency(uint32_t hz);

// 調査用: SD カードのマウントを count 回やり直し、成功した回数を返す。
int sdMountTest(int count, uint32_t frequency);

struct TouchEvent {
    enum Type { None, Tap, Drag } type = None;
    int x = 0;   // Tap: 位置 / Drag: 開始位置
    int y = 0;
    int dx = 0;  // Drag: 移動量
    int dy = 0;
    bool touching = false;  // 指が触れている最中か
};
// 指を離した時点で Tap か Drag を 1 回返す。
TouchEvent pollTouch();

// 画面下部のホームキーが押されたか（読むとクリアされる）。
bool takeHomePressed();

// BOOT ボタンが押されているか。
bool bootPressed();

// 周辺回路をすべて止めてディープスリープに入る。BOOT ボタンで復帰（再起動）する。
[[noreturn]] void deepSleep();

}  // namespace board
