// -----------------------------------------------------------------------------
// ota_task.h
// OTA-обновление прошивки — специализированная задача, поднимаемая только
// при получении команды ota_update от Android.
//
// Архитектура:
//   - OTA Task подписывается на TOPIC_OTA_CHUNK (string — base64 JSON)
//   - OTA Task САМ декодирует base64, пишет flash, публикует результат
//   - Protocol Task принимает результат → формирует JSON ack → Android
//
// Протокол OTA:
//   1. Android → {"command":"ota_update","size":<firmware_size>}
//   2. Protocol → останавливает телеметрию, запускает OTA Task
//   3. Protocol → Android: {"ota_init":{"size":<chunk_size>,"count":<chunks>}}
//   4. Android → {"command":"ota_data","data":{"pack":N,"bin":"<base64>"}}
//   5. Protocol → TOPIC_OTA_CHUNK (base64 строка) → ack_id
//   6. OTA Task → base64_decode → Update.write() → TOPIC_OTA_RESULT
//   7. Protocol → JSON ack → Android
//   8. Android → {"command":"ota_end"}
//   9. OTA Task → Update.end() → ESP.restart()
//
// ВЕРСИЯ: 6.8.8 — OTA Task: base64_decode внутри OTA (не в Protocol)
// -----------------------------------------------------------------------------

#ifndef OTA_TASK_H
#define OTA_TASK_H

#include <Arduino.h>
#include <stdbool.h>
#include <stddef.h>

// =============================================================================
// Константы OTA
// =============================================================================

#define OTA_CHUNK_BIN_SIZE   1024   // Бинарные данные за чанк (байт)
#define OTA_CHUNK_B64_SIZE   1368   // Base64-представление 1024 байт (~1368 символа)
#define OTA_DECODE_BUF_SIZE  1536   // Буфер декодирования (с запасом)
#define OTA_MAX_CHUNKS       8192   // Максимум чанков (~2 МБ)

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
