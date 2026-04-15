// -----------------------------------------------------------------------------
// ota_task.cpp
// OTA-обновление прошивки — полноправная задача FreeRTOS.
//
// Архитектура:
//   - Запускается в setup() как все задачи (simulator, calculator...)
//   - Подписывается на TOPIC_CMD и TOPIC_OTA_CHUNK_PACK
//   - В обычном режиме — просто висит, не мешает
//   - По CMD_OTA_START: останавливает телеметрию, Update.begin, обрабатывает чанки
//   - По CMD_OTA_END: Update.end() → ESP.restart()
//
// ВЕРСИЯ: 6.8.19 — OTA Task стартует в setup(), как все задачи
// -----------------------------------------------------------------------------

#include "ota_task.h"
#include "data_router.h"
#include "topics.h"
#include "commands.h"
#include "app_config.h"
#include "task_common.h"
#include <Update.h>

// =============================================================================
// CRC16 — полином 0xA001 (CRC-16-IBM)
// =============================================================================

static uint16_t crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

// =============================================================================
// Base64-декодер
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

static TaskHandle_t  otaTaskHandle   = NULL;
static bool          isRunningFlag   = false;
static unsigned long lastHeartbeat   = 0;
static bool          otaActive       = false;  // true = OTA в процессе записи
static size_t        otaFirmwareSize = 0;
static size_t        otaWritten     = 0;
static uint16_t      expectedPack   = 1;
static unsigned long lastChunkTime  = 0;
#define OTA_TIMEOUT_MS 30000  // 30 сек без данных — Android отвалился

static uint8_t       decodeBuf[OTA_DECODE_BUF_SIZE];
static QueueHandle_t cmdQ       = NULL;
static QueueHandle_t chunkPackQ = NULL;

// =============================================================================
// publishOtaResult — публикация pack номера в шину (для Protocol Task)
// =============================================================================

static void publishOtaResult(int pack) {
    DataRouter::getInstance().publish(TOPIC_OTA_RESULT, pack);
}

// =============================================================================
// otaIsInProgress — проверяется loop() для блокировки рестартов
// =============================================================================

bool otaIsInProgress() {
    return otaActive;
}

// =============================================================================
// logHeapState — диагностика
// =============================================================================

static void logHeapState(const char* label) {
    Serial.printf("[OTA] HEAP [%s]: free=%u, min_free=%u, max_alloc=%u\n",
                  label,
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMinFreeHeap(),
                  (unsigned)ESP.getMaxAllocHeap());
}

// =============================================================================
// processChunkPack — обработка одного бинарного пакета OTA
// =============================================================================

static void processChunkPack(const OtaChunkPack* pack) {
    if (!pack) {
        Serial.println("[OTA] NULL pack received");
        publishOtaResult(-1);
        return;
    }

    // Валидация
    if (!pack->isValid()) {
        Serial.printf("[OTA] Invalid pack: pack=%u, len=%u\n",
                      pack->pack, pack->b64_len);
        publishOtaResult(-pack->pack);
        return;
    }

    // Проверка последовательности
    if (pack->pack != expectedPack) {
        if (pack->pack < expectedPack) {
            Serial.printf("[OTA] Duplicate pack %u (expected %u), re-ack\n",
                          pack->pack, expectedPack);
            publishOtaResult(pack->pack);
            return;
        }
        uint16_t gap = pack->pack - expectedPack;
        if (gap > 2) {
            Serial.printf("[OTA] Gap detected: expected %u, got %u (gap=%u)\n",
                          expectedPack, pack->pack, gap);
        }
    }

    // Декодируем base64
    int decodedLen = b64Decode(pack->b64, decodeBuf);
    if (decodedLen <= 0) {
        Serial.printf("[OTA] Base64 decode failed for pack %u\n", pack->pack);
        publishOtaResult(-pack->pack);
        return;
    }

    // Проверяем CRC16 если он был передан
    if (pack->crc16 != 0) {
        uint16_t calcCrc = crc16(decodeBuf, decodedLen);
        if (calcCrc != pack->crc16) {
            Serial.printf("[OTA] CRC16 mismatch pack %u: %04X vs %04X\n",
                          pack->pack, calcCrc, pack->crc16);
            publishOtaResult(-pack->pack);
            return;
        }
    }

    // Пишем во flash
    size_t written = Update.write(decodeBuf, decodedLen);
    if (written != (size_t)decodedLen) {
        Serial.printf("[OTA] Flash write failed at pack %u\n", pack->pack);
        publishOtaResult(-pack->pack);
        return;
    }

    otaWritten += written;
    expectedPack = pack->pack + 1;
    lastChunkTime = millis();

    publishOtaResult(pack->pack);

    // Лог прогресса каждые 10%
    int progress = (int)((otaWritten * 100) / otaFirmwareSize);
    static int lastLog = 0;
    if (progress - lastLog >= 10) {
        lastLog = progress;
        Serial.printf("[OTA] Progress: %d%% (%u/%u bytes), pack %u\n",
                      progress, (unsigned)otaWritten, (unsigned)otaFirmwareSize, pack->pack);
    }
}

// =============================================================================
// otaTask — Главная задача OTA
// =============================================================================

