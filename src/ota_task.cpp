// -----------------------------------------------------------------------------
// ota_task.cpp
// OTA-обновление прошивки — специализированная задача.
//
// Жизненный цикл:
//   otaTaskStart(firmwareSize)  → создаёт задачу, инициализирует Update
//   OTA Task подписывается на TOPIC_OTA_CHUNK (OtaChunkPack — бинарные данные)
//   Принимает чанки → пишет во flash → публикует pack номер в TOPIC_OTA_STATUS
//   Protocol Task принимает pack номер → формирует JSON ack → Android
//   При ota_end (TOPIC_CMD) → Update.end() → ESP.restart()
//   При ошибке → публикует отрицательный pack в TOPIC_OTA_STATUS
//
// Архитектура: OTA Task НЕ работает с JSON. Только бинарные данные → flash.
//
// ВЕРСИЯ: 6.8.7 — OTA: бинарные данные через DataRouter (без JSON/base64)
// -----------------------------------------------------------------------------

#include "ota_task.h"
#include "data_router.h"
#include "topics.h"
#include "commands.h"
#include "app_config.h"
#include "packets.h"
#include <Update.h>
#include <ArduinoJson.h>

// =============================================================================
// Глобальные переменные OTA
// =============================================================================

static TaskHandle_t  otaTaskHandle  = NULL;
static bool          otaRunning     = false;
static unsigned long lastHeartbeat  = 0;
static size_t        otaFirmwareSize = 0;
static size_t        otaWritten     = 0;
static int           expectedPack   = 1;     // Ожидаемый номер пакета (начинаем с 1)
static int           totalChunks    = 0;

// Очереди (создаются внутри задачи, регистрируются в DataRouter)
static QueueHandle_t cmdQ      = NULL;
static QueueHandle_t chunkQ    = NULL;

// =============================================================================
// otaIsInProgress — проверяется loop() для блокировки рестартов
// =============================================================================

bool otaIsInProgress() {
    return otaRunning;
}

// =============================================================================
// publishOtaStatus — публикация pack номера в шину (для Protocol Task)
//   pack > 0  — чанк успешно записан
//   pack < 0  — ошибка (абсолютное значение = pack номер)
// =============================================================================

static void publishOtaStatus(int pack) {
    DataRouter::getInstance().publish(TOPIC_OTA_STATUS, pack);
}

// =============================================================================
// processChunk — обработка одного чанка (бинарные данные из OtaChunkPack)
// =============================================================================

static void processChunk(const OtaChunkPack* pack) {
    if (!pack) return;

    // Проверка последовательности
    if (pack->pack != expectedPack) {
        Serial.printf("[OTA] Wrong pack: expected %d, got %d — DISCARDED\n", expectedPack, pack->pack);
        // Повтор предыдущего — отправляем ack чтобы Android двинулся дальше
        publishOtaStatus(expectedPack - 1);
        return;
    }

    // Пишем во flash (Update.write требует non-const)
    uint8_t* dataPtr = const_cast<uint8_t*>(pack->data);
    size_t written = Update.write(dataPtr, pack->dataLen);
    if (written != pack->dataLen) {
        Serial.printf("[OTA] Flash write failed at pack %d\n", pack->pack);
        publishOtaStatus(-pack->pack);  // отрицательный = ошибка
        return;
    }

    otaWritten += written;
    expectedPack++;

    // Публикуем успеш — Protocol Task сформирует JSON ack
    publishOtaStatus(pack->pack);

    // Лог прогресса каждые 10%
    int progress = (int)((otaWritten * 100) / otaFirmwareSize);
    static int lastLog = 0;
    if (progress - lastLog >= 10) {
        lastLog = progress;
        Serial.printf("[OTA] Progress: %d%% (%u/%u bytes)\n",
                      progress, (unsigned)otaWritten, (unsigned)otaFirmwareSize);
    }
}

// =============================================================================
// otaTask — основная задача OTA
// =============================================================================

