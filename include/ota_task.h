// -----------------------------------------------------------------------------
// ota_task.h
// OTA-обновление прошивки — специализированная задача, поднимаемая только
// при получении команды ota_update от Android.
//
// Архитектура:
//   - OTA Task подписывается на TOPIC_CMD (ota_update, ota_end)
//     и на TOPIC_OTA_CHUNK (данные прошивки base64)
//   - Protocol Task проверяет последовательность пакетов и пересылает
//     данные в TOPIC_OTA_CHUNK только если pack == expectedPack
//   - При ошибке OTA Task шлёт ota_error в Protocol (через btSend)
//
// Протокол OTA:
//   1. Android → {"command":"ota_update","size":<firmware_size>}
//   2. Protocol → сохраняет версию, останавливает задачи, запускает OTA Task
//   3. Protocol → Android: {"ota_init":{"size":<chunk_size>,"count":<chunks>}}
//   4. Android → {"command":"ota_data","data":{"pack":N,"bin":"<base64>"}}
//   5. Protocol → TOPIC_OTA_CHUNK (если pack == expectedPack)
//   6. OTA Task → декодирует base64, пишет во flash
//   7. Android → {"command":"ota_end"}
//   8. OTA Task → Update.end() → ESP.restart()
//   9. При загрузке: Protocol сравнивает версию с сохранённой → уведомляет Android
//
// ВЕРСИЯ: 6.6.0 — OTA Task (специфическая задача, msg_id/ack_id гарантия)
// -----------------------------------------------------------------------------

#ifndef OTA_TASK_H
#define OTA_TASK_H

#include <Arduino.h>
#include <stdbool.h>
#include <stddef.h>

// =============================================================================
// Константы OTA
// =============================================================================

#define OTA_CHUNK_BIN_SIZE   256    // Бинарные данные за чанк (байт)
#define OTA_CHUNK_B64_SIZE   344    // Base64-представление 256 байт (~344 символа)
#define OTA_DECODE_BUF_SIZE  512    // Буфер декодирования (с запасом)
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
