// SD カードへの確実なファイル書き込み。
//
// WiFi の通信中は、SD カードへの書き込みがまれに失敗する（実機で確認: 500KB の受信で
// 10 回に 1〜2 回。SD カード単体の書き込みでは起きない）。Arduino の File は書き込みの
// 失敗を取りこぼすことがあるので、ここでは一定量ごとに実際にカードへ書き出して結果を
// 確かめ、失敗したらファイルを開き直して、確認済みの位置から書き直す。
#pragma once

#include <Arduino.h>
#include <stdio.h>

class SafeWriter {
public:
    ~SafeWriter() { abort(); }

    // SD カード上のパス（"/" 始まり）に新しく書き始める。既存のファイルは上書きする。
    bool begin(const String &sdPath);

    // データを追記する。やり直しても書けなかった場合は false（以後は書けない）。
    bool write(const uint8_t *data, size_t length);

    // 残りを書き出して閉じる。すべて書けていれば true。
    bool finish();

    // 書きかけのファイルを閉じる（ファイル自体は残る。消すのは呼び出し側）。
    void abort();

    bool failed() const { return failed_; }
    size_t bytesWritten() const { return committed_ + pending_; }
    // 書き直しが発生した回数（調査用）。
    static uint32_t retryCount() { return retries_; }

private:
    bool commit();

    static constexpr size_t kBlockSize = 16 * 1024;
    static uint32_t retries_;

    String path_;            // VFS 上のパス（"/sd" + SD カード上のパス）
    FILE *file_ = nullptr;
    uint8_t *block_ = nullptr;
    size_t pending_ = 0;     // block_ にたまっている、まだ書き出していない量
    size_t committed_ = 0;   // カードへの書き出しを確認できた量
    bool failed_ = false;
};