void otaTask(void* parameter) {
    lastHeartbeat = millis();
    otaRunning = true;

    DataRouter& dr = DataRouter::getInstance();

    // Подписка на команды (ota_end)
    cmdQ = xQueueCreate(3, sizeof(uint8_t));
    dr.subscribe(TOPIC_CMD, cmdQ, QueuePolicy::FIFO_DROP);

    // Подписка на чанки данных (бинарные OtaChunkPack)
    chunkQ = xQueueCreate(1, sizeof(OtaChunkPack));
    dr.subscribe(TOPIC_OTA_CHUNK, chunkQ, QueuePolicy::FIFO_DROP);

    Serial.printf("[OTA] Task started. Size=%u bytes, chunks=%d\n",
                  (unsigned)otaFirmwareSize, totalChunks);

    while (1) {
        lastHeartbeat = millis();

        // --- Команды (ota_end) ---
        if (cmdQ) {
            uint8_t cmd;
            while (xQueueReceive(cmdQ, &cmd, 0) == pdTRUE) {
                if ((Command)cmd == CMD_OTA_END) {
                    // Финализация
                    Serial.println("[OTA] CMD_OTA_END received, finalizing...");
                    if (!Update.end(true)) {
                        Serial.println("[OTA] Update.end() FAILED!");
                        publishOtaStatus(-1);
                    } else {
                        Serial.printf("[OTA] Complete! %u bytes written.\n",
                                      (unsigned)otaWritten);
                        Serial.println("[OTA] Rebooting...");
                        delay(500);
                        ESP.restart();
                    }
                }
            }
        }

        // --- Чанки данных (бинарные OtaChunkPack) ---
        if (chunkQ) {
            OtaChunkPack chunk;
            while (xQueueReceive(chunkQ, &chunk, 0) == pdTRUE) {
                processChunk(&chunk);
            }
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

// =============================================================================
// otaTaskStart — инициализация и запуск OTA Task
// =============================================================================

void otaTaskStart(size_t firmwareSize, int ackId) {
    // Если OTA уже запущена — повторяем pack=-1 (ota_init сигнал для Protocol)
    if (otaTaskHandle) {
        publishOtaStatus(-1);  // маркер ota_init
        return;
    }

    otaFirmwareSize = firmwareSize;
    otaWritten = 0;
    expectedPack = 1;
    totalChunks = (firmwareSize + OTA_CHUNK_BIN_SIZE - 1) / OTA_CHUNK_BIN_SIZE;

    if (totalChunks > OTA_MAX_CHUNKS) {
        Serial.printf("[OTA] Firmware too large: %u bytes (max ~%d KB)\n",
                      (unsigned)firmwareSize, OTA_MAX_CHUNKS * OTA_CHUNK_BIN_SIZE / 1024);
        return;
    }

    // Инициализируем Update
    if (!Update.begin(firmwareSize)) {
        Serial.println("[OTA] Update.begin() FAILED!");
        Update.printError(Serial);
        return;
    }

    Serial.printf("[OTA] Update initialized. Ready for %d chunks.\n", totalChunks);

    // Публикуем ota_init сигнал — Protocol Task сформирует JSON и отправит Android
    publishOtaStatus(-1);

    // Ядро 1 (то же, что и Protocol)
    xTaskCreatePinnedToCore(otaTask, "OTA", 4096, NULL, 3, &otaTaskHandle, 1);
}

// =============================================================================
// otaTaskStop — остановка задачи (вызывается при ошибке или завершении)
// =============================================================================

void otaTaskStop() {
    if (otaTaskHandle) {
        vTaskDelete(otaTaskHandle);
        otaTaskHandle = NULL;
    }
    otaRunning = false;
}

// =============================================================================
// otaTaskIsRunning — проверка активности задачи
// =============================================================================

bool otaTaskIsRunning() {
    return otaRunning && otaTaskHandle &&
           (millis() - lastHeartbeat) < 5000;
}
