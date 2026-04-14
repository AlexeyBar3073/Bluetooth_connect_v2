// -----------------------------------------------------------------------------
// ota_task.h
// OTA-обновление прошивки — специализированная задача, поднимаемая только
// при получении команды ota_update от Android.
//
// Архитектура:
//   - Protocol Task формирует OtaChunkPack → TOPIC_OTA_CHUNK_PACK
//   - OTA Task получает бинарный пакет, декодирует base64, пишет flash
//   - Результат публикуется в TOPIC_OTA_RESULT (pack номер)
//   - Protocol Task принимает результат → формирует JSON ack → Android
//
// Протокол OTA:
//   1. Android → {"command":"ota_update","size":<firmware_size>}
//   2. Protocol → останавливает телеметрию, запускает OTA Task
//   3. Protocol → Android: {"ota_init":{"size":<chunk_size>,"count":<chunks>}}
//   4. Android → {"command":"ota_data","data":{"pack":N,"bin":"<base64>","crc16":C}}
//   5. Protocol → OtaChunkPack → TOPIC_OTA_CHUNK_PACK → ack_id
//   6. OTA Task → base64_decode → CRC16 verify → Update.write() → TOPIC_OTA_RESULT
//   7. Protocol → JSON ack → Android
//   8. Android → {"command":"ota_end"}
//   9. OTA Task → Update.end() → ESP.restart()
//
// ВЕРСИЯ: 6.8.13 — OtaChunkPack: типизированный пакет вместо строки
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
// Формат: [pack:2][crc16:2][b64_len:2][b64_data:N]
// =============================================================================
//
// Назначение:
//   Типизированный пакет для передачи OTA-данных через DataRouter.
//   Заменяет строковый TOPIC_OTA_CHUNK — нет сериализации, нет промежуточных буферов.
//   Прямой memcpy в очередь подписчика (~30-50% эффективнее publishString).
//
// Поля:
//   pack   — Номер чанка (1..2048), используется для проверки последовательности
//   crc16  — CRC16 от декодированных bin-данных (0 = Android не отправил)
//   b64_len — Длина base64-строки (без \0)
//   b64    — Base64-данные + нуль-терминатор для удобства отладки
//
// Размер: 2+2+2+1369 = 1375 байт
//
#pragma pack(push, 1)
typedef struct {
    uint16_t pack;                          // Номер чанка (1..2048)
    uint16_t crc16;                         // CRC16 от декодированных данных (0 = нет проверки)
    uint16_t b64_len;                       // Длина base64-строки (без \0)
    char     b64[OTA_CHUNK_B64_SIZE + 1];  // Base64-данные + нуль-терминатор

    // Валидация: проверяет что длина соответствует реальной строке
    bool isValid() const {
        return pack > 0 && b64_len > 0 && b64_len <= OTA_CHUNK_B64_SIZE && b64[b64_len] == '\0';
    }
} OtaChunkPack;
#pragma pack(pop)

static_assert(sizeof(OtaChunkPack) <= 1400, "OtaChunkPack too large!");

// =============================================================================
// Состояние OTA (глобальное, проверяется loop() и Protocol)
// =============================================================================

bool otaIsInProgress();

// =============================================================================
// OTA Task — задача обновления (поднимается/снимается динамически)
// =============================================================================

void otaTask(void* parameter);
void otaTaskStart(size_t firmwareSize, int ackId);
void otaTaskStop();
bool otaTaskIsRunning();

#endif // OTA_TASK_H
