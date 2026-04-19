// -----------------------------------------------------------------------------
// kline_task.cpp
// K-Line диагностика (опрос ЭБУ, автоопределение протокола).
//
// Назначение:
// - Подписан на TOPIC_CMD (QueuePolicy::FIFO_DROP, depth=5) → kl_* команды
// - Публикует KlinePack в TOPIC_KLINE_PACK каждые 1000 мс
// - Режим симуляции: тестовые данные (температуры, DTC)
//
// ВАЖНО: K-Line НЕ формирует JSON! Он публикует KlinePack в свой топик.
// Protocol Task подписан, собирает SERVICE JSON.
//
// -----------------------------------------------------------------------------
// ПРАВИЛА ФАЙЛА:
// -----------------------------------------------------------------------------
// ✅ можно:
//   - Добавлять реальный K-Line опрос ЭБУ
//   - Добавлять новые PID
//   - Менять логику автоопределения
//
// ❌ нельзя:
//   - Блокировать >500 мс
//   - Публиковать KlinePack чаще 1000 мс
//   - Формировать JSON или публиковать в TOPIC_MSG_OUTGOING
//   - Оперировать msg_id / ack_id
//
// ВЕРСИЯ: Определяется в app_config.h (FW_VERSION_STR)
// -----------------------------------------------------------------------------

#include "kline_task.h"
#include "data_router.h"
#include "task_common.h"
#include "topics.h"
#include "packets.h"
#include "commands.h"
#include "app_config.h"
#include "debug.h"

// =============================================================================
// Глобальные переменные
// =============================================================================

static TaskHandle_t  taskHandle     = NULL;
static bool          isRunningFlag  = false;
static unsigned long lastHeartbeat  = 0;
static int           currentProtocol = 0;
static bool          isRealMode     = false;

// --- Контекст задачи (фреймворк task_common) ---
// Внедрение фреймворка task_common (v6.8.22+):
//   - Автоматическая обработка CMD_OTA_START с полной очисткой ресурсов
//   - Единый heartbeat для мониторинга в loop()
//   - Регистрация подписок для корректной отписки при завершении
static TaskContext ctx = {0};

// --- Очередь команд (создаётся модулем) ---
// Примечание: теперь очередь создаётся автоматически через taskInit()
static QueueHandle_t cmdQueue = NULL;

// --- Тестовые данные ---
static float    testCoolant   = 90.0f;
static float    testAtf       = 75.0f;
static int      testDtcCount  = 2;
static char     testDtcCodes[64] = "P0135;P0141";
static unsigned long lastTestUpdate = 0;

// =============================================================================
// updateTestData
// =============================================================================
static void updateTestData() {
    unsigned long now = millis();
    if (now - lastTestUpdate < 1000) return;
    lastTestUpdate = now;
    testCoolant = 85.0f + (random(0, 20) / 10.0f);
    testAtf     = 70.0f + (random(0, 15) / 10.0f);
}

// =============================================================================
// klineCmdHandler — обработка специфичных команд модуля K-Line
// =============================================================================
// Вызывается из taskProcessCommands() для каждой полученной команды.
// CMD_OTA_START обрабатывается автоматически во фреймворке, сюда не попадает.
//
// Параметры:
//   cmd — код команды (enum Command)
//
// Возвращает:
//   true  — команда обработана
//   false — команда не распознана (будет залогировано фреймворком)
//
static bool klineCmdHandler(uint8_t cmd) {
    switch ((Command)cmd) {
        case CMD_KL_GET_DTC:
            DBG_PRINTLN("[KLine] DTC request received (will update KlinePack)");
            return true;

        case CMD_KL_CLEAR_DTC:
            testDtcCount = 0;
            memset(testDtcCodes, 0, sizeof(testDtcCodes));
            DBG_PRINTLN("[KLine] DTC cleared");
            return true;

        case CMD_KL_RESET_ADAPT:
            DBG_PRINTLN("[KLine] TCM adaptation reset requested");
            return true;

        case CMD_KL_PUMP_ATF:
            DBG_PRINTLN("[KLine] ATF pump requested");
            return true;

        case CMD_KL_DETECT_PROTO:
            currentProtocol = 1;
            DBG_PRINTLN("[KLine] Protocol detection requested");
            return true;

        case CMD_OTA_START:
            return true;  // Завершить задачу при OTA

        default:
            return false;  // Команда не распознана
    }
}

