// -----------------------------------------------------------------------------
// data_bus.h
// Центральная шина данных (Pub/Sub) на основе очередей FreeRTOS.
//
// Архитектура:
//   publish(topic, value) → xQueueSend/xQueueOverwrite → очередь подписчика
//   Модуль сам читает: xQueueReceive(своя_очередь, &msg, timeout)
//
// Принципы:
// - Нулевые блокировки в publish(): только xQueueSend(..., 0) — мгновенно
// - Очередь на каждого подписчика — каждый разбирает почту в своём темпе
// - OVERWRITE (depth=1) — для сенсоров/пакетов (всегда актуальное)
// - FIFO_DROP (depth=N) — для команд/msg (буферизация, дроп при переполнении)
// - Retained State Cache — новые подписчики получают последнее значение
//
// RAM: ~3.8 КБ (47 топиков × кэш + ~30 подписчиков × очередь)
// Производительность: publish() ~3-5 мкс
//
// -----------------------------------------------------------------------------
// ПРАВИЛА ФАЙЛА:
// -----------------------------------------------------------------------------
// ✅ Можно:
//   - Добавлять новые топики в enum Topic
//   - Добавлять новые типы данных в DataType
//   - Увеличивать PAYLOAD_MAX, BUS_MAX_SUBSCRIBERS
//   - Добавлять новые методы publish для новых типов
//
// ❌ Нельзя:
//   - Менять числовые значения топиков (сломает NVS-совместимость)
//   - Удалять топики или типы данных
//   - Использовать callback-и (архитектура — ТОЛЬКО очереди)
//   - Вызывать publish() из прерывания (используй xQueueSendFromISR)
//   - Забывать освобождать очередь через unsubscribe()
//
// ВЕРСИЯ: 5.0.0 — MAJOR: Очередная архитектура шины (без callback-ов)
// -----------------------------------------------------------------------------

#ifndef DATA_BUS_H
#define DATA_BUS_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include "topics.h"
#include "commands.h"

// =============================================================================
// Конфигурация шины
// =============================================================================

#define PAYLOAD_MAX          256    // Макс. размер строки в BusMessage (байт)
#define BUS_MAX_SUBSCRIBERS  50     // Макс. количество подписчиков (очередей)

// =============================================================================
// Типы данных
// =============================================================================
//
// DataType определяет, какое поле union value содержит данные.
//
enum DataType : uint8_t {
    TYPE_FLOAT,      // value.f
    TYPE_INT,        // value.i
    TYPE_BOOL,       // value.b
    TYPE_STRING,     // value.s (копируется, null-terminated)
    TYPE_DOUBLE,     // value.d
    TYPE_CMD         // cmd (CmdPayload)
};

// =============================================================================
// BusMessage — Сообщение шины (передаётся через очередь FreeRTOS)
// =============================================================================
//
// Размер: ~268 байт (Topic 1 + DataType 1 + union 256 + CmdPayload ~10 + padding)
//
// Передаётся через xQueueSend/xQueueOverwrite от publish() к подписчику.
// Содержит данные в зависимости от type.
//
struct BusMessage {
    Topic     topic;     // Топик сообщения
    DataType  type;      // Тип данных

    union {
        float  f;        // TYPE_FLOAT
        int    i;        // TYPE_INT
        bool   b;        // TYPE_BOOL
        double d;        // TYPE_DOUBLE
        char   s[PAYLOAD_MAX];  // TYPE_STRING
    } value;

    CmdPayload cmd;      // TYPE_CMD — команда (enum + union параметров + msg_id)
};

// =============================================================================
// Политика очереди
// =============================================================================
//
// QUEUE_OVERWRITE: глубина=1, xQueueOverwrite()
//   — Новое сообщение перезаписывает старое
//   — Для: сенсоры, пакеты, данные которые должны быть актуальными
//
// QUEUE_FIFO_DROP: глубина=N, xQueueSend(..., 0)
//   — Буферизация N сообщений, при переполнении — дроп (dropCount++)
//   — Для: команды, входящие/исходящие JSON
//
enum QueuePolicy {
    QUEUE_OVERWRITE,    // Глубина 1, перезапись (актуальное значение)
    QUEUE_FIFO_DROP     // Глубина N, FIFO, дроп при переполнении
};

// =============================================================================
// SubscriberOpts — Опции подписки
// =============================================================================
//
// policy   — Политика очереди (OVERWRITE или FIFO_DROP)
// depth    — Глубина очереди (1 для OVERWRITE, 3-5 для FIFO_DROP)
// retain   — При подписке получить последнее значение из кэша
//
struct SubscriberOpts {
    QueuePolicy policy;     // OVERWRITE или FIFO_DROP
    uint8_t     depth;      // Глубина очереди (1-5)
    bool        retain;     // Получить retain-значение при подписке
};

