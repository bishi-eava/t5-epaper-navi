#include "board.h"

#include <SPI.h>
#include <WiFi.h>
#include <Wire.h>

#include "TouchDrvGT911.hpp"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include "lgfx/v1/platforms/esp32/Bus_EPD.h"
#include "lgfx/v1/platforms/esp32/Panel_EPD.hpp"

namespace board {
namespace {

// ---- ピンと定数 -------------------------------------------------------------

constexpr int kPanelWidth = 960;
constexpr int kPanelHeight = 540;
constexpr int kEpdBusSpeedHz = 20000000;

constexpr int kI2cSda = 39;
constexpr int kI2cScl = 40;

constexpr uint8_t kAddrPca9535 = 0x20;
constexpr uint8_t kAddrRtc = 0x51;
constexpr uint8_t kAddrGauge = 0x55;    // BQ27220
constexpr uint8_t kAddrTps = 0x68;      // TPS651851
constexpr uint8_t kAddrCharger = 0x6B;  // BQ25896

constexpr gpio_num_t kPinBoot = GPIO_NUM_0;
constexpr gpio_num_t kPinTouchInt = GPIO_NUM_3;
constexpr gpio_num_t kPinTouchRst = GPIO_NUM_9;
constexpr gpio_num_t kPinBacklight = GPIO_NUM_11;
constexpr gpio_num_t kPinDummyBus = GPIO_NUM_1;

constexpr gpio_num_t kPinEpdCkh = GPIO_NUM_4;
constexpr gpio_num_t kPinEpdD0 = GPIO_NUM_5;
constexpr gpio_num_t kPinEpdD1 = GPIO_NUM_6;
constexpr gpio_num_t kPinEpdD2 = GPIO_NUM_7;
constexpr gpio_num_t kPinEpdD3 = GPIO_NUM_15;
constexpr gpio_num_t kPinEpdD4 = GPIO_NUM_16;
constexpr gpio_num_t kPinEpdD5 = GPIO_NUM_17;
constexpr gpio_num_t kPinEpdD6 = GPIO_NUM_18;
constexpr gpio_num_t kPinEpdD7 = GPIO_NUM_8;
constexpr gpio_num_t kPinEpdSth = GPIO_NUM_41;
constexpr gpio_num_t kPinEpdLe = GPIO_NUM_42;
constexpr gpio_num_t kPinEpdStv = GPIO_NUM_45;
constexpr gpio_num_t kPinEpdCkv = GPIO_NUM_48;

constexpr int kPinSpiMiso = 21;
constexpr int kPinSpiMosi = 13;
constexpr int kPinSpiSck = 14;
constexpr int kPinSdCs = 12;
constexpr int kPinLoraCs = 46;
constexpr int kPinLoraBusy = 47;

// PCA9535 のピン番号（ポート0 = 0〜7、ポート1 = 8〜15）
constexpr uint8_t kPcaLoraGpsEn = 0;
constexpr uint8_t kPcaEpdOe = 8;
constexpr uint8_t kPcaEpdMode = 9;
constexpr uint8_t kPcaTpsPwrUp = 11;
constexpr uint8_t kPcaVcomCtrl = 12;
constexpr uint8_t kPcaTpsWakeUp = 13;
constexpr uint8_t kPcaTpsPowerGood = 14;

constexpr uint8_t kPcaRegInput = 0x00;
constexpr uint8_t kPcaRegOutput = 0x02;
constexpr uint8_t kPcaRegConfig = 0x06;

constexpr uint8_t kTpsRegEnable = 0x01;
constexpr uint8_t kTpsRegVcom = 0x03;
constexpr uint8_t kTpsRegPowerGood = 0x0F;
constexpr int kPowerGoodTimeoutMs = 400;

constexpr int kBacklightChannel = 0;
constexpr int kTapSlop = 24;  // これ以内の移動はタップとみなす

int g_vcomMv = kVcomDefaultMv;
int g_vcomReadbackMv = -1;

// ---- I2C ヘルパー -----------------------------------------------------------

bool i2cWrite(uint8_t addr, const uint8_t *data, size_t len)
{
    Wire.beginTransmission(addr);
    const size_t written = Wire.write(data, len);
    return written == len && Wire.endTransmission() == 0;
}

bool i2cWriteReg(uint8_t addr, uint8_t reg, uint8_t value)
{
    const uint8_t buf[2] = {reg, value};
    return i2cWrite(addr, buf, sizeof(buf));
}

bool i2cRead(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len)
{
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
        return false;
    }
    if (Wire.requestFrom(static_cast<int>(addr), static_cast<int>(len)) != len) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        buf[i] = static_cast<uint8_t>(Wire.read());
    }
    return true;
}

