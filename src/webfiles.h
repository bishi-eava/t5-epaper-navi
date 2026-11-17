// PC やスマホのブラウザから SD カードのファイルを読み書きするための WiFi 転送機能。
//
// 転送中は本体が WiFi のアクセスポイント（親機）になる。PC やスマホをそのアクセスポイントに
// つなぎ、ブラウザで url() を開いて使う。アクセスポイントへの接続にパスワード (WPA2) が要り、
// ブラウザ側の認証はない。本体で転送画面を開いている間だけ動かす。
// アクセスポイントの名前とパスワードは、設定アプリの「WiFi 転送」で変えられる。
// 仕様は m5-wifi-fileserver と esp-mp4player のファイルブラウザに合わせている。
#pragma once

#include <Arduino.h>

namespace webfiles {

// アクセスポイントとサーバーを始める。SD カードがなければ false。
// 動作中は、ふだんの WiFi 接続（ルーターへの接続）は切れる。
// accessPoint を false にすると、WiFi を切り替えずにサーバーだけを動かす（動作確認用）。
bool start(bool accessPoint = true);
// サーバーとアクセスポイントを止め、ふだんの WiFi 接続に戻す。
void stop();
bool running();

// 毎ループ呼ぶ。ブラウザからの要求を処理する。
void tick();

// 転送画面に表示する接続情報。
String ssid();        // アクセスポイントの名前。設定がなければ既定値
String password();    // アクセスポイントのパスワード。設定がなければ既定値
String defaultSsid();      // 既定の名前（t5-epaper）
String defaultPassword();  // 既定のパスワード（12345678）。秘密ではない
String url();         // ブラウザで開くアドレス
String wifiQrText();  // スマホのカメラで読むと WiFi につながる QR コードの中身

// つながっている端末の数と、これまでに処理した要求の数（転送画面の表示用）。
int clientCount();
uint32_t requestCount();

}  // namespace webfiles
