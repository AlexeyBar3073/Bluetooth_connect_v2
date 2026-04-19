// -----------------------------------------------------------------------------
// climate.cpp
// Модуль климата и сервисных датчиков.
//
// Назначение:
// - Публикует ClimatePack в TOPIC_CLIMATE_PACK каждые 1000 мс
// - Режим симуляции: тестовые данные (температуры, давление шин, омывайка)
//
// Архитектура (фреймворк task_common):
// - Использует taskInit() для инициализации и подписки на TOPIC_CMD
// - Регистрирует подписки через taskRegisterSubscription()
// - taskProcessCommands() обрабатывает CMD_OTA_START с полной очисткой ресурсов
// - taskHeartbeat() обновляет счётчик для loop()-мониторинга
//
// ВАЖНО: Climate НЕ формирует JSON! Он публикует ClimatePack в свой топик.
// Protocol Task подписан на TOPIC_CLIMATE_PACK и собирает SERVICE JSON.
//
// ВЕРСИЯ: Определяется в app_config.h (FW_VERSION_STR)
// -----------------------------------------------------------------------------

#include "climate.h"
#include "data_router.h"
#include "task_common.h"
#include "topics.h"
#include "packets.h"
#include "commands.h"
#include "app_config.h"
#include "debug.h"

// =============================================================================
// Глобальные переменные модуля
// =============================================================================

static TaskHandle_t  taskHandle     = NULL;
static bool          isRunningFlag  = false;
static unsigned long lastHeartbeat  = 0;

// Контекст задачи (фреймворк task_common)
static TaskContext ctx = {0};

// Тестовые данные для симуляции
static float  testInterior = 22.0f;   // Температура в салоне (°C)
static float  testExterior = 15.0f;   // Температура за бортом (°C)
static bool   testTire = false;       // Низкое давление в шинах
static bool   testWash = false;       // Низкий уровень омывайки

static unsigned long lastTestUpdate = 0;  // Таймер обновления тестовых данных

// =============================================================================
// updateTestData — обновление тестовых данных каждые 5 секунд
// =============================================================================
// Имитирует изменение показаний датчиков для реалистичности симуляции.
// Вызывается перед публикацией ClimatePack.
//
static void updateTestData() {
    unsigned long now = millis();
    if (now - lastTestUpdate < 5000) {
        return;  // Обновляем раз в 5 секунд
    }
    lastTestUpdate = now;
    
    // Плавное изменение температур в реалистичных пределах
    testInterior = 20.0f + (random(0, 50) / 10.0f);   // 20.0 - 24.9 °C
    testExterior = 10.0f + (random(0, 80) / 10.0f);   // 10.0 - 17.9 °C
    
    // Редкие события для датчиков-предупреждений
    testTire = (random(0, 100) < 5);   // 5% шанс низкого давления
    testWash = (random(0, 100) < 3);   // 3% шанс низкого уровня омывайки
}

// =============================================================================
// climateCmdHandler — обработка специфичных команд модуля Climate
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
static bool climateCmdHandler(uint8_t cmd) {
    switch ((Command)cmd) {
        case CMD_OTA_START:
            return true;  // Завершить задачу при OTA
            
        default:
            return false;
    }
}