// ---- PCA9535 (IO 拡張) ------------------------------------------------------

uint8_t g_pcaOutput[2] = {0x00, 0x00};

bool pcaInit()
{
    // ポート0: すべて出力。IO0_0 (LORA_EN) は Low = GPS/LoRa 電源オフで始める。
    // ポート1: IO1_2 (ボタン)、IO1_6 (PWR_GOOD)、IO1_7 (TPS_INT) が入力。
    g_pcaOutput[0] = 0x00;
    g_pcaOutput[1] = 0x00;
    return i2cWriteReg(kAddrPca9535, kPcaRegOutput + 0, g_pcaOutput[0]) &&
           i2cWriteReg(kAddrPca9535, kPcaRegOutput + 1, g_pcaOutput[1]) &&
           i2cWriteReg(kAddrPca9535, kPcaRegConfig + 0, 0x00) &&
           i2cWriteReg(kAddrPca9535, kPcaRegConfig + 1, 0xC4);
}

bool pcaSet(uint8_t pin, bool level)
{
    const uint8_t port = pin / 8;
    const uint8_t mask = static_cast<uint8_t>(1U << (pin & 7));
    if (level) {
        g_pcaOutput[port] |= mask;
    } else {
        g_pcaOutput[port] &= static_cast<uint8_t>(~mask);
    }
    return i2cWriteReg(kAddrPca9535, kPcaRegOutput + port, g_pcaOutput[port]);
}

bool pcaGet(uint8_t pin, bool &level)
{
    uint8_t value = 0;
    if (!i2cRead(kAddrPca9535, kPcaRegInput + pin / 8, &value, 1)) {
        return false;
    }
    level = (value & (1U << (pin & 7))) != 0;
    return true;
}

// ---- 電子ペーパー -----------------------------------------------------------
// 電源シーケンスは公式サンプル examples/m5gfx_display_test に合わせている。

class EpdBus : public lgfx::Bus_EPD {
public:
    bool init() override
    {
        Wire.begin(kI2cSda, kI2cScl);
        Wire.setClock(400000);

        pinMode(kPinBacklight, OUTPUT);
        digitalWrite(kPinBacklight, LOW);
        pinMode(kPinTouchRst, OUTPUT);
        digitalWrite(kPinTouchRst, HIGH);

        if (!pcaInit()) {
            Serial.println("[board] PCA9535 init failed");
            return false;
        }
        return lgfx::Bus_EPD::init();
    }

    bool powerControl(bool on) override
    {
        if (_pwr_on == on) {
            return true;
        }
        wait();
        // 電源 IC が起き上がる前の I2C アクセスは失敗することがある（実機で確認）ので、やり直す。
        bool ok = false;
        for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
            if (attempt > 0) {
                powerOff();
                delay(10);
            }
            ok = on ? powerOn() : powerOff();
        }
        if (!ok) {
            Serial.printf("[board] EPD power %s failed\n", on ? "on" : "off");
            return false;
        }
        _pwr_on = on;
        return true;
    }

