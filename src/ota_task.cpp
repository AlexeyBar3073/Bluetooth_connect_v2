// -----------------------------------------------------------------------------
// ota_task.cpp
// OTA-обновление прошивки — полноправная задача FreeRTOS.
//
// Архитектура:
//   - Запускается лениво при вызове otaBeginUpdate() из protocol_task
//   - Подписывается на TOPIC_CMD и TOPIC_OTA_CHUNK_PACK
//   - В обычном режиме задача не создана (экономия RAM)
//   - По CMD_OTA_START: активирует otaActive и начинает обработку чанков
//   - Update.begin() вызывается при получении первого чанка (для оптимизации памяти)
//   - По CMD_OTA_END: Update.end() → ESP.restart()
//
// Обновление (интеграция с task_common):
// - Использует taskInit() для унифицированной инициализации
// - taskHeartbeat() обновляет счётчик для loop()-мониторинга
// - taskProcessCommands() обрабатывает команды
// - OTA Task НЕ завершается при CMD_OTA_START (активирует otaActive)
// - При CMD_OTA_END выполняет финализацию и перезагрузку
//
// ВАЖНО: OTA Task работает на Ядре 0 (вместе с Simulator/RealEngine)
//
// ВЕРСИЯ: Определяется в app_config.h (FW_VERSION_STR)
// -----------------------------------------------------------------------------

#include "ota_task.h"
#include "data_router.h"
#include "task_common.h"
#include "topics.h"
#include "commands.h"
#include "app_config.h"
#include "debug.h"
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

// --- Состояние OTA ---
static bool          otaActive       = false;  // true = OTA в процессе записи
static bool          updateInitialized = false; // true = Update.begin() уже вызван
static size_t        otaFirmwareSize = 0;
static size_t        otaWritten      = 0;
static uint16_t      expectedPack    = 1;
static unsigned long lastChunkTime   = 0;
#define OTA_TIMEOUT_MS 30000  // 30 сек без данных — Android отвалился

// --- Буферы и очереди ---
static uint8_t       decodeBuf[OTA_DECODE_BUF_SIZE];
static QueueHandle_t chunkPackQ = NULL;

// --- Контекст задачи (фреймворк task_common) ---
// Внедрение фреймворка task_common:
//   - Унифицированная инициализация через taskInit()
//   - Единый heartbeat для мониторинга в loop()
//   - Обработка команд через taskProcessCommands()
static TaskContext ctx = {0};

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
    DBG_PRINTF("[OTA] HEAP [%s]: free=%u, min_free=%u, max_alloc=%u",
                  label,
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMinFreeHeap(),
                  (unsigned)ESP.getMaxAllocHeap());
}

// =============================================================================
// initializeUpdate — инициализация Update.begin() при получении первого чанка
// =============================================================================

static bool initializeUpdate() {
    // Проверяем, не инициализирован ли уже Update
    if (updateInitialized) {
        return true;
    }

    logHeapState("before");

    // 1. Проверяем, сколько места в системном разделе для обновления
    size_t maxFreeAppSpace = ESP.getFreeSketchSpace();

    DBG_PRINTLN("--- OTA Debug Info ---");
    DBG_PRINTF("[OTA] Required size (from server): %u bytes", otaFirmwareSize);
    DBG_PRINTF("[OTA] Max available partition space: %u bytes", maxFreeAppSpace);
    DBG_PRINTF("[OTA] Current Free Heap: %u bytes", ESP.getFreeHeap());
    DBG_PRINTF("[OTA] Largest Free Block: %u bytes", ESP.getMaxAllocHeap());
    DBG_PRINTLN("-----------------------");

    if (otaFirmwareSize > maxFreeAppSpace) {
        DBG_PRINTF("[OTA] ERROR: Firmware is too large! Need %u, have %u", otaFirmwareSize, maxFreeAppSpace);
        return false;
    }

    if (!Update.begin(otaFirmwareSize)) {
        DBG_PRINTLN("[OTA] Update.begin() FAILED!");
        Update.printError(Serial);
        logHeapState("update_begin_failed");
        return false;
    }

    updateInitialized = true;
    otaWritten = 0;
    expectedPack = 1;
    lastChunkTime = millis();
    
    logHeapState("update_begin_ok");
    DBG_PRINTF("[OTA] Update initialized. Ready for %d chunks.",
                  (int)((otaFirmwareSize + OTA_CHUNK_BIN_SIZE - 1) / OTA_CHUNK_BIN_SIZE));
                  
    return true;
}

// =============================================================================
// processChunkPack — обработка одного бинарного пакета OTA
// =============================================================================

