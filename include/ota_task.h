// -----------------------------------------------------------------------------
// ota_task.h
// OTA-обновление прошивки — специализированная задача, поднимаемая только
// при получении команды ota_update от Android.
//
// Архитектура:
//   - OTA Task подписывается на TOPIC_OTA_CHUNK (OtaChunkPack — бинарные данные)
//     и на TOPIC_CMD (ota_end)
//   - Protocol Task декодирует base64 из JSON, публикует OtaChunkPack
//   - OTA Task пишет flash → публикует pack номер в TOPIC_OTA_STATUS
//   - Protocol Task принимает pack номер → формирует JSON ack → Android
//
// Протокол OTA:
//   1. Android → {"command":"ota_update","size":<firmware_size>}
//   2. Protocol → сохраняет версию, останавливает задачи, запускает OTA Task
//   3. Protocol → Android: {"ota_init":{"size":<chunk_size>,"count":<chunks>}}
//   4. Android → {"command":"ota_data","data":{"pack":N,"bin":"<base64>"}}
//   5. Protocol → base64_decode → OtaChunkPack → TOPIC_OTA_CHUNK
//   6. OTA Task → Update.write() → pack номер → TOPIC_OTA_STATUS
//   7. Protocol → JSON ack → Android
//   8. Android → {"command":"ota_end"}
//   9. OTA Task → Update.end() → ESP.restart()
//
// ВЕРСИЯ: 6.8.7 — OTA Task: бинарные данные (без JSON/base64)
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
#define OTA_MAX_CHUNKS       8192   // Максимум чанков (~2 МБ, достаточно для ESP32)

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