private:
    // 電源 IC への書き込み。応答があるまで少しのあいだ繰り返す。
    static bool tpsWrite(const uint8_t *data, size_t len)
    {
        for (int i = 0; i < 20; ++i) {
            if (i2cWrite(kAddrTps, data, len)) {
                return true;
            }
            delay(1);
        }
        return false;
    }

    bool waitPcaPowerGood()
    {
        for (int i = 0; i < kPowerGoodTimeoutMs; ++i) {
            bool level = false;
            if (pcaGet(kPcaTpsPowerGood, level) && level) {
                return true;
            }
            delay(1);
        }
        return false;
    }

    bool waitTpsPowerGood()
    {
        for (int i = 0; i < kPowerGoodTimeoutMs; ++i) {
            uint8_t value = 0;
            if (i2cRead(kAddrTps, kTpsRegPowerGood, &value, 1) && (value & 0xFA) == 0xFA) {
                return true;
            }
            delay(1);
        }
        return false;
    }

    bool powerOn()
    {
        if (!pcaSet(kPcaEpdOe, true) || !pcaSet(kPcaEpdMode, true) ||
            !pcaSet(kPcaTpsWakeUp, true) || !pcaSet(kPcaTpsPwrUp, true) ||
            !pcaSet(kPcaVcomCtrl, true)) {
            return false;
        }
        delay(1);
        if (!waitPcaPowerGood()) {
            return false;
        }
        const uint8_t enableData[2] = {kTpsRegEnable, 0x3F};
        if (!tpsWrite(enableData, sizeof(enableData))) {
            return false;
        }
        const uint16_t vcom = static_cast<uint16_t>(g_vcomMv / 10);  // 10mV 単位
        const uint8_t vcomData[3] = {kTpsRegVcom, static_cast<uint8_t>(vcom & 0xFF),
                                     static_cast<uint8_t>(vcom >> 8)};
        if (!tpsWrite(vcomData, sizeof(vcomData))) {
            return false;
        }
        uint8_t readback[2] = {0, 0};
        g_vcomReadbackMv = i2cRead(kAddrTps, kTpsRegVcom, readback, 2)
                               ? (readback[0] | ((readback[1] & 0x01) << 8)) * 10
                               : -1;
        return waitTpsPowerGood();
    }

    bool powerOff()
    {
        if (!pcaSet(kPcaEpdOe, false) || !pcaSet(kPcaEpdMode, false) ||
            !pcaSet(kPcaTpsPwrUp, false) || !pcaSet(kPcaVcomCtrl, false)) {
            return false;
        }
        delay(1);
        return pcaSet(kPcaTpsWakeUp, false);
    }
};

class T5Display : public lgfx::LGFX_Device {
public:
    T5Display()
    {
        auto busCfg = bus_.config();
        busCfg.bus_speed = kEpdBusSpeedHz;
        busCfg.pin_data[0] = kPinEpdD0;
        busCfg.pin_data[1] = kPinEpdD1;
        busCfg.pin_data[2] = kPinEpdD2;
        busCfg.pin_data[3] = kPinEpdD3;
        busCfg.pin_data[4] = kPinEpdD4;
        busCfg.pin_data[5] = kPinEpdD5;
        busCfg.pin_data[6] = kPinEpdD6;
        busCfg.pin_data[7] = kPinEpdD7;
        busCfg.pin_pwr = kPinDummyBus;
        busCfg.pin_spv = kPinEpdStv;
        busCfg.pin_ckv = kPinEpdCkv;
        busCfg.pin_sph = kPinEpdSth;
        busCfg.pin_oe = kPinDummyBus;
        busCfg.pin_le = kPinEpdLe;
        busCfg.pin_cl = kPinEpdCkh;
        busCfg.bus_width = 8;
        bus_.config(busCfg);

        panel_.setBus(&bus_);

        auto panelCfg = panel_.config();
        panelCfg.memory_width = kPanelWidth;
        panelCfg.memory_height = kPanelHeight;
        panelCfg.panel_width = kPanelWidth;
        panelCfg.panel_height = kPanelHeight;
        panelCfg.offset_x = 0;
        panelCfg.offset_y = 0;
        panelCfg.offset_rotation = 3;
        panelCfg.bus_shared = false;
        panel_.config(panelCfg);

        auto detail = panel_.config_detail();
        detail.line_padding = 0;
        detail.task_priority = 3;
        panel_.config_detail(detail);

        setPanel(&panel_);
    }

private:
    EpdBus bus_;
    lgfx::Panel_EPD panel_;
};