void otaTask(void* parameter) {
    (void)parameter;

    TaskContext ctx;
    if (!taskInit(&ctx, "OTA", &isRunningFlag, &lastHeartbeat)) return;

    // Очередь чанков создаётся позже — в otaBeginUpdate()
    chunkPackQ = NULL;

    Serial.println("[OTA] Task started, waiting for update...");

    while (1) {
        taskHeartbeat(&ctx);

        // Обработка команд
        if (cmdQ) {
            uint8_t cmd;
            while (xQueueReceive(cmdQ, &cmd, 0) == pdTRUE) {
                if ((Command)cmd == CMD_OTA_END) {
                    // Android сказал "всё, завершай" — вызываем Update.end()
                    // независимо от таймаута. Если что-то не так — Update.end() сам вернёт ошибку.
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
                // CMD_OTA_START — игнорируем (для других задач), OTA уже запущена
            }
        }

                // Чанки данных (только когда OTA активна)
        if (otaActive && chunkPackQ) {
            static OtaChunkPack otaPack; // static чтобы не нагружать стек задачи
            while (xQueueReceive(chunkPackQ, &otaPack, 0) == pdTRUE) {
                processChunkPack(&otaPack);
            }
        }


        // Таймаут
        if (otaActive && (millis() - lastChunkTime > OTA_TIMEOUT_MS)) {
            Serial.println("[OTA] TIMEOUT, aborting");
            Update.abort();
            otaActive = false;
            publishOtaResult(-998);
        }

        vTaskDelay(otaActive ? 10 : 100 / portTICK_PERIOD_MS);
    }
}

// =============================================================================
// otaTaskStart — вызывается из setup(), как все задачи
// =============================================================================

void otaTaskStart() {
    if (!otaTaskHandle) {
        lastHeartbeat = millis();
        isRunningFlag = true;
        // Ядро 1 — OTA (тот же приоритет что и Protocol)
        xTaskCreatePinnedToCore(otaTask, "OTA", TASK_STACK_OTA, NULL, 3, &otaTaskHandle, 1);
        Serial.println("[OTA] Started (TASK_STACK_OTA, P3, Core 1)");
    }
}


// =============================================================================
// otaTaskStop — остановка задачи
// =============================================================================

void otaTaskStop() {
    if (otaTaskHandle) {
        vTaskDelete(otaTaskHandle);
        otaTaskHandle = NULL;
        isRunningFlag = false;
    }
}

// =============================================================================
// otaTaskIsRunning — проверка активности
// =============================================================================

bool otaTaskIsRunning() {
    return isRunningFlag && (millis() - lastHeartbeat) < 5000;
}

// =============================================================================
// otaBeginUpdate — вызывается protocol_task при ota_update
// Устанавливает параметры и активирует OTA
// =============================================================================

void otaBeginUpdate(size_t firmwareSize) {
    // Если предыдущая OTA ещё активна — принудительно завершаем
    if (otaActive) {
        Serial.println("[OTA] Previous OTA still active, aborting...");
        Update.abort();
        otaActive = false;
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    // Запускаем OTA Task лениво — только когда действительно нужна OTA
    // Это экономит ~8 KB RAM на старте (стековый буфер задачи)
        if (!otaTaskHandle) {
        lastHeartbeat = millis();
        isRunningFlag = true;
        chunkPackQ = NULL;
        xTaskCreatePinnedToCore(otaTask, "OTA", TASK_STACK_OTA, NULL, 3, &otaTaskHandle, 1);
        // Даём задаче время инициализироваться

        vTaskDelay(50 / portTICK_PERIOD_MS);
    }

    otaFirmwareSize = firmwareSize;
    otaWritten = 0;
    expectedPack = 1;
    lastChunkTime = millis();

    // Создаём очередь чанков — QUEUE_OVERWRITE, depth=1
    DataRouter& dr = DataRouter::getInstance();
    if (chunkPackQ) {
        vQueueDelete(chunkPackQ);
        chunkPackQ = NULL;
    }
    chunkPackQ = xQueueCreate(1, sizeof(OtaChunkPack));
    if (!chunkPackQ) {
        Serial.println("[OTA] ERROR: Failed to create chunk queue in otaBeginUpdate!");
        return;
    }
    dr.subscribe(TOPIC_OTA_CHUNK_PACK, chunkPackQ, QueuePolicy::FIFO_DROP);

    // Drain очередей от старых данных
    dr.drainTopic(TOPIC_OTA_CHUNK_PACK);

    logHeapState("before");

    if (!Update.begin(firmwareSize)) {
        Serial.println("[OTA] Update.begin() FAILED!");
        Update.printError(Serial);
        logHeapState("update_begin_failed");
        return;
    }

    otaActive = true;
    logHeapState("update_begin_ok");
    Serial.printf("[OTA] Update initialized. Ready for %d chunks.\n",
                  (firmwareSize + OTA_CHUNK_BIN_SIZE - 1) / OTA_CHUNK_BIN_SIZE);
}

// =============================================================================
// otaIsReady — OTA Task готова принять чанки
// =============================================================================

bool otaIsReady() {
    return otaActive;
}
