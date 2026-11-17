// ロック画面: 画像のみを全画面に表示する。スリープ中もこの表示が残る。
#include "apps.h"
#include "settings.h"

namespace apps {
namespace {

using board::display;

// 画像が未設定、または読めないときの既定の絵（山と月）。
void drawDefaultImage()
{
    const int w = ui::kWidth, h = ui::kHeight;
    // 空: 上から下へ明るくなる階調
    for (int y = 0; y < h; ++y) {
        const int level = 120 + y * 120 / h;
        display.drawFastHLine(0, y, w, ui::gray(static_cast<uint8_t>(level)));
    }
    display.fillCircle(390, 230, 62, TFT_WHITE);
    display.fillTriangle(-80, h, 190, 520, 460, h, ui::gray(90));
    display.fillTriangle(180, h, 430, 610, 680, h, ui::gray(50));
    display.fillTriangle(-140, h, 60, 760, 300, h, TFT_BLACK);
}

}  // namespace

void showLockScreen()
{
    board::setDrawMode(board::epd_mode_t::epd_quality);
    display.fillScreen(TFT_WHITE);
    const String path = settings::lockImage();
    if (path.length() == 0 || !drawImageFit(path, 0, 0, ui::kWidth, ui::kHeight)) {
        drawDefaultImage();
    }
    board::present();
}

}  // namespace apps