T5Display g_display;
TouchDrvGT911 g_touch;
bool g_touchReady = false;
bool g_sdMounted = false;
uint32_t g_sdLastTryMs = 0;
uint32_t g_sdFrequency = 25000000;
volatile bool g_homePressed = false;
uint32_t g_homeLastMs = 0;

// ---- 日時変換 ---------------------------------------------------------------

uint8_t bcdToDec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
uint8_t decToBcd(uint8_t v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); }

// 西暦の年月日から 1970-01-01 起点の通算日数を求める。
int64_t daysFromCivil(int y, int m, int d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = static_cast<int>(y - era * 400);
    const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

}  // namespace

lgfx::LGFX_Device &display = g_display;

bool begin()
{
    // スリープ中に保持していたピン状態を解除する。
    gpio_hold_dis(kPinTouchRst);
    gpio_deep_sleep_hold_dis();
    rtc_gpio_deinit(kPinBoot);

    pinMode(kPinBoot, INPUT_PULLUP);

    if (!g_display.init_without_reset(false)) {
        Serial.println("[board] display init failed");
        return false;
    }
    g_display.setAutoDisplay(false);
    g_display.setColorDepth(4);
    g_display.setRotation(0);
    g_display.setTextWrap(false);
    g_display.setEpdMode(epd_mode_t::epd_quality);

    ledcSetup(kBacklightChannel, 1000, 8);
    ledcAttachPin(kPinBacklight, kBacklightChannel);
    ledcWrite(kBacklightChannel, 0);

    g_touch.setPins(kPinTouchRst, kPinTouchInt);
    g_touchReady = g_touch.begin(Wire, GT911_SLAVE_ADDRESS_L, kI2cSda, kI2cScl);
    if (g_touchReady) {
        g_touch.setHomeButtonCallback(
            [](void *) {
                const uint32_t now = millis();
                if (now - g_homeLastMs >= 250) {
                    g_homeLastMs = now;
                    g_homePressed = true;
                }
            },
            nullptr);
        g_touch.setInterruptMode(LOW_LEVEL_QUERY);
        g_touch.setMaxCoordinates(g_display.width() - 1, g_display.height() - 1);
    } else {
        Serial.println("[board] GT911 init failed");
    }

    setRadioPower(true);
    return true;
}

void setDrawMode(epd_mode_t mode)
{
    g_display.setEpdMode(mode);
}

void present()
{
    g_display.powerSaveOff();
    g_display.display();
    g_display.waitDisplay();
    g_display.powerSaveOn();
}

int vcomReadback() { return g_vcomReadbackMv; }

void setVcom(int millivolts)
{
    g_vcomMv = constrain(millivolts, kVcomMinMv, kVcomMaxMv);
}

void setFrontlight(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    ledcWrite(kBacklightChannel, percent * 255 / 100);
}

Battery readBattery()
{
    Battery b;
    uint8_t buf[2];
    if (i2cRead(kAddrGauge, 0x2C, buf, 2)) {  // StateOfCharge
        b.percent = buf[0] | (buf[1] << 8);
        b.valid = b.percent >= 0 && b.percent <= 100;
    }
    if (i2cRead(kAddrGauge, 0x08, buf, 2)) {  // Voltage
        b.millivolts = buf[0] | (buf[1] << 8);
    }
    if (i2cRead(kAddrGauge, 0x0C, buf, 2)) {  // Current（符号付き、正が充電）
        b.milliamps = static_cast<int16_t>(buf[0] | (buf[1] << 8));
    }
    uint8_t status = 0;
    if (i2cRead(kAddrCharger, 0x0B, &status, 1)) {
        const uint8_t chargeState = (status >> 3) & 0x03;  // 1: プリチャージ, 2: 急速充電
        b.charging = chargeState == 1 || chargeState == 2;
        b.usbPower = (status & 0x04) != 0;  // Power Good
    }
    return b;
}

