// -----------------------------------------------------------------------------
// ota_task.cpp
// OTA-обновление прошивки — специализированная задача.
//
// Жизненный цикл:
//   otaTaskStart(firmwareSize)  → создаёт задачу, инициализирует Update
//   OTA Task подписывается на TOPIC_OTA_CHUNK_PACK (OtaChunkPack — бинарный пакет)
//   Декодирует base64 → CRC16 verify → пишет во flash → публикует pack в TOPIC_OTA_RESULT
//   Protocol Task принимает результат → формирует JSON ack → Android
//   При ota_end (TOPIC_CMD) → Update.end() → ESP.restart()
//
// OTA Task получает типизированный OtaChunkPack (без JSON обёртки, без парсинга строк).
// Protocol Task извлёк bin из JSON, сформировал OtaChunkPack, передал OTA.
//
// ПАМЯТЬ: Перед Update.begin() останавливаются лишние задачи и освобождается куча.
//
// ВЕРСИЯ: 6.8.13 — OtaChunkPack: типизированный пакет + CRC16 verify
// -----------------------------------------------------------------------------

#include "ota_task.h"
#include "data_router.h"
#include "topics.h"
#include "commands.h"
#include "app_config.h"
#include <Update.h>

// =============================================================================
// extern — остановка задач на время OTA
// =============================================================================

extern void simulatorStop();
extern void calculatorStop();
extern void klineStop();
extern void climateStop();
extern void oledStop();

// =============================================================================
// CRC16 — полином 0xA001 (CRC-16-IBM), стандарт для embedded
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
static QueueHandle_t cmdQ       = NULL;
static QueueHandle_t chunkPackQ = NULL;  // Бинарные пакеты OtaChunkPack

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
// logHeapState — диагностика состояния кучи
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

    // Валидация пакета
    if (!pack->isValid()) {
        Serial.printf("[OTA] Invalid pack: pack=%u, len=%u\n",
                      pack->pack, pack->b64_len);
        publishOtaResult(-pack->pack);
        return;
    }

    // Проверка последовательности
    if (pack->pack != (uint16_t)expectedPack) {
        if (pack->pack < (uint16_t)expectedPack) {
            // Дубликат — подтверждаем (Android мог не получить ack)
            Serial.printf("[OTA] Duplicate pack %u (expected %u), re-ack\n",
                          pack->pack, expectedPack);
            publishOtaResult(pack->pack);
            return;
        }
        // Будущий пакет — всё равно обрабатываем (OTA не ждёт retransmit)
        Serial.printf("[OTA] Out-of-order pack %u (expected %u)\n",
                      pack->pack, expectedPack);
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

    // Публикуем успеш — Protocol Task сформирует JSON ack
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
// otaTask — основная задача OTA
// =============================================================================

void otaTask(void* parameter) {
    lastHeartbeat = millis();
    otaRunning = true;

    DataRouter& dr = DataRouter::getInstance();

    // Подписка на команды (ota_end)
    cmdQ = xQueueCreate(3, sizeof(uint8_t));
    dr.subscribe(TOPIC_CMD, cmdQ, QueuePolicy::FIFO_DROP);

    // Подписка на бинарные пакеты OTA (OtaChunkPack)
    chunkPackQ = xQueueCreate(3, sizeof(OtaChunkPack));  // Буфер из 3 чанков на случай всплеска
    dr.subscribe(TOPIC_OTA_CHUNK_PACK, chunkPackQ, QueuePolicy::FIFO_DROP);

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

        // --- Бинарные пакеты OTA (OtaChunkPack) ---
        if (chunkPackQ) {
            OtaChunkPack otaPack;
            while (xQueueReceive(chunkPackQ, &otaPack, 0) == pdTRUE) {
                processChunkPack(&otaPack);
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

    // =========================================================================
    // Освобождение памяти перед Update.begin()
    // Update.begin(firmwareSize) требует contiguous блок ~1.18 МБ.
    // Останавливаем задачи, которые НЕ нужны во время OTA:
    //   - Simulator/RealEngine (физика автомобиля)
    //   - Calculator (расчёт TripPack)
    //   - K-Line (диагностика)
    //   - Climate (климат)
    //   - OLED (дисплей)
    // =========================================================================

    Serial.println("[OTA] Freeing memory before Update.begin()...");
    logHeapState("before_stop");

#if REAL_ENGINE_ENABLED
    extern void realEngineStop();
    realEngineStop();
#else
    simulatorStop();
#endif
    calculatorStop();
    klineStop();
    climateStop();
#if OLED_ENABLED
    oledStop();
#endif

    // Небольшая задержка чтобы задачи освободили ресурсы
    delay(100);
    logHeapState("after_stop");

    // Очистка очередей телеметрии от накопленных данных
    // Protocol Task может иметь накопленные пакеты в очередях подписок
    DataRouter& dr = DataRouter::getInstance();
    dr.drainTopic(TOPIC_ENGINE_PACK);
    dr.drainTopic(TOPIC_TRIP_PACK);
    dr.drainTopic(TOPIC_KLINE_PACK);
    dr.drainTopic(TOPIC_CLIMATE_PACK);
    dr.drainTopic(TOPIC_SETTINGS_PACK);
    dr.drainTopic(TOPIC_MSG_INCOMING);
    dr.drainTopic(TOPIC_MSG_OUTGOING);
    dr.drainTopic(TOPIC_OTA_CHUNK_PACK);

    logHeapState("after_drain");

    // Инициализируем Update
    if (!Update.begin(firmwareSize)) {
        Serial.println("[OTA] Update.begin() FAILED!");
        Update.printError(Serial);
        logHeapState("update_begin_failed");
        Serial.printf("[OTA] Required: %u bytes, Max alloc: %u bytes\n",
                      (unsigned)firmwareSize, (unsigned)ESP.getMaxAllocHeap());
        return;
    }

    Serial.printf("[OTA] Update initialized. Ready for %d chunks.\n", totalChunks);
    logHeapState("update_begin_ok");

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
