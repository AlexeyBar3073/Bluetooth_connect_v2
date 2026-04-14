// -----------------------------------------------------------------------------
// ota_task.h
// OTA-обновление прошивки — полноправная задача FreeRTOS.
//
// Архитектура:
//   - Запускается в setup() как все задачи
//   - Ждёт CMD_OTA_START через TOPIC_CMD
//   - otaBeginUpdate() вызывается из protocol_task для инициализации Update
//   - OTA Task обрабатывает чанки из TOPIC_OTA_CHUNK_PACK
//   - CMD_OTA_END → Update.end() → ESP.restart()
//
// ВЕРСИЯ: 6.8.19 — OTA Task стартует в setup(), как все задачи
// -----------------------------------------------------------------------------

#ifndef OTA_TASK_H
#define OTA_TASK_H

#include <Arduino.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// =============================================================================
// Константы OTA
// =============================================================================

#define OTA_CHUNK_BIN_SIZE   1024   // Бинарные данные за чанк (байт)
#define OTA_CHUNK_B64_SIZE   1368   // Base64-представление 1024 байт (~1368 символа)
#define OTA_DECODE_BUF_SIZE  1536   // Буфер декодирования (с запасом)
#define OTA_MAX_CHUNKS       8192   // Максимум чанков (~2 МБ)

// =============================================================================
// OtaChunkPack — бинарный пакет данных для OTA-обновления
// =============================================================================

#pragma pack(push, 1)
typedef struct {
    uint16_t pack;                          // Номер чанка (1..2048)
    uint16_t crc16;                         // CRC16 от декодированных данных (0 = нет проверки)
    uint16_t b64_len;                       // Длина base64-строки (без \0)
    char     b64[OTA_CHUNK_B64_SIZE + 1];  // Base64-данные + нуль-терминатор

    bool isValid() const {
        return pack > 0 && b64_len > 0 && b64_len <= OTA_CHUNK_B64_SIZE && b64[b64_len] == '\0';
    }

#if DEBUG_LOG
    void print() const {
        char preview[12];
        size_t n = (b64_len < 10) ? b64_len : 10;
        memcpy(preview, b64, n);
        preview[n] = '\0';
        Serial.printf("[OtaChunk] pack=%u, crc=%04X, len=%u, preview=\"%s\"\n",
                      pack, crc16, b64_len, preview);
    }
#endif
} OtaChunkPack;
#pragma pack(pop)

static_assert(sizeof(OtaChunkPack) <= 1400, "OtaChunkPack too large!");

// =============================================================================
// Управление задачей OTA
// =============================================================================

// Вызывается в setup() — запускает задачу OTA
void otaTaskStart();

// Вызывается из protocol_task при ota_update — инициализирует Update
void otaBeginUpdate(size_t firmwareSize);

// Проверки состояния
bool otaIsInProgress();   // для loop() — блокировка рестартов
bool otaTaskIsRunning();
bool otaIsReady();        // OTA готова принимать чанки

#endif // OTA_TASK_H
