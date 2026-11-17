#include "gps.h"

#include <TinyGPS++.h>
#include <sys/time.h>

#include "board.h"

namespace gps {
namespace {

// 既定のボーレート: L76K = 9600, MIA-M10Q = 38400（公式リポジトリ docs/pinmap.md）
struct Candidate {
    uint32_t baud;
    const char *model;
};
const Candidate kCandidates[] = {{9600, "L76K"}, {38400, "MIA-M10Q"}};
constexpr int kCandidateCount = sizeof(kCandidates) / sizeof(kCandidates[0]);

constexpr uint32_t kProbeMs = 2500;        // 1 つのボーレートで受信を待つ時間
constexpr uint32_t kFixTimeoutMs = 10000;  // これより古い位置は測位なしとみなす

TinyGPSPlus g_gps;
bool g_running = false;
bool g_detected = false;
int g_candidate = 0;  // 判別済みなら、次回もそのボーレートから始める
uint32_t g_probeStartMs = 0;
uint32_t g_probeBaseline = 0;  // 判別開始時点の、チェックサム一致文の数
bool g_timeChecked = false;
const char *g_lastModel = "";  // 判別結果。電源を切っても覚えておく（本体情報の表示用）

void openPort()
{
    Serial1.end();
    Serial1.setRxBufferSize(4096);  // 画面更新中は読み出せないので大きめに取る
    Serial1.begin(kCandidates[g_candidate].baud, SERIAL_8N1, board::kPinGpsRx, board::kPinGpsTx);
    g_probeStartMs = millis();
    g_probeBaseline = g_gps.passedChecksum();
}

// GPS の時刻で本体の時計と RTC を合わせる（受信開始ごとに 1 回）。
void syncTimeOnce()
{
    if (g_timeChecked || !g_gps.date.isValid() || !g_gps.time.isValid() ||
        g_gps.date.year() < 2024 || g_gps.time.age() > 1500 || !g_gps.location.isValid()) {
        return;
    }
    g_timeChecked = true;
    const time_t utc = board::utcFromCivil(g_gps.date.year(), g_gps.date.month(), g_gps.date.day(),
                                           g_gps.time.hour(), g_gps.time.minute(),
                                           g_gps.time.second());
    if (llabs(static_cast<long long>(utc) - static_cast<long long>(time(nullptr))) > 2) {
        const timeval tv = {utc, 0};
        settimeofday(&tv, nullptr);
        board::rtcWrite(utc);
        Serial.println("[gps] time set from GPS");
    }
}

}  // namespace

void start()
{
    if (g_running) {
        return;
    }
    g_gps = TinyGPSPlus();
    g_running = true;
    g_detected = false;
    g_timeChecked = false;
    openPort();
}

void stop()
{
    if (!g_running) {
        return;
    }
    Serial1.end();
    g_running = false;
    g_detected = false;
}

bool running() { return g_running; }

void tick()
{
    if (!g_running) {
        return;
    }
    while (Serial1.available()) {
        g_gps.encode(static_cast<char>(Serial1.read()));
    }

    if (!g_detected) {
        if (g_gps.passedChecksum() > g_probeBaseline) {
            g_detected = true;
            g_lastModel = kCandidates[g_candidate].model;
            Serial.printf("[gps] detected %s at %u bps\n", kCandidates[g_candidate].model,
                          static_cast<unsigned>(kCandidates[g_candidate].baud));
        } else if (millis() - g_probeStartMs >= kProbeMs) {
            g_candidate = (g_candidate + 1) % kCandidateCount;
            openPort();
        }
        return;
    }
    syncTimeOnce();
}

bool detected() { return g_detected; }

const char *modelName()
{
    return g_lastModel;
}

bool hasFix()
{
    return g_running && g_detected && g_gps.location.isValid() &&
           g_gps.location.age() < kFixTimeoutMs;
}

double latitude() { return g_gps.location.lat(); }
double longitude() { return g_gps.location.lng(); }

float hdop()
{
    return g_gps.hdop.isValid() ? static_cast<float>(g_gps.hdop.hdop()) : 99.0f;
}

int satellites()
{
    return g_gps.satellites.isValid() ? static_cast<int>(g_gps.satellites.value()) : 0;
}

}  // namespace gps
