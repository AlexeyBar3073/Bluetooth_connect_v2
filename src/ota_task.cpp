// -----------------------------------------------------------------------------
// ota_task.cpp
// OTA-обновление прошивки — специализированная задача.
//
// Жизненный цикл:
//   otaTaskStart(firmwareSize)  → создаёт задачу, инициализирует Update
//   OTA Task подписывается на TOPIC_CMD и TOPIC_OTA_CHUNK
//   Принимает чанки → декодирует base64 → пишет во flash
//   При ota_end → Update.end() → ESP.restart()
//   При ошибке → отправка ota_error на Android
//
// ВЕРСИЯ: 6.6.0 — OTA Task (специфическая задача, msg_id/ack_id гарантия)
// -----------------------------------------------------------------------------

#include "ota_task.h"
#include "data_router.h"
#include "topics.h"
#include "commands.h"
#include "app_config.h"
#include "bt_transport.h"
#include <Update.h>
#include <ArduinoJson.h>

// =============================================================================
// Base64-декодер (без внешних зависимостей)
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
// sendOtaError — отправка сообщения об ошибке на Android
// =============================================================================

static void sendOtaError(const char* reason) {
    Serial.printf("[OTA] ERROR: %s\n", reason);
    JsonDocument doc;
    doc["ota_error"] = reason;
    char buf[128];
    serializeJson(doc, buf, sizeof(buf));
    btSend(buf);
}

// =============================================================================
// sendOtaInit — ответ Android с параметрами чанков + ack_id
// =============================================================================

static void sendOtaInit(int chunkSize, int count, int ackId) {
    Serial.printf("[OTA] INIT: chunkSize=%d bytes, count=%d chunks, ack_id=%d\n", chunkSize, count, ackId);
    JsonDocument doc;
    doc["ack_id"] = ackId;
    JsonObject init = doc["ota_init"].to<JsonObject>();
    init["size"]  = chunkSize;
    init["count"] = count;
    char buf[128];
    serializeJson(doc, buf, sizeof(buf));
    btSend(buf);
}

// =============================================================================
// processChunk — обработка одного чанка данных
// =============================================================================

static bool processChunk(const char* jsonStr) {
    JsonDocument doc;
    if (deserializeJson(doc, jsonStr)) {
        sendOtaError("invalid_chunk_json");
        return false;
    }

    int pack = doc["pack"];
    const char* b64 = doc["bin"];

    if (pack <= 0 || !b64) {
        sendOtaError("missing_pack_or_bin");
        return false;
    }

    // Проверка последовательности
    if (pack != expectedPack) {
        Serial.printf("[OTA] Wrong pack: expected %d, got %d — DISCARDED\n", expectedPack, pack);
        // Повтор предыдущего — отправляем ack чтобы Android двинулся дальше
        JsonDocument ack;
        ack["ota_read"] = expectedPack - 1;
        ack["ack_id"] = doc["ack_id"];
        char buf[64];
        serializeJson(ack, buf, sizeof(buf));
        btSend(buf);
        return true;
    }

    // Декодируем base64
    int decodedLen = b64Decode(b64, decodeBuf);
    if (decodedLen <= 0) {
        sendOtaError("base64_decode_failed");
        return false;
    }

    // Пишем во flash
    size_t written = Update.write(decodeBuf, decodedLen);
    if (written != (size_t)decodedLen) {
        sendOtaError("flash_write_failed");
        return false;
    }

    otaWritten += written;

    // Отправляем подтверждение Android (ack_id = msg_id входящего сообщения)
    JsonDocument ack;
    ack["ota_read"] = pack;
    ack["ack_id"] = doc["ack_id"];
    char buf[64];
    serializeJson(ack, buf, sizeof(buf));
    btSend(buf);

    expectedPack++;

    // Лог прогресса каждые 10%
    int progress = (int)((otaWritten * 100) / otaFirmwareSize);
    static int lastLog = 0;
    if (progress - lastLog >= 10) {
        lastLog = progress;
        Serial.printf("[OTA] Progress: %d%% (%u/%u bytes)\n",
                      progress, (unsigned)otaWritten, (unsigned)otaFirmwareSize);
    }

    return true;
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

    // Подписка на чанки данных
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
                        sendOtaError("update_end_failed");
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

        // --- Чанки данных ---
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
    // Если OTA уже запущена — повторно отправляем ack_id,
    // чтобы Android получил подтверждение (повторный запрос)
    if (otaTaskHandle) {
        sendOtaInit(OTA_CHUNK_BIN_SIZE, totalChunks, ackId);
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

    // Ответ Android — один пакет: ota_init + ack_id
    sendOtaInit(OTA_CHUNK_BIN_SIZE, totalChunks, ackId);

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
    // Update.aborton() не вызываем — если задача остановлена,
    // следующая попытка начнёт заново с Update.begin()
}

// =============================================================================
// otaTaskIsRunning — проверка активности задачи
// =============================================================================

bool otaTaskIsRunning() {
    return otaRunning && otaTaskHandle &&
           (millis() - lastHeartbeat) < 5000;
}
