// t5-epaper-navi: LilyGO T5 E-Paper S3 Pro 向けファームウェア。仕様は SPEC.md を参照。
#include <Arduino.h>

#include "apps.h"
#include "board.h"
#include "debug.h"
#include "gps.h"
#include "net.h"
#include "settings.h"
#include "ui.h"
#include "webfiles.h"

namespace {

uint32_t g_lastActivityMs = 0;
bool g_bootArmed = false;  // 復帰時に押されていた BOOT が一度離されたか

// ロック画面を表示してディープスリープに入る。BOOT ボタンで復帰（再起動）する。
[[noreturn]] void lockAndSleep()
{
    apps::showLockScreen();
    gps::stop();  // 地図を開いたままスリープするときも、UART を閉じて送信線を解放する
    board::deepSleep();
}

}  // namespace

void setup()
{
    Serial.begin(115200);
    Serial.println("[t5-epaper] start " FIRMWARE_VERSION);

    // このボードの設定では loop() がコア 0 で動く。SD カードへの書き込みや TLS 通信は
    // 数秒ブロックすることがあり、既定のままだとウォッチドッグで再起動してしまう
    // （容量の大きい SD カードでのフォルダ作成中に発生することを実機で確認）。
    disableCore0WDT();

    if (!board::begin()) {
        // 画面が使えないと何も表示できないので、ログだけ出して止まる。
        while (true) {
            Serial.println("[t5-epaper] display init failed");
            delay(2000);
        }
    }

    settings::load();
    board::setVcom(settings::vcom());
    board::setFrontlight(settings::frontlight());
    net::begin();

    ui::setHome(apps::home());
    ui::goHome();
    g_lastActivityMs = millis();
}

void loop()
{
    net::tick();
    webfiles::tick();  // WiFi 転送中だけ、ブラウザからの要求を処理する

    board::TouchEvent ev = board::pollTouch();
    bool homePressed = board::takeHomePressed();
    debug::poll(ev, homePressed);  // シリアルからの操作も、画面の操作と同じに扱う
    if (ev.touching || ev.type != board::TouchEvent::None) {
        g_lastActivityMs = millis();
    }
    ui::dispatch(ev);

    if (homePressed) {
        g_lastActivityMs = millis();
        ui::goHome();
    }

    // BOOT ボタン: 起きている間に押すと、すぐにロックしてスリープする。
    if (!board::bootPressed()) {
        g_bootArmed = true;
    } else if (g_bootArmed) {
        lockAndSleep();
    }

    ui::update();
    if (ui::takeActivity()) {
        g_lastActivityMs = millis();
    }

    const uint32_t timeoutMs = settings::sleepMinutes() * 60000UL;
    if (timeoutMs > 0 && millis() - g_lastActivityMs >= timeoutMs) {
        lockAndSleep();
    }

    delay(20);
}
