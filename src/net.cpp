#include "net.h"

#include <Preferences.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <sys/time.h>

#include "board.h"

namespace net {
namespace {

constexpr char kNamespace[] = "wifi";
constexpr size_t kMaxSaved = 8;
constexpr time_t kMinValidTime = 1704067200;  // 2024-01-01 UTC

constexpr uint32_t kAutoRetryMs = 20000;     // 自動接続をやり直す間隔
constexpr uint32_t kScanRetryMs = 2000;      // スキャン自体が失敗したときにやり直す間隔
constexpr uint32_t kAutoConnectWaitMs = 15000;  // 接続開始から結果を待つ時間

std::vector<Network> g_saved;
bool g_autoScanRunning = false;
bool g_suspended = false;
uint32_t g_autoNextTryMs = 0;   // 次に自動接続を試みる時刻
uint32_t g_autoBusyUntilMs = 0; // 接続の結果待ちの期限
bool g_sntpStarted = false;
bool g_rtcUpdatePending = false;

void loadSaved()
{
    g_saved.clear();
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) {
        return;
    }
    const size_t count = prefs.getUChar("count", 0);
    for (size_t i = 0; i < count && i < kMaxSaved; ++i) {
        Network n;
        n.ssid = prefs.getString(("s" + String(i)).c_str(), "");
        n.password = prefs.getString(("p" + String(i)).c_str(), "");
        if (n.ssid.length() > 0) {
            g_saved.push_back(n);
        }
    }
    prefs.end();
}

void storeSaved()
{
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
        return;
    }
    prefs.clear();
    prefs.putUChar("count", static_cast<uint8_t>(g_saved.size()));
    for (size_t i = 0; i < g_saved.size(); ++i) {
        prefs.putString(("s" + String(i)).c_str(), g_saved[i].ssid);
        prefs.putString(("p" + String(i)).c_str(), g_saved[i].password);
    }
    prefs.end();
}

// 直近に接続したものが先頭に来るように保存する。
void remember(const String &ssid, const String &password)
{
    for (size_t i = 0; i < g_saved.size(); ++i) {
        if (g_saved[i].ssid == ssid) {
            g_saved.erase(g_saved.begin() + i);
            break;
        }
    }
    g_saved.insert(g_saved.begin(), Network{ssid, password});
    if (g_saved.size() > kMaxSaved) {
        g_saved.resize(kMaxSaved);
    }
    storeSaved();
}

void startSntp()
{
    configTzTime("JST-9", "ntp.nict.jp", "pool.ntp.org");
    g_sntpStarted = true;
    g_rtcUpdatePending = true;
}

}  // namespace

void begin()
{
    setenv("TZ", "JST-9", 1);
    tzset();

    const time_t rtc = board::rtcRead();
    if (rtc >= kMinValidTime) {
        const timeval tv = {rtc, 0};
        settimeofday(&tv, nullptr);
    }

    loadSaved();
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    Serial.printf("[net] saved networks: %u\n", static_cast<unsigned>(g_saved.size()));
    g_autoNextTryMs = millis();
}

void suspend()
{
    g_suspended = true;
    g_autoScanRunning = false;
    WiFi.scanDelete();
    WiFi.disconnect(true);
}

void resume()
{
    g_suspended = false;
    WiFi.mode(WIFI_STA);
    g_autoBusyUntilMs = 0;
    g_autoNextTryMs = millis() + 500;  // すぐ自動接続をやり直す
}

void tick()
{
    if (g_suspended) {
        return;
    }
    // 自動接続: 未接続で保存済みネットワークがあれば、周囲をスキャンして
    // 電波の強い保存済みネットワークへ接続する。失敗したら一定間隔でやり直す。
    const uint32_t now = millis();
    if (g_autoScanRunning) {
        const int16_t n = WiFi.scanComplete();
        if (n != WIFI_SCAN_RUNNING) {
            g_autoScanRunning = false;
            int best = -1;
            for (int i = 0; i < n; ++i) {
                if (isSaved(WiFi.SSID(i)) && (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best))) {
                    best = i;
                }
            }
            Serial.printf("[net] auto scan: result=%d best=%d\n", n, best);
            if (best >= 0 && WiFi.status() != WL_CONNECTED) {
                const String ssid = WiFi.SSID(best);
                const String password = savedPassword(ssid);
                WiFi.begin(ssid.c_str(), password.length() ? password.c_str() : nullptr);
                g_autoBusyUntilMs = now + kAutoConnectWaitMs;
            }
            WiFi.scanDelete();
            // 起動直後のスキャンは失敗することがある（実機で確認）。その場合はすぐやり直す。
            g_autoNextTryMs = now + (n < 0 ? kScanRetryMs : kAutoRetryMs);
        }
    } else if (!g_saved.empty() && WiFi.status() != WL_CONNECTED &&
               static_cast<int32_t>(now - g_autoNextTryMs) >= 0 &&
               static_cast<int32_t>(now - g_autoBusyUntilMs) >= 0) {
        const int16_t started = WiFi.scanNetworks(true);
        g_autoScanRunning = started == WIFI_SCAN_RUNNING;
        Serial.printf("[net] auto scan start: %d\n", started);
        g_autoNextTryMs = now + (g_autoScanRunning ? kAutoRetryMs : kScanRetryMs);
    }

    if (WiFi.status() == WL_CONNECTED && !g_sntpStarted) {
        Serial.printf("[net] connected: %s\n", WiFi.SSID().c_str());
        startSntp();
    }

    if (g_rtcUpdatePending && sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        g_rtcUpdatePending = false;
        const bool ok = board::rtcWrite(time(nullptr));
        Serial.printf("[net] time synced, rtc write %s\n", ok ? "ok" : "failed");
    }
}

bool connected() { return !g_suspended && WiFi.status() == WL_CONNECTED; }

String currentSsid() { return connected() ? WiFi.SSID() : String(); }

bool timeValid() { return time(nullptr) >= kMinValidTime; }

bool syncTime()
{
    if (!connected()) {
        return false;
    }
    startSntp();
    return true;
}

const std::vector<Network> &saved() { return g_saved; }

bool isSaved(const String &ssid)
{
    for (const auto &n : g_saved) {
        if (n.ssid == ssid) {
            return true;
        }
    }
    return false;
}

String savedPassword(const String &ssid)
{
    for (const auto &n : g_saved) {
        if (n.ssid == ssid) {
            return n.password;
        }
    }
    return String();
}

void forget(const String &ssid)
{
    for (size_t i = 0; i < g_saved.size(); ++i) {
        if (g_saved[i].ssid == ssid) {
            g_saved.erase(g_saved.begin() + i);
            storeSaved();
            break;
        }
    }
    if (WiFi.SSID() == ssid) {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_STA);
    }
}

bool connect(const String &ssid, const String &password, uint32_t timeoutMs)
{
    g_autoScanRunning = false;
    WiFi.scanDelete();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    delay(100);
    WiFi.begin(ssid.c_str(), password.length() ? password.c_str() : nullptr);

    const uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        const wl_status_t status = WiFi.status();
        if (status == WL_CONNECTED) {
            remember(ssid, password);
            g_sntpStarted = false;  // 次の tick で時刻同期する
            return true;
        }
        if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
            break;
        }
        delay(100);
    }
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    return false;
}

}  // namespace net