time_t rtcRead()
{
    uint8_t r[7];
    if (!i2cRead(kAddrRtc, 0x02, r, sizeof(r))) {
        return 0;
    }
    if (r[0] & 0x80) {  // VL: 電源断で時刻が失われている
        return 0;
    }
    const int sec = bcdToDec(r[0] & 0x7F);
    const int min = bcdToDec(r[1] & 0x7F);
    const int hour = bcdToDec(r[2] & 0x3F);
    const int day = bcdToDec(r[3] & 0x3F);
    const int month = bcdToDec(r[5] & 0x1F);
    const int year = 2000 + bcdToDec(r[6]);
    if (year < 2024 || month < 1 || month > 12 || day < 1 || day > 31) {
        return 0;
    }
    return utcFromCivil(year, month, day, hour, min, sec);
}

time_t utcFromCivil(int year, int month, int day, int hour, int minute, int second)
{
    return static_cast<time_t>(daysFromCivil(year, month, day) * 86400 + hour * 3600 +
                               minute * 60 + second);
}

bool rtcWrite(time_t utc)
{
    struct tm t;
    gmtime_r(&utc, &t);
    const uint8_t buf[8] = {
        0x02,
        decToBcd(t.tm_sec),
        decToBcd(t.tm_min),
        decToBcd(t.tm_hour),
        decToBcd(t.tm_mday),
        static_cast<uint8_t>(t.tm_wday),
        decToBcd(t.tm_mon + 1),
        decToBcd((t.tm_year + 1900) % 100),
    };
    return i2cWrite(kAddrRtc, buf, sizeof(buf));
}

void setRadioPower(bool on)
{
    if (!on) {
        pcaSet(kPcaLoraGpsEn, false);
        pinMode(kPinLoraCs, INPUT);  // 電源のないチップへ、信号線から電流を流し込まない
        return;
    }

    // CS を High にして LoRa チップを非選択にしてから通電する。
    pinMode(kPinLoraCs, OUTPUT);
    digitalWrite(kPinLoraCs, HIGH);
    pinMode(kPinLoraBusy, INPUT);
    pcaSet(kPcaLoraGpsEn, true);

    // LoRa チップ (SX1262) をスリープさせる。起動完了 (BUSY が Low) を待ってから
    // SetSleep コマンドを送る。以後 CS を High に保つあいだはスリープしたままになる。
    SPI.begin(kPinSpiSck, kPinSpiMiso, kPinSpiMosi, kPinSdCs);
    delay(5);
    for (int i = 0; i < 100 && digitalRead(kPinLoraBusy) == HIGH; ++i) {
        delay(1);
    }
    SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    digitalWrite(kPinLoraCs, LOW);
    SPI.transfer(0x84);  // SetSleep
    SPI.transfer(0x00);  // 設定を保持しないコールドスタート
    digitalWrite(kPinLoraCs, HIGH);
    SPI.endTransaction();
}

