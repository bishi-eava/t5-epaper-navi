// シリアル経由の調査用コンソール。画面を見られない環境から動作を確かめるために使う。
//
// コマンド（1 行ずつ送る）:
//   shot            画面の内容を 16 進文字列で出力する（1 行 = 画面 1 行、1 文字 = 1 画素の 16 階調）
//   tap X Y         タップを送る
//   drag X Y DX DY  ドラッグを送る
//   home            ホームキーを送る
//   info            SD カード、WiFi、GPS、メモリの状態と、直前のスリープで減った電池の量を出力する
//   charger         充電 IC (BQ25896) のレジスタをすべて出力する
//   gps LAT LON     測位した位置を置き換える（屋内で案内の表示を確かめる用）。gps off で元に戻す
//   routekey KEY    ルート案内で使う経路サービスの API キーを保存する
//   dest LAT LON    マップに目的地の候補を渡す（スマホからの受信と同じ扱い。マップを開いて使う）
//   ls PATH         SD カード上のフォルダの中身を出力する（最大 200 件）
//   web 1|0         WiFi を切り替えずに転送サーバーだけを動かす / 止める（同じ LAN から確認する用）
//   head PATH       SD カード上のファイルの先頭 32 バイトを 16 進で出力する
#pragma once

#include "board.h"

namespace debug {

// 受信したコマンドを処理する。タッチ操作のコマンドなら ev に、ホームキーなら home に結果を入れる。
void poll(board::TouchEvent &ev, bool &home);

}  // namespace debug