// =============================================================================
// klineTask — Главная задача
// =============================================================================
//
// Архитектура на основе task_common (обновлено в v6.8.22):
//   1. taskInit() — инициализация, создание cmdQ, подписка на TOPIC_CMD
//   2. Основной цикл:
//      - taskHeartbeat() — обновление счётчика активности
//      - taskProcessCommands() — обработка команд (включая OTA)
//      - Публикация KlinePack каждые 1000 мс
//
// При получении CMD_OTA_START:
//   - taskProcessCommands() вызывает taskShutdown()
//   - taskShutdown() отписывается от топиков, удаляет очереди, завершает задачу
//   - Память полностью освобождается для OTA-обновления
//
void klineTask(void* parameter) {
    (void)parameter;
    
    // === ИНИЦИАЛИЗАЦИЯ ЧЕРЕЗ ФРЕЙМВОРК ===
    // taskInit() заменяет ручное создание cmdQueue и подписку на TOPIC_CMD.
    // Все подписки, созданные модулем, должны быть зарегистрированы через
    // taskRegisterSubscription() для автоматической очистки при shutdown.
    if (!taskInit(&ctx, "KLine", &isRunningFlag, &lastHeartbeat)) {
        DBG_PRINTLN("[KLine] ERROR: taskInit failed!");
        isRunningFlag = false;
        vTaskDelete(NULL);
        return;
    }
    
    // Сохраняем указатель на cmdQueue для обратной совместимости
    // (используется в klineStop() для проверки)
    cmdQueue = ctx.cmdQ;
    
    DataRouter& router = DataRouter::getInstance();

    unsigned long lastPublish = 0;

    while (1) {
        // Heartbeat — обновление счётчика для loop()-мониторинга
        taskHeartbeat(&ctx);
        
        // Обработка команд (CMD_OTA_START обрабатывается автоматически)
        // При получении CMD_OTA_START эта функция НЕ ВОЗВРАЩАЕТСЯ
        taskProcessCommands(&ctx, klineCmdHandler);

        // Публикация KlinePack каждые 1000 мс
        unsigned long now = millis();
        if (now - lastPublish >= 1000) {
            lastPublish = now;
            updateTestData();

            KlinePack pack;
            memset(&pack, 0, sizeof(pack));
            pack.version           = 2;
            pack.coolant_temp      = testCoolant;
            pack.atf_temp          = testAtf;
            pack.tcc_lockup        = false;
            pack.selector_position = 3;    // D
            pack.current_gear      = 2;    // 2-я передача
            pack.voltage           = 14.0f + (random(0, 50) / 100.0f);
            pack.fuel_percent      = 55.0f + (random(0, 20) / 10.0f);
            pack.output_shaft_rpm  = 1500.0f + (random(0, 500));
            pack.dtc_count         = (uint8_t)testDtcCount;
            strlcpy(pack.dtc_codes, testDtcCodes, sizeof(pack.dtc_codes));

            router.publishPacket(TOPIC_KLINE_PACK, &pack, sizeof(pack));
        }

        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

// =============================================================================
// Управление
// =============================================================================

void klineStart() {
    if (!taskHandle) {
        // СРАЗУ — чтобы loop() не думал что crashed
        lastHeartbeat = millis();
        isRunningFlag = true;
        // Ядро 1 — K-Line (диагностика ЭБУ)
        xTaskCreatePinnedToCore(klineTask, "KLine", TASK_STACK_SIZE, NULL, TASK_PRIORITY_KLINE, &taskHandle, 1);
    }
}

void klineStop() {
    if (taskHandle) {
        vTaskDelete(taskHandle);
        taskHandle = NULL;
    }
    isRunningFlag = false;
    // НЕ удаляем очередь — при рестарте создастся новая, старая освободится кучей.
    // vQueueDelete при асинхронных рестартах вызывает гонки и assert pxQueue.
    // Примечание: при CMD_OTA_START очередь корректно удаляется через taskShutdown()
    cmdQueue = NULL;
#if DEBUG_LOG
    DBG_PRINTLN("[KLine] Stopped (safe shutdown)");
#endif
}

bool klineIsRunning() { return isRunningFlag && (millis() - lastHeartbeat) < 3000; }
bool klineIsConnected() { return !isRealMode; }

// Legacy-функции для обратной совместимости
void klineRequestDTC() {}
void klineClearDTC() {}
void klineResetTCMAdaptation() {}
void klineStartABSBleed() {}