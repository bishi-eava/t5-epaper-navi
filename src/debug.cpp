#include "debug.h"

#include <WiFi.h>
#include <errno.h>

#include <algorithm>

#include "apps.h"
#include "gps.h"
#include "net.h"
#include "settings.h"
#include "ui.h"
#include "webfiles.h"

namespace debug {
namespace {

String g_line;

void sendScreenshot()
{
    static uint8_t rgb[ui::kWidth * 3];
    static char hex[ui::kWidth + 2];
    Serial.printf("SHOT %d %d\n", ui::kWidth, ui::kHeight);
    for (int y = 0; y < ui::kHeight; ++y) {
        board::display.readRectRGB(0, y, ui::kWidth, 1, rgb);
        for (int x = 0; x < ui::kWidth; ++x) {
            hex[x] = "0123456789ABCDEF"[rgb[x * 3] >> 4];
        }
        hex[ui::kWidth] = '\n';
        Serial.write(reinterpret_cast<const uint8_t *>(hex), ui::kWidth + 1);
    }
    Serial.println("ENDSHOT");
}

void sendInfo()
{
    const bool sd = board::sdReady();
    Serial.printf("INFO sd=%d", sd);
    if (sd) {
        Serial.printf(" sdTotalMB=%u sdUsedMB=%u tiles=%d wifiTxt=%d",
                      static_cast<unsigned>(SD.totalBytes() / (1024 * 1024)),
                      static_cast<unsigned>(SD.usedBytes() / (1024 * 1024)),
                      SD.exists("/cyberjapandata.gsi.go.jp/xyz/std"), SD.exists("/wifi.txt"));
    }
    Serial.printf(" wifi=%d ssid=%s gpsRunning=%d gpsDetected=%d gpsModel=%s fix=%d sats=%d "
                  "hdop=%.1f lat=%.6f lon=%.6f heap=%u psram=%u\n",
                  net::connected(), net::currentSsid().c_str(), gps::running(), gps::detected(),
                  gps::modelName(), gps::hasFix(), gps::satellites(), gps::hdop(), gps::latitude(),
                  gps::longitude(), static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(ESP.getFreePsram()));
    // 直前のスリープで減った電池の量と、そこから求めた平均電流。
    const board::SleepReport sleep = board::lastSleep();
    if (sleep.valid) {
        const int usedMah = sleep.mahBefore - sleep.mahAfter;
        Serial.printf("INFO lastSleep seconds=%u mah=%d->%d percent=%d->%d mv=%d->%d "
                      "wakeMilliamps=%d",
                      static_cast<unsigned>(sleep.seconds), sleep.mahBefore, sleep.mahAfter,
                      sleep.percentBefore, sleep.percentAfter, sleep.millivoltsBefore,
                      sleep.millivoltsAfter, sleep.wakeMilliamps);
        if (sleep.seconds > 0) {
            Serial.printf(" avgMilliamps=%.2f", usedMah * 3600.0 / sleep.seconds);
        }
        Serial.println();
    } else {
        Serial.println("INFO lastSleep none");
    }
    if (sd) {
        File root = SD.open("/");
        int count = 0;
        for (File f = root.openNextFile(); f && count < 30; f = root.openNextFile(), ++count) {
            Serial.printf("INFO root: %s%s\n", f.name(), f.isDirectory() ? "/" : "");
            f.close();
        }
        root.close();
    }
}

void sendListing(const String &path)
{
    if (!board::sdReady()) {
        Serial.println("LS sd not ready");
        return;
    }
    File dir = SD.open(path);
    if (!dir || !dir.isDirectory()) {
        Serial.printf("LS not a directory: %s\n", path.c_str());
        return;
    }
    int count = 0;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile(), ++count) {
        if (count < 200) {
            Serial.printf("LS %s%s %u\n", f.name(), f.isDirectory() ? "/" : "",
                          static_cast<unsigned>(f.size()));
        }
        f.close();
    }
    dir.close();
    Serial.printf("LS total %d\n", count);
}

void run(const String &line, board::TouchEvent &ev, bool &home)
{
    int a = 0, b = 0, c = 0, d = 0;
    if (line == "shot") {
        sendScreenshot();
    } else if (line == "charger") {
        board::chargerDump(Serial);
    } else if (line == "info") {
        sendInfo();
        const settings::MapView view = settings::mapView();
        Serial.printf("INFO vcom setting=%d readback=%d\n", settings::vcom(),
                      board::vcomReadback());
        Serial.printf("INFO mapView lat=%.6f lon=%.6f zoom=%d\n", view.lat, view.lon, view.zoom);
    } else if (line.startsWith("routekey ")) {
        // 経路サービスの API キーを保存する。キーそのものは出力しない。
        const bool ok = settings::setRouteKey(line.substring(9));
        Serial.printf("INFO routekey saved=%d length=%u\n", ok,
                      static_cast<unsigned>(settings::routeKey().length()));
    } else if (line == "gps off") {
        gps::stopSimulation();
    } else if (line.startsWith("gps ")) {
        double lat = 0, lon = 0;
        if (sscanf(line.c_str(), "gps %lf %lf", &lat, &lon) == 2) {
            gps::simulate(lat, lon);
        }
    } else if (line.startsWith("dest ")) {
        double lat = 0, lon = 0;
        if (sscanf(line.c_str(), "dest %lf %lf", &lat, &lon) == 2) {
            apps::offerDestination(lat, lon, "コンソールから");
            Serial.printf("INFO dest offered lat=%.6f lon=%.6f\n", lat, lon);
        }
    } else if (sscanf(line.c_str(), "web %d", &a) == 1) {
        if (a) {
            const bool ok = webfiles::start(false);
            Serial.printf("INFO web started=%d url=%s\n", ok, webfiles::url().c_str());
        } else {
            webfiles::stop();
        }
    } else if (line.startsWith("head ")) {
        File f = board::sdReady() ? SD.open(line.substring(5)) : File();
        if (!f || f.isDirectory()) {
            Serial.println("HEAD cannot open");
        } else {
            uint8_t buf[32] = {0};
            const size_t n = f.read(buf, sizeof(buf));
            Serial.printf("HEAD size=%u bytes=", static_cast<unsigned>(f.size()));
            for (size_t i = 0; i < n; ++i) {
                Serial.printf("%02X", buf[i]);
            }
            Serial.println();
            f.close();
        }
    } else if (line.startsWith("ls ")) {
        sendListing(line.substring(3));
    } else if (sscanf(line.c_str(), "sdtest %d %d", &a, &b) == 2) {
        const uint32_t start = millis();
        const int ok = board::sdMountTest(a, static_cast<uint32_t>(b));
        Serial.printf("INFO sdtest freq=%d ok=%d/%d ms=%u\n", b, ok, a,
                      static_cast<unsigned>(millis() - start));
    } else if (sscanf(line.c_str(), "sdwrite %d %d", &a, &b) == 2) {
        // 調査用: a KB のファイルを b バイトずつ書き、読み戻して確かめる。
        static uint8_t chunk[4096];
        const size_t step = std::min<size_t>(sizeof(chunk), std::max(1, b));
        const char *path = "/t5-epaper-sdwrite.tmp";
        bool ok = board::sdReady();
        size_t total = 0, failAt = 0;
        int err = 0;
        const uint32_t start = millis();
        if (ok) {
            File f = SD.open(path, FILE_WRITE);
            ok = f;
            while (ok && total < static_cast<size_t>(a) * 1024) {
                for (size_t i = 0; i < step; ++i) {
                    chunk[i] = static_cast<uint8_t>((total + i) * 31 + 7);
                }
                errno = 0;
                const size_t n = f.write(chunk, step);
                if (n != step) {
                    ok = false;
                    failAt = total + n;
                    err = errno;
                }
                total += n;
            }
            f.close();
        }
        const uint32_t writeMs = millis() - start;
        size_t verified = 0;
        if (ok) {
            File f = SD.open(path);
            while (f && verified < total) {
                const int n = f.read(chunk, step);
                if (n <= 0) break;
                bool same = true;
                for (int i = 0; i < n; ++i) {
                    same &= chunk[i] == static_cast<uint8_t>((verified + i) * 31 + 7);
                }
                if (!same) break;
                verified += n;
            }
            f.close();
            ok = verified == total;
        }
        SD.remove(path);
        Serial.printf("INFO sdwrite kb=%d step=%u ok=%d total=%u failAt=%u errno=%d verified=%u ms=%u\n",
                      a, static_cast<unsigned>(step), ok, static_cast<unsigned>(total),
                      static_cast<unsigned>(failAt), err, static_cast<unsigned>(verified),
                      static_cast<unsigned>(writeMs));
    } else if (sscanf(line.c_str(), "sdfreq %d", &a) == 1) {
        board::sdSetFrequency(static_cast<uint32_t>(a));
        Serial.printf("INFO sdfreq=%d ready=%d\n", a, board::sdReady());
    } else if (sscanf(line.c_str(), "gpspower %d", &a) == 1) {
        board::setRadioPower(a != 0);
    } else if (line == "home") {
        home = true;
    } else if (sscanf(line.c_str(), "tap %d %d", &a, &b) == 2) {
        ev.type = board::TouchEvent::Tap;
        ev.x = a;
        ev.y = b;
    } else if (sscanf(line.c_str(), "drag %d %d %d %d", &a, &b, &c, &d) == 4) {
        ev.type = board::TouchEvent::Drag;
        ev.x = a;
        ev.y = b;
        ev.dx = c;
        ev.dy = d;
    } else {
        Serial.printf("ERR unknown command: %s\n", line.c_str());
        return;
    }
    // 受け取ったコマンドを返す。API キーは出力に残さない。
    Serial.printf("OK %s\n", line.startsWith("routekey ") ? "routekey" : line.c_str());
}

}  // namespace

void poll(board::TouchEvent &ev, bool &home)
{
    while (Serial.available()) {
        const char ch = static_cast<char>(Serial.read());
        if (ch == '\n' || ch == '\r') {
            if (g_line.length() > 0) {
                const String line = g_line;
                g_line = "";
                run(line, ev, home);
                return;  // 1 回の呼び出しで処理するコマンドは 1 つ
            }
        } else if (g_line.length() < 120) {
            g_line += ch;
        }
    }
}

}  // namespace debug