// =============================================================================
// climateTask — Главная задача FreeRTOS
// =============================================================================
// Архитектура на основе task_common:
//   1. taskInit() — инициализация, создание cmdQ, подписка на TOPIC_CMD
//   2. Основной цикл:
//      - taskHeartbeat() — обновление счётчика активности
//      - taskProcessCommands() — обработка команд (включая OTA)
//      - Публикация ClimatePack каждые 1000 мс
//
// При получении CMD_OTA_START:
//   - taskProcessCommands() вызывает taskShutdown()
//   - taskShutdown() отписывается от топиков, удаляет очереди, завершает задачу
//   - Память полностью освобождается для OTA-обновления
//
void climateTask(void* parameter) {
    (void)parameter;
    
    // === ИНИЦИАЛИЗАЦИЯ ЧЕРЕЗ ФРЕЙМВОРК ===
    // taskInit() выполняет:
    //   - Установку isRunningFlag = true
    //   - Создание cmdQ (очередь команд)
    //   - Подписку на TOPIC_CMD
    //   - Регистрацию подписки для автоматической очистки при shutdown
    if (!taskInit(&ctx, "Climate", &isRunningFlag, &lastHeartbeat)) {
        DBG_PRINTLN("[Climate] ERROR: taskInit failed!");
        isRunningFlag = false;
        vTaskDelete(NULL);
        return;
    }
    
    DataRouter& router = DataRouter::getInstance();
    
    // === ПОДПИСКА НА ТОПИКИ ДАННЫХ ===
    // Climate только публикует данные, но не подписывается на другие топики.
    // Если в будущем потребуется подписка (например, на настройки климата),
    // используем паттерн:
    //   QueueHandle_t settingsQ = xQueueCreate(1, sizeof(SettingsPack));
    //   router.subscribe(TOPIC_SETTINGS_PACK, settingsQ, QueuePolicy::OVERWRITE);
    //   taskRegisterSubscription(&ctx, TOPIC_SETTINGS_PACK, settingsQ);
    
    unsigned long lastPublish = 0;
    const unsigned long PUBLISH_INTERVAL = 1000;  // 1 Гц
    
    // =========================================================================
    // ОСНОВНОЙ ЦИКЛ ЗАДАЧИ
    // =========================================================================
    while (1) {
        // 1. Heartbeat — обновление счётчика для loop()-мониторинга
        taskHeartbeat(&ctx);
        
        // 2. Обработка команд (CMD_OTA_START обрабатывается автоматически)
        //    При получении CMD_OTA_START эта функция НЕ ВОЗВРАЩАЕТСЯ
        taskProcessCommands(&ctx, climateCmdHandler);
        
        // 3. Периодическая публикация ClimatePack
        unsigned long now = millis();
        if (now - lastPublish >= PUBLISH_INTERVAL) {
            lastPublish = now;
            
            // Обновление тестовых данных (раз в 5 секунд внутри функции)
            updateTestData();
            
            // Формирование пакета ClimatePack
            ClimatePack pack;
            memset(&pack, 0, sizeof(pack));
            pack.version = 1;
            pack.interior_temp = testInterior;
            pack.exterior_temp = testExterior;
            pack.tire_pressure = testTire;
            pack.washer_level  = testWash;
            
            // Публикация в шину DataRouter
            // Protocol Task подписан на TOPIC_CLIMATE_PACK и добавит эти данные в SERVICE JSON
            router.publishPacket(TOPIC_CLIMATE_PACK, &pack, sizeof(pack));
        }
        
        // 4. Задержка 100 мс — даём работать другим задачам
        //    Частота публикации 1 Гц обеспечивается проверкой now - lastPublish
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

// =============================================================================
// Управление задачей (публичный API)
// =============================================================================

// climateStart — запуск задачи Climate
// Вызывается из main.cpp в setup()
void climateStart() {
    if (!taskHandle) {
        // Устанавливаем heartbeat СРАЗУ, чтобы loop() не думал, что задача crashed
        lastHeartbeat = millis();
        isRunningFlag = true;
        
        // Создаём задачу на Ядре 1 (все сервисные задачи)
        // Приоритет TASK_PRIORITY_CLIMATE (1 — низкий, т.к. не критично)
        xTaskCreatePinnedToCore(
            climateTask,                // Функция задачи
            "Climate",                  // Имя (для отладки)
            TASK_STACK_CLIMATE,         // Размер стека (из app_config.h)
            NULL,                       // Параметры (не используются)
            TASK_PRIORITY_CLIMATE,      // Приоритет (из app_config.h)
            &taskHandle,                // Указатель на хендл
            1                           // Ядро 1 (сервисные задачи)
        );
    }
}

// climateStop — остановка задачи Climate
// Вызывается при необходимости принудительно завершить задачу
void climateStop() {
    
    DBG_PRINTLN("[Climate] climateStop() CALLED");

    if (taskHandle) {
        // Принудительное удаление задачи
        // ВНИМАНИЕ: ресурсы НЕ очищаются! Используйте только если задача зависла.
        // Для корректного завершения используйте CMD_OTA_START через шину.
        vTaskDelete(taskHandle);
        taskHandle = NULL;
        isRunningFlag = false;
        DBG_PRINTLN("[Climate] Stopped (forced)");
    }
}

// climateIsRunning — проверка активности задачи
// Вызывается из loop() для мониторинга и перезапуска при зависании
bool climateIsRunning() {
    // Задача считается активной если:
    //   1. Флаг isRunningFlag установлен
    //   2. Последний heartbeat был менее 3 секунд назад
    return isRunningFlag && (millis() - lastHeartbeat) < 3000;
}