bool sdReady()
{
    const uint32_t now = millis();
    if (g_sdMounted) {
        // カードが抜き差しされると、マウント済みのままでは読めなくなる。
        // 定期的に実際に読めるか確かめ、読めなければマウントし直す。
        static uint32_t lastCheckMs = 0;
        if (now - lastCheckMs < 1000) {
            return true;
        }
        lastCheckMs = now;
        File root = SD.open("/");
        const bool ok = root && root.isDirectory();
        root.close();
        if (ok) {
            return true;
        }
        Serial.println("[board] SD card lost, remounting");
        SD.end();
        g_sdMounted = false;
        g_sdLastTryMs = 0;
    }
    if (g_sdLastTryMs != 0 && now - g_sdLastTryMs < 1000) {
        return false;  // 連続でのマウント試行を避ける
    }
    g_sdLastTryMs = now ? now : 1;
    SPI.begin(kPinSpiSck, kPinSpiMiso, kPinSpiMosi, kPinSdCs);
    g_sdMounted = SD.begin(kPinSdCs, SPI, g_sdFrequency);
    if (!g_sdMounted) {
        SD.end();
        Serial.println("[board] SD mount failed");
    }
    return g_sdMounted;
}

void sdSetFrequency(uint32_t hz)
{
    g_sdFrequency = hz;
    if (g_sdMounted) {
        SD.end();
        g_sdMounted = false;
    }
    g_sdLastTryMs = 0;
}

int sdMountTest(int count, uint32_t frequency)
{
    int ok = 0;
    for (int i = 0; i < count; ++i) {
        SD.end();
        g_sdMounted = false;
        SPI.begin(kPinSpiSck, kPinSpiMiso, kPinSpiMosi, kPinSdCs);
        if (SD.begin(kPinSdCs, SPI, frequency)) {
            File root = SD.open("/");
            if (root && root.openNextFile()) {
                ++ok;
            }
            root.close();
        }
        delay(50);
    }
    SD.end();
    g_sdLastTryMs = 0;
    return ok;
}

TouchEvent pollTouch()
{
    static bool down = false;
    static int startX = 0, startY = 0, lastX = 0, lastY = 0;

    TouchEvent ev;
    if (!g_touchReady) {
        return ev;
    }

    int16_t x = 0, y = 0;
    const bool pressed = g_touch.isPressed() && g_touch.getPoint(&x, &y, 1) > 0;
    if (pressed) {
        if (!down) {
            down = true;
            startX = x;
            startY = y;
        }
        lastX = x;
        lastY = y;
        ev.touching = true;
        return ev;
    }

    if (down) {
        down = false;
        ev.x = startX;
        ev.y = startY;
        ev.dx = lastX - startX;
        ev.dy = lastY - startY;
        ev.type = (abs(ev.dx) <= kTapSlop && abs(ev.dy) <= kTapSlop) ? TouchEvent::Tap
                                                                    : TouchEvent::Drag;
    }
    return ev;
}

bool takeHomePressed()
{
    const bool pressed = g_homePressed;
    g_homePressed = false;
    return pressed;
}

bool bootPressed()
{
    return digitalRead(kPinBoot) == LOW;
}

void deepSleep()
{
    // BOOT を押したまま入ると即座に復帰してしまうので、離されるのを待つ。
    while (bootPressed()) {
        delay(10);
    }
    delay(50);

    g_display.waitDisplay();
    g_display.powerSaveOn();  // 画面電源 (TPS65185) を落とす

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    if (g_sdMounted) {
        SD.end();
        g_sdMounted = false;
    }
    SPI.end();

    setRadioPower(false);
    ledcWrite(kBacklightChannel, 0);
    ledcDetachPin(kPinBacklight);
    pinMode(kPinBacklight, OUTPUT);
    digitalWrite(kPinBacklight, LOW);

    // タッチ IC を止め、リセットを Low のまま保持する（公式ファームと同じ手順）。
    if (g_touchReady) {
        g_touch.sleep();
    }
    digitalWrite(kPinTouchRst, LOW);
    gpio_hold_en(kPinTouchRst);
    gpio_deep_sleep_hold_en();

    Serial.flush();

    rtc_gpio_pullup_en(kPinBoot);
    rtc_gpio_pulldown_dis(kPinBoot);
    esp_sleep_enable_ext0_wakeup(kPinBoot, 0);
    esp_deep_sleep_start();
    while (true) {
    }
}

}  // namespace board
