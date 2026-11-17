// WiFi 接続と時刻同期。接続情報は本体フラッシュ (NVS) に保存する。
#pragma once

#include <Arduino.h>
#include <vector>

namespace net {

struct Network {
    String ssid;
    String password;
};

// 保存済みネットワークを読み込み、RTC から時刻を復元し、自動接続を始める。
void begin();

// 毎ループ呼ぶ。自動接続の進行と、NTP 同期後の RTC 更新を行う。
void tick();

// ふだんの WiFi 接続（ルーターへの接続）を止める / 再開する。
// WiFi 転送で本体をアクセスポイントにするあいだ使う。
void suspend();
void resume();

bool connected();
String currentSsid();

// 時刻が設定済みか（RTC か NTP から取得できているか）。
bool timeValid();
// NTP での時刻合わせをやり直す。WiFi 未接続なら false。
bool syncTime();

const std::vector<Network> &saved();
bool isSaved(const String &ssid);
String savedPassword(const String &ssid);
void forget(const String &ssid);

// 接続を試み、成功したら保存する。完了まで（最大 timeoutMs）ブロックする。
bool connect(const String &ssid, const String &password, uint32_t timeoutMs = 15000);

}  // namespace net