static void processChunkPack(const OtaChunkPack* pack) {
    if (!pack) {
        DBG_PRINTLN("[OTA] NULL pack received");
        publishOtaResult(-1);
        return;
    }

    // Валидация
    if (!pack->isValid()) {
        DBG_PRINTF("[OTA] Invalid pack: pack=%u, len=%u",
                      pack->pack, pack->b64_len);
        publishOtaResult(-pack->pack);
        return;
    }

    // При получении первого чанка инициализируем Update.begin()
    // Это важная оптимизация: Update.begin() вызывается только когда
    // все другие задачи уже остановились и освободили память
    if (!updateInitialized) {
        if (pack->pack != 1) {
            // Первый чанк должен иметь номер 1
            DBG_PRINTF("[OTA] First chunk must be pack 1, got %u", pack->pack);
            publishOtaResult(-pack->pack);
            return;
        }
        
        // Инициализируем Update при получении первого чанка
        if (!initializeUpdate()) {
            DBG_PRINTLN("[OTA] Failed to initialize Update");
            publishOtaResult(-pack->pack);
            return;
        }
    }

    // Проверка последовательности
    if (pack->pack != expectedPack) {
        if (pack->pack < expectedPack) {
            DBG_PRINTF("[OTA] Duplicate pack %u (expected %u), re-ack",
                          pack->pack, expectedPack);
            publishOtaResult(pack->pack);
            return;
        }
        uint16_t gap = pack->pack - expectedPack;
        if (gap > 2) {
            DBG_PRINTF("[OTA] Gap detected: expected %u, got %u (gap=%u)",
                          expectedPack, pack->pack, gap);
        }
    }

    // Декодируем base64
    int decodedLen = b64Decode(pack->b64, decodeBuf);
    if (decodedLen <= 0) {
        DBG_PRINTF("[OTA] Base64 decode failed for pack %u", pack->pack);
        publishOtaResult(-pack->pack);
        return;
    }

    // Проверяем CRC16 если он был передан
    if (pack->crc16 != 0) {
        uint16_t calcCrc = crc16(decodeBuf, decodedLen);
        if (calcCrc != pack->crc16) {
            DBG_PRINTF("[OTA] CRC16 mismatch pack %u: %04X vs %04X",
                          pack->pack, calcCrc, pack->crc16);
            publishOtaResult(-pack->pack);
            return;
        }
    }

    // Пишем во flash
    size_t written = Update.write(decodeBuf, decodedLen);
    if (written != (size_t)decodedLen) {
        DBG_PRINTF("[OTA] Flash write failed at pack %u", pack->pack);
        publishOtaResult(-pack->pack);
        return;
    }

    otaWritten += written;
    expectedPack = pack->pack + 1;
    lastChunkTime = millis();

    publishOtaResult(pack->pack);
}

// =============================================================================
// otaCmdHandler — обработка специфичных команд модуля OTA
// =============================================================================
// Вызывается из taskProcessCommands() для каждой полученной команды.
//
// ВАЖНО: OTA Task НЕ завершается при CMD_OTA_START!
// Вместо этого он активирует otaActive и начинает обработку чанков.
//
// Параметры:
//   cmd — код команды (enum Command)
//
// Возвращает:
//   true  — команда обработана
//   false — команда не распознана
//
static bool otaCmdHandler(uint8_t cmd) {
    switch ((Command)cmd) {
        case CMD_OTA_START:
            // OTA Task уже активен или будет активирован через otaBeginUpdate()
            // Здесь ничего не делаем, otaActive устанавливается в otaBeginUpdate()
            DBG_PRINTLN("[OTA] CMD_OTA_START received (OTA task already ready)");
            return true;
            
        case CMD_OTA_END:
            // Android сказал "всё, завершай" — вызываем Update.end()
            DBG_PRINTLN("[OTA] CMD_OTA_END received, finalizing...");
            if (!Update.end(true)) {
                DBG_PRINTLN("[OTA] Update.end() FAILED!");
                publishOtaResult(-1);
            } else {
                DBG_PRINTF("[OTA] Complete! %u bytes written.",
                              (unsigned)otaWritten);
                DBG_PRINTLN("[OTA] Rebooting...");
                delay(500);
                ESP.restart();
            }
            return true;
            
        default:
            return false;  // Команда не распознана
    }
}

