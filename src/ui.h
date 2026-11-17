// 画面共通の部品: ステータスバー、アプリ切り替え、ボタン、ソフトウェアキーボード。
#pragma once

#include <Arduino.h>

#include "board.h"

namespace ui {

constexpr int kWidth = 540;
constexpr int kHeight = 960;
constexpr int kStatusHeight = 56;                 // ステータスバーの高さ
constexpr int kContentTop = kStatusHeight;        // アプリが描画できる領域の上端
constexpr int kBottomBarHeight = 84;              // アプリ下部の操作バーの高さ
constexpr int kBottomBarTop = kHeight - kBottomBarHeight;

struct Rect {
    int x, y, w, h;
    bool contains(int px, int py) const
    {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

// 日本語フォント（プロポーショナル）
extern const lgfx::IFont *const fontSmall;   // 20px
extern const lgfx::IFont *const fontNormal;  // 24px
extern const lgfx::IFont *const fontMedium;  // 28px
extern const lgfx::IFont *const fontLarge;   // 36px

uint32_t gray(uint8_t level);  // 0 = 黒, 255 = 白

// 文字色に使う白。TFT_WHITE は 16 ビット色の定数なので、text() の色引数（24 ビット色）には
// 渡せない（水色として解釈され、灰色で表示されてしまう）。
constexpr uint32_t kWhite = 0xFFFFFFu;

// 文字列を描く。色は黒、背景は透過。
void text(const String &s, int x, int y, const lgfx::IFont *font,
          lgfx::textdatum_t datum = lgfx::textdatum_t::top_left, uint32_t color = TFT_BLACK);

// 幅に収まるよう末尾を「…」で省略する。
String fitText(const String &s, int maxWidth, const lgfx::IFont *font);

// 角丸のボタン。filled なら黒地に白文字。
void button(const Rect &r, const String &label, bool filled = false,
            const lgfx::IFont *font = nullptr);

// タップに反応する領域として登録する（描画のときに呼ぶ）。登録した領域は、タップされた
// 瞬間に白黒反転して「押された」ことを示す。button() で描いたボタンは自動で登録される。
void touchable(const Rect &r);

// 画面中央にメッセージを出して即座に反映する（時間のかかる処理の前に使う）。
void message(const String &line1, const String &line2 = String());

// ---- アプリ -----------------------------------------------------------------

class App {
public:
    virtual ~App() = default;
    virtual const char *title() const = 0;
    virtual void enter() {}
    virtual void leave() {}
    // コンテンツ領域（kContentTop より下）を描く。背景は白で消去済み。
    virtual void draw() = 0;
    virtual void tap(int x, int y) {}
    virtual void drag(int x, int y, int dx, int dy) {}
    virtual void tick() {}
    // 「戻る」が押されたとき。アプリ内で処理したら true、ホームに戻るなら false。
    virtual bool back() { return false; }
};

void setHome(App &home);
void open(App &app);
void goHome();
bool isHome();

// 次のフレームで全体を描き直す。
void requestRedraw();
// ステータスバーだけを描き直す。
void requestStatusRedraw();
// アプリが画面の一部だけを描き直すときの手順:
//   beginPartial() → 描画 → flush()
// 既定の高速モードは白黒 2 値になる。階調が要るときは epd_text を指定する。
void beginPartial(board::epd_mode_t mode = board::epd_mode_t::epd_fast);
void flush();
// 残像を消すため、次の全体描画を高品質モードで行う。
void requestCleanRefresh();

// 画面の操作以外の活動（通信など）があったことを知らせる。無操作スリープまでの時間が延びる。
void noteActivity();
// noteActivity() が呼ばれていたら true を返してクリアする。
bool takeActivity();

// タッチイベントを現在のアプリへ配る。
void dispatch(const board::TouchEvent &ev);
// 定期処理と、必要な再描画を行う。
void update();

// ---- ソフトウェアキーボード（英数字と記号）----------------------------------

class Keyboard {
public:
    enum Result { None, Done, Cancel };

    void begin(const String &label, const String &initial = String());
    void draw();
    Result tap(int x, int y);
    const String &value() const { return value_; }

private:
    void drawField();
    void drawKeys();

    String label_;
    String value_;
    int page_ = 0;  // 0: 小文字, 1: 大文字, 2: 記号
};

}  // namespace ui
