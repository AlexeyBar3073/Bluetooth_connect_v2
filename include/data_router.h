// -----------------------------------------------------------------------------
// data_router.h
// Маршрутизатор данных (Pub/Sub) с типизированными топиками.
//
// Архитектура:
// - Каждый топик управляет СВОИМ массивом подписчиков и СВОИМ кэшем
// - Очередь создаётся МОДУЛЕМ (подписчиком), маршрутизатор только регистрирует
// - Тип данных топика строго определён — нет единого BusMessage
// - RAM: ~2-3 КБ (все топики, все подписчики)
//
// Принципы:
//   publish(topic, value) → копирует напрямую в очереди подписчиков
//   Модуль: xQueueReceive(своя_очередь, &value, timeout)
//
// -----------------------------------------------------------------------------
// ПРАВИЛА ФАЙЛА:
// -----------------------------------------------------------------------------
// ✅ Можно:
//   - Добавлять новые топики
//   - Менять типы данных топиков
//   - Увеличивать глубины очередей
//
// ❌ Нельзя:
//   - Менять числовые значения топиков
//   - Удалять топики
//   - Использовать callback-и
//   - Забывать создавать очередь до subscribe()
//
// ВЕРСИЯ: 6.0.0 — MAJOR: Типизированные топики, очереди у модулей
// -----------------------------------------------------------------------------

#ifndef DATA_ROUTER_H
#define DATA_ROUTER_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include "topics.h"
#include "commands.h"

// =============================================================================
// Максимальное количество подписчиков на один топик
// =============================================================================
#define ROUTER_MAX_SUBS_PER_TOPIC  10

// =============================================================================
// Максимальная глубина очереди (для FIFO_DROP топиков)
// =============================================================================
#define ROUTER_MAX_QUEUE_DEPTH   5

// =============================================================================
// Стратегия очереди
// =============================================================================

enum class QueuePolicy : uint8_t {
    OVERWRITE,    // Глубина 1, xQueueOverwrite — всегда актуальное
    FIFO_DROP     // Глубина N, xQueueSend — дроп при переполнении
};

// =============================================================================
// Типы данных топиков (строго определены)
// =============================================================================
//
// ВАЖНО: эти enum-ы используются для проверки типов при compile-time.
// Каждый топик маршрутизатора имеет свой тип данных.
//

// Типы данных для каждого топика:
//   TOPIC_ENGINE_PACK    → EnginePack* (binary, publishPacket)
//   TOPIC_TRIP_PACK      → TripPack*
//   TOPIC_KLINE_PACK     → KlinePack*
//   TOPIC_CLIMATE_PACK   → ClimatePack*
//   TOPIC_SETTINGS_PACK  → SettingsPack*
//   TOPIC_CMD            → uint8_t (enum Command, 1 байт)
//   TOPIC_MSG_INCOMING   → char* (строка)
//   TOPIC_MSG_OUTGOING   → char* (строка)
//   TOPIC_TRANSPORT_STATUS → bool
//   TOPIC_CORRECT_ODO    → double (или float)

// =============================================================================
// DataRouter — Маршрутизатор (Singleton)
// =============================================================================
//
// Использование:
//
// 1. Модуль создаёт очередь:
//    QueueHandle_t cmdQ = xQueueCreate(5, sizeof(uint8_t));
//
// 2. Подписка:
//    DataRouter::getInstance().subscribe(TOPIC_CMD, cmdQ, QueuePolicy::FIFO_DROP);
//
// 3. Чтение:
//    uint8_t cmd;
//    if (xQueueReceive(cmdQ, &cmd, pdMS_TO_TICKS(50)) == pdTRUE) {
//        if ((Command)cmd == CMD_RESET_TRIP_A) { ... }
//    }
//
// 4. Публикация:
//    DataRouter::getInstance().publish(TOPIC_CMD, CMD_RESET_TRIP_A);
//

class DataRouter {
public:
    static DataRouter& getInstance();
    void begin();

    // --- Подписка: модуль передаёт СВОЮ очередь. retain=true → получит кэш ---
    bool subscribe(Topic topic, QueueHandle_t queue, QueuePolicy policy, bool retain = false);

    // Отписка: модуль сам удаляет очередь
    void unsubscribe(Topic topic, QueueHandle_t queue);

    // --- Публикация примитивных типов ---
    bool publish(Topic topic, float value);
    bool publish(Topic topic, int value);
    bool publish(Topic topic, bool value);
    bool publish(Topic topic, double value);

    // --- Публикация команды (TOPIC_CMD — только enum, 1 байт) ---
    bool publish(Topic topic, Command cmd);

    // --- Публикация бинарных пакетов (EnginePack, TripPack...) ---
    bool publishPacket(Topic topic, const void* data, size_t len);

    // --- Публикация строк (TOPIC_MSG_INCOMING / TOPIC_MSG_OUTGOING) ---
    bool publishString(Topic topic, const char* str);

    // --- Подписка на строки (создаёт очередь для string, depth=N) ---
    // Возвращает очередь, которую модуль должен передать в subscribe
    // Используется так:
    //   QueueHandle_t q = router.subscribeString(TOPIC_MSG_OUTGOING, depth);
    //   router.subscribe(TOPIC_MSG_OUTGOING, q, QueuePolicy::FIFO_DROP);
    //
    // Но проще: модуль сам создаёт очередь нужного типа, а subscribe регистрирует.

    // Получить кэшированное значение (если топик поддерживает кэш)
    bool getCached(Topic topic, Command& out);
    bool getCached(Topic topic, bool& out);

    // Статистика
    uint32_t getDropCount(Topic topic);
    void printStats();

private:
    DataRouter() {}

    // --- Внутренняя структура: слот подписчика ---
    struct SubscriberSlot {
        QueueHandle_t queue;
        QueuePolicy   policy;
        uint8_t       depth;
        uint32_t      dropCount;
    };

    // --- Внутренняя структура: топик-маршрутизатор ---
    struct TopicRouter {
        bool            valid;              // Топик активен
        QueuePolicy     defaultPolicy;      // Политика по умолчанию
        uint8_t         subCount;           // Текущее количество подписчиков
        SubscriberSlot  subs[ROUTER_MAX_SUBS_PER_TOPIC];

        // Кэш для retain (примитивные типы)
        bool     boolValid;
        bool     boolCache;
        bool     cmdValid;
        Command  cmdCache;     // 1 байт — последняя команда

        // Кэш пакетов (SettingsPack — 19 байт, TripPack — 41 байт, etc.)
        bool     packetValid;
        uint8_t  packetLen;
        uint8_t  packetCache[80];  // Max пакет (KlinePack=74 байта)
    };

    TopicRouter _topicRouters[TOPIC_COUNT];
    SemaphoreHandle_t _mutex;

    // Внутренние методы
    bool _dispatch(Topic topic, const void* data, size_t len);
};

#endif // DATA_ROUTER_H