// =============================================================================
// DataBus — Центральная шина данных (Singleton)
// =============================================================================
//
// publish() — «почтальон»: кладёт конверт в почтовый ящик и сразу возвращается.
// Каждый участник разбирает почту в своём темпе через xQueueReceive().
//
class DataBus {
public:
    // getInstance: Единственный экземпляр шины (Singleton).
    static DataBus& getInstance();

    // begin: Инициализация шины — мьютекс, очистка кэша.
    // ДОЛЖЕН вызываться ПЕРЕД subscribe/publish.
    void begin();

    // subscribe: Подписка на топик — создаёт очередь и возвращает handle.
    // Подписчик читает через xQueueReceive(queue, &msg, timeout).
    // Если opts.retain=true — получает последнее значение из кэша.
    QueueHandle_t subscribe(Topic topic, const SubscriberOpts& opts);

    // unsubscribe: Отписка — удаляет очередь подписчика.
    void unsubscribe(Topic topic, QueueHandle_t queue);

    // ========================================================================
    // Публикация (publish) — кладёт BusMessage в очередь КАЖДОГО подписчика
    // ========================================================================
    //
    // publish() НЕ блокирует отправителя.
    // Для OVERWRITE: xQueueOverwrite() — всегда успешно
    // Для FIFO_DROP: xQueueSend(..., 0) — false если очередь полна (дроп)
    //

    // publish(float): Публикация числа с плавающей точкой.
    bool publish(Topic topic, float value);

    // publish(int): Публикация целого числа.
    bool publish(Topic topic, int value);

    // publish(bool): Публикация логического значения.
    bool publish(Topic topic, bool value);

    // publish(double): Публикация double.
    bool publish(Topic topic, double value);

    // publish(const char*): Публикация строки (копируется в BusMessage.value.s).
    bool publish(Topic topic, const char* value);

    // publish(CmdPayload): Публикация команды.
    bool publish(Topic topic, const CmdPayload& cmd);

    // publishPacket: Публикация бинарного пакета (EnginePack, TripPack и т.д.).
    // Копирует данные в BusMessage.value.s (до PAYLOAD_MAX байт).
    // Тип сообщения — TYPE_STRING (данные в value.s), топик — пакетный.
    // Подписчик читает и кастует: memcpy(&pack, msg.value.s, sizeof(pack)).
    bool publishPacket(Topic topic, const void* data, size_t len);

    // ========================================================================
    // Кэш — retained state (последнее значение каждого топика)
    // ========================================================================

    // getCached: Получает последнее опубликованное значение топика.
    // Используется для retain-инжекции при подписке.
    bool getCached(Topic topic, BusMessage& out);

    // getDropCount: Количество дропов для топика (FIFO_DROP очереди).
    uint32_t getDropCount(Topic topic);

    // printStats: Вывод статистики в Serial (для отладки).
    void printStats();

private:
    DataBus() {}

    // Подписчик: топик + очередь + опции + счётчик дропов
    struct SubscriberSlot {
        Topic           topic;
        QueueHandle_t   queue;
        SubscriberOpts  opts;
        uint32_t        dropCount;  // Счётчик отброшенных сообщений
    } _subscribers[BUS_MAX_SUBSCRIBERS];
    int _subscriberCount = 0;

    // Кэш последнего значения для каждого топика (retain)
    struct CachedValue {
        bool        valid;      // true = есть значение в кэше
        BusMessage  msg;        // Последнее опубликованное сообщение
    } _cache[TOPIC_COUNT];

    SemaphoreHandle_t _mutex;   // Мьютекс для защиты _subscribers и _cache

    // _getQueueForTopic: Создаёт очередь с нужной политикой и глубиной.
    QueueHandle_t _getQueueForTopic(const SubscriberOpts& opts);

    // _updateCache: Обновляет кэш последнего значения топика.
    void _updateCache(Topic topic, const BusMessage& msg);

    // _dispatchToSubscribers: Рассылает сообщение всем подписчикам топика.
    // Для OVERWRITE: xQueueOverwrite (всегда успешно)
    // Для FIFO_DROP: xQueueSend(..., 0) → dropCount++ при неудаче
    bool _dispatchToSubscribers(Topic topic, const BusMessage& msg);
};

#endif // DATA_BUS_H
