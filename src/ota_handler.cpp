// -----------------------------------------------------------------------------
// ota_handler.cpp
// OTA обновление прошивки через JSON-протокол.
//
// Протокол:
//   ota_start(size)  → begin Update
//   otaWriteChunk(base64) → decode → write to flash
//   otaFinalize() → end Update, verify, reboot
//
// Размер чанка: ~256 байт (бинарных) → ~344 символа base64
// -----------------------------------------------------------------------------

#include "ota_handler.h"
#include "app_config.h"
#include <Update.h>

static bool      otaActive = false;
static size_t    otaSize = 0;
static size_t    otaWritten = 0;
static uint8_t   otaDecodeBuf[512];  // Буфер для декодированного base64

// =============================================================================
// Простой base64 декодер (без внешних зависимостей)
// =============================================================================

static const char b64Table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64Decode(const char* input, uint8_t* output) {
    int inputLen = 0;
    while (input[inputLen]) inputLen++;

    int outputLen = 0;
    int val = 0;
    int valb = -8;

    for (int i = 0; i < inputLen; i++) {
        char c = input[i];
        if (c == '=' || c == '\r' || c == '\n' || c == ' ') continue;

        int pos = -1;
        for (int j = 0; j < 64; j++) {
            if (b64Table[j] == c) { pos = j; break; }
        }
        if (pos == -1) continue;

        val = (val << 6) + pos;
        valb += 6;
        if (valb >= 0) {
            output[outputLen++] = (uint8_t)((val >> valb) & 0xFF);
            valb -= 8;
        }
    }
    return outputLen;
}

// =============================================================================
// otaBegin — начать запись
// =============================================================================

bool otaBegin(size_t firmwareSize) {
    if (otaActive) {
        Serial.println("[OTA] Already in progress!");
        return false;
    }

    Serial.printf("[OTA] Starting... size=%u bytes\n", firmwareSize);

    if (!Update.begin(firmwareSize)) {
#if DEBUG_LOG
        Update.printError(Serial);
#endif
        return false;
    }

    otaActive = true;
    otaSize = firmwareSize;
    otaWritten = 0;

    Serial.println("[OTA] Update initialized. Ready to receive chunks.");
    return true;
}

// =============================================================================
// otaWriteChunk — декодировать base64 и записать во flash
// =============================================================================

bool otaWriteChunk(const char* base64Data) {
    if (!otaActive) {
        Serial.println("[OTA] Not active!");
        return false;
    }

    // Декодируем base64
    int decodedLen = b64Decode(base64Data, otaDecodeBuf);
    if (decodedLen <= 0) {
        Serial.println("[OTA] Base64 decode failed!");
        return false;
    }

    // Пишем во flash
    size_t written = Update.write(otaDecodeBuf, decodedLen);
    if (written != (size_t)decodedLen) {
        Serial.printf("[OTA] Write failed! Expected %d, wrote %zu\n", decodedLen, written);
#if DEBUG_LOG
        Update.printError(Serial);
#endif
        return false;
    }

    otaWritten += written;

    int progress = (int)((otaWritten * 100) / otaSize);
    static int lastLog = 0;
    if (progress - lastLog >= 10) {
        lastLog = progress;
        Serial.printf("[OTA] Progress: %d%% (%u/%u)\n", progress, (unsigned)otaWritten, (unsigned)otaSize);
    }

    return true;
}

// =============================================================================
// otaFinalize — завершить обновление
// =============================================================================

bool otaFinalize() {
    if (!otaActive) {
        Serial.println("[OTA] Not active!");
        return false;
    }

    Serial.println("[OTA] Finalizing...");

    if (!Update.end(true)) {  // true = reboot после проверки
#if DEBUG_LOG
        Update.printError(Serial);
#endif
        otaActive = false;
        return false;
    }

    Serial.printf("[OTA] Complete! %u bytes written.\n", (unsigned)otaWritten);
    Serial.println("[OTA] Rebooting...");

    otaActive = false;
    return true;
}

int otaGetProgress() {
    if (!otaActive) return 0;
    if (otaSize == 0) return 0;
    return (int)((otaWritten * 100) / otaSize);
}

bool otaIsActive() {
    return otaActive;
}
