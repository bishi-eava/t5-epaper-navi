#include "safewrite.h"

#include <unistd.h>

uint32_t SafeWriter::retries_ = 0;

namespace {
constexpr char kMountPoint[] = "/sd";  // SD.begin() の既定のマウント先
constexpr int kMaxAttempts = 5;
}  // namespace

bool SafeWriter::begin(const String &sdPath)
{
    abort();
    path_ = String(kMountPoint) + sdPath;
    pending_ = 0;
    committed_ = 0;
    failed_ = false;
    if (block_ == nullptr) {
        block_ = static_cast<uint8_t *>(ps_malloc(kBlockSize));
    }
    if (block_ == nullptr) {
        failed_ = true;
        return false;
    }
    // 空のファイルを作る（既存のファイルは空にする）。
    for (int attempt = 0; attempt < kMaxAttempts && file_ == nullptr; ++attempt) {
        file_ = fopen(path_.c_str(), "wb");
        if (file_ == nullptr) {
            delay(20);
        }
    }
    failed_ = file_ == nullptr;
    return !failed_;
}

bool SafeWriter::write(const uint8_t *data, size_t length)
{
    if (failed_ || block_ == nullptr) {
        return false;
    }
    while (length > 0) {
        const size_t n = std::min(length, kBlockSize - pending_);
        memcpy(block_ + pending_, data, n);
        pending_ += n;
        data += n;
        length -= n;
        if (pending_ == kBlockSize && !commit()) {
            return false;
        }
    }
    return true;
}

// たまっている分をカードへ書き出し、結果を確かめる。失敗したら開き直して書き直す。
bool SafeWriter::commit()
{
    if (pending_ == 0) {
        return !failed_;
    }
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (file_ == nullptr) {
            // 確認済みの位置から書き直す。
            file_ = fopen(path_.c_str(), committed_ == 0 ? "wb" : "r+b");
            if (file_ != nullptr && committed_ > 0 &&
                fseek(file_, static_cast<long>(committed_), SEEK_SET) != 0) {
                fclose(file_);
                file_ = nullptr;
            }
        }
        if (file_ != nullptr) {
            const bool ok = fwrite(block_, 1, pending_, file_) == pending_ && fflush(file_) == 0 &&
                            fsync(fileno(file_)) == 0;
            if (ok) {
                committed_ += pending_;
                pending_ = 0;
                return true;
            }
            fclose(file_);
            file_ = nullptr;
        }
        ++retries_;
        Serial.printf("[sd] write retry %d at %u (%s)\n", attempt + 1,
                      static_cast<unsigned>(committed_), path_.c_str());
        delay(20);
    }
    failed_ = true;
    return false;
}

bool SafeWriter::finish()
{
    const bool ok = commit();
    if (file_ != nullptr) {
        const bool closed = fclose(file_) == 0;
        file_ = nullptr;
        if (!closed) {
            failed_ = true;
        }
    }
    return ok && !failed_;
}

void SafeWriter::abort()
{
    if (file_ != nullptr) {
        fclose(file_);
        file_ = nullptr;
    }
    pending_ = 0;
}