// =============================================================================
// otaTask — Главная задача OTA
// =============================================================================
//
// Архитектура на основе task_common:
//   1. taskInit() — инициализация, создание cmdQ, подписка на TOPIC_CMD
//   2. Основной цикл:
//      - taskHeartbeat() — обновление счётчика активности
//      - taskProcessCommands() — обработка команд
//      - Обработка чанков (только когда otaActive = true)
//      - Проверка таймаута OTA
//
// Ядро: 0 (вместе с Simulator/RealEngine)
//
void otaTask(void* parameter) {
    (void)parameter;

    // === ИНИЦИАЛИЗАЦИЯ ЧЕРЕЗ ФРЕЙМВОРК ===
    if (!taskInit(&ctx, "OTA", &isRunningFlag, &lastHeartbeat)) {
        DBG_PRINTLN("[OTA] ERROR: taskInit failed!");
        isRunningFlag = false;
        vTaskDelete(NULL);
        return;
    }

    // Очередь чанков создаётся позже — в otaBeginUpdate()
    chunkPackQ = NULL;

    DBG_PRINTLN("[OTA] Task started (Core 1, task_common framework), waiting for update...");

    while (1) {
        // Heartbeat — обновление счётчика для loop()-мониторинга
        taskHeartbeat(&ctx);

        // Обработка команд
        taskProcessCommands(&ctx, otaCmdHandler);

        // Чанки данных (только когда OTA активна)
        if (otaActive && chunkPackQ) {
            static OtaChunkPack otaPack; // static чтобы не нагружать стек задачи
            while (xQueueReceive(chunkPackQ, &otaPack, 0) == pdTRUE) {
                processChunkPack(&otaPack);
            }
        }

        // Таймаут
        if (otaActive && (millis() - lastChunkTime > OTA_TIMEOUT_MS)) {
            DBG_PRINTLN("[OTA] TIMEOUT, aborting");
            Update.abort();
            otaActive = false;
            updateInitialized = false; // Сбрасываем флаг инициализации
            publishOtaResult(-998);
        }

        vTaskDelay(otaActive ? 10 : 100 / portTICK_PERIOD_MS);
    }
}

// =============================================================================
// otaTaskStart — вызывается из setup() (опционально, для обратной совместимости)
// =============================================================================

void otaTaskStart() {
    if (!otaTaskHandle) {
        lastHeartbeat = millis();
        isRunningFlag = true;
        // Ядро 1 — OTA (вместе с Simulator/RealEngine)
        xTaskCreatePinnedToCore(otaTask, "OTA", TASK_STACK_OTA, NULL, 
                                TASK_PRIORITY_PROTOCOL, &otaTaskHandle, 1);
        DBG_PRINTLN("[OTA] Started (Core 1, task_common framework)");
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
        DBG_PRINTLN("[OTA] Stopped");
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
// =============================================================================
// Устанавливает параметры и активирует OTA.
// Update.begin() будет вызван при получении первого чанка (ленивая инициализация).
//
// Параметры:
//   firmwareSize — размер прошивки в байтах (получен от Android)
//
void otaBeginUpdate(size_t firmwareSize) {
    DBG_PRINTF("[OTA] otaBeginUpdate() CALLED, size=%u bytes", (unsigned)firmwareSize);
    
    // Если предыдущая OTA ещё активна — принудительно завершаем
    if (otaActive) {
        DBG_PRINTLN("[OTA] Previous OTA still active, aborting...");
        Update.abort();
        otaActive = false;
        updateInitialized = false;
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    // Запускаем OTA Task лениво — только когда действительно нужна OTA
    // Это экономит ~8 KB RAM на старте (стековый буфер задачи)
    if (!otaTaskHandle) {
        lastHeartbeat = millis();
        isRunningFlag = true;
        chunkPackQ = NULL;
        // Ядро 1 — OTA (вместе с Simulator/RealEngine)
        xTaskCreatePinnedToCore(otaTask, "OTA", TASK_STACK_OTA, NULL, 
                                TASK_PRIORITY_PROTOCOL, &otaTaskHandle, 1);
        // Даём задаче время инициализироваться
        vTaskDelay(50 / portTICK_PERIOD_MS);
        DBG_PRINTLN("[OTA] Task created lazily (Core 1)");
    }

    otaFirmwareSize = firmwareSize;
    otaWritten = 0;
    expectedPack = 1;
    lastChunkTime = millis();
    updateInitialized = false; // Сбрасываем флаг инициализации для новой OTA

    // Создаём очередь чанков — FIFO_DROP, depth=1
    DataRouter& dr = DataRouter::getInstance();
    if (chunkPackQ) {
        // ВАЖНО: Мы должны отписаться от топика перед удалением очереди
        // чтобы избежать утечек памяти
        dr.unsubscribe(TOPIC_OTA_CHUNK_PACK, chunkPackQ);
        vQueueDelete(chunkPackQ);
        chunkPackQ = NULL;
    }
    chunkPackQ = xQueueCreate(1, sizeof(OtaChunkPack));
    if (!chunkPackQ) {
        DBG_PRINTLN("[OTA] ERROR: Failed to create chunk queue in otaBeginUpdate!");
        return;
    }
    dr.subscribe(TOPIC_OTA_CHUNK_PACK, chunkPackQ, QueuePolicy::FIFO_DROP);

    // Drain очередей от старых данных
    dr.drainTopic(TOPIC_OTA_CHUNK_PACK);

    // Активируем OTA - теперь задача готова принимать чанки
    otaActive = true;
    DBG_PRINTF("[OTA] Ready to receive %d chunks (Update.begin() will be called on first chunk).",
                  (int)((firmwareSize + OTA_CHUNK_BIN_SIZE - 1) / OTA_CHUNK_BIN_SIZE));
}

// =============================================================================
// otaIsReady — OTA Task готова принять чанки
// =============================================================================

bool otaIsReady() {
    return otaActive;
}