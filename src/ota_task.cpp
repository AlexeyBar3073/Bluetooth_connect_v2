// -----------------------------------------------------------------------------
// ota_task.cpp
// OTA-обновление прошивки — специализированная задача.
//
// Жизненный цикл:
//   otaTaskStart(firmwareSize)  → создаёт задачу, инициализирует Update
//   OTA Task подписывается на TOPIC_OTA_CHUNK (string — {"pack":N,"bin":"<base64>"})
//   Декодирует base64 → пишет во flash → публикует pack номер в TOPIC_OTA_RESULT
//   Protocol Task принимает результат → формирует JSON ack → Android
//   При ota_end (TOPIC_CMD) → Update.end() → ESP.restart()
//   При ошибке → публикует отрицательный pack в TOPIC_OTA_RESULT
//
// OTA Task САМ декодирует base64. Protocol только маршрутизирует.
//
// ВЕРСИЯ: 6.8.8 — OTA: base64_decode в OTA Task (не в Protocol)
// -----------------------------------------------------------------------------

#include "ota_task.h"
#include "data_router.h"
#include "topics.h"
#include "commands.h"
#include "app_config.h"
#include <Update.h>
#include <ArduinoJson.h>

// =============================================================================
// Base64-декодер (OTA Task сам декодирует)
// =============================================================================

static const char b64Table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

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
// Глобальные переменные OTA
// =============================================================================

static TaskHandle_t  otaTaskHandle  = NULL;
static bool          otaRunning     = false;
static unsigned long lastHeartbeat  = 0;
static size_t        otaFirmwareSize = 0;
static size_t        otaWritten     = 0;
static int           expectedPack   = 1;     // Ожидаемый номер пакета (начинаем с 1)
static int           totalChunks    = 0;
static uint8_t       decodeBuf[OTA_DECODE_BUF_SIZE];

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
// publishOtaResult — публикация pack номера в шину (для Protocol Task)
//   pack > 0  — чанк успешно записан
//   pack < 0  — ошибка (абсолютное значение = pack номер)
// =============================================================================

static void publishOtaResult(int pack) {
    DataRouter::getInstance().publish(TOPIC_OTA_RESULT, pack);
}

// =============================================================================
// processChunk — обработка одного чанка (base64 строка JSON)
// =============================================================================

static void processChunk(const char* jsonStr) {
    JsonDocument doc;
    if (deserializeJson(doc, jsonStr)) {
        Serial.printf("[OTA] Invalid chunk JSON\n");
        publishOtaResult(-1);
        return;
    }

    int pack = doc["pack"];
    const char* b64 = doc["bin"];

    if (pack <= 0 || !b64) {
        Serial.printf("[OTA] Missing pack or bin\n");
        publishOtaResult(-1);
        return;
    }

    // Проверка последовательности
    if (pack != expectedPack) {
        Serial.printf("[OTA] Wrong pack: expected %d, got %d — DISCARDED\n", expectedPack, pack);
        // Повтор предыдущего — Android уже получил ack, пусть двигается дальше
        publishOtaResult(expectedPack - 1);
        return;
    }

    // Декодируем base64
    int decodedLen = b64Decode(b64, decodeBuf);
    if (decodedLen <= 0) {
        Serial.printf("[OTA] Base64 decode failed for pack %d\n", pack);
        publishOtaResult(-pack);
        return;
    }

    // Пишем во flash
    size_t written = Update.write(decodeBuf, decodedLen);
    if (written != (size_t)decodedLen) {
        Serial.printf("[OTA] Flash write failed at pack %d\n", pack);
        publishOtaResult(-pack);
        return;
    }

    otaWritten += written;
    expectedPack++;

    // Публикуем успеш — Protocol Task сформирует JSON ack
    publishOtaResult(pack);

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

    // Подписка на чанки данных (string — {"pack":N,"bin":"<base64>"})
    chunkQ = xQueueCreate(1, OTA_DECODE_BUF_SIZE + 128);
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
                        publishOtaResult(-1);
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

        // --- Чанки данных (string JSON) ---
        if (chunkQ) {
            char chunkBuf[OTA_DECODE_BUF_SIZE + 128];
            while (xQueueReceive(chunkQ, chunkBuf, 0) == pdTRUE) {
                processChunk(chunkBuf);
            }
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

// =============================================================================
// otaTaskStart — инициализация и запуск OTA Task
// =============================================================================

void otaTaskStart(size_t firmwareSize, int ackId) {
    // Если OTA уже запущена — повторяем сигнал ota_init
    if (otaTaskHandle) {
        // -1 = маркер ota_init для Protocol
        publishOtaResult(-1);
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

    // Сигнал Protocol Task что OTA готова (он сформирует ota_init JSON)
    publishOtaResult(-1);

    // Ядро 1 (то же, что и Protocol)
    xTaskCreatePinnedToCore(otaTask, "OTA", 4096, NULL, 3, &otaTaskHandle, 1);
}

// =============================================================================
// otaTaskStop — остановка задачи
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
