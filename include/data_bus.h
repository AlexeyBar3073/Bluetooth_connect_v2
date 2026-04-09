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
// - BusMessage содержит встроенный буфер (без heap-аллокации)
//
// RAM: ~13 КБ (.bss, PAYLOAD_MAX=384, 10 топиков, 50 подписчиков)
// Размер BusMessage: ~416 байт
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
// ВЕРСИЯ: 5.1.0 — MAJOR: Встроенный буфер (без heap-аллокации)
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

#define PAYLOAD_MAX          256    // Макс. размер пакета (байт). Встроенный буфер.
#define BUS_MAX_SUBSCRIBERS  25     // Макс. количество подписчиков (очередей)

// =============================================================================
// Типы данных
// =============================================================================

enum DataType : uint8_t {
    TYPE_FLOAT,      // value.f
    TYPE_INT,        // value.i
    TYPE_BOOL,       // value.b
    TYPE_STRING,     // value.s (встроенный буфер, null-terminated)
    TYPE_DOUBLE,     // value.d
    TYPE_CMD         // cmd (CmdPayload)
};

// =============================================================================
// BusMessage — Сообщение шины (передаётся через очередь FreeRTOS)
// =============================================================================
//
// Размер: ~416 байт (Topic 1 + DataType 1 + union 384 + CmdPayload ~24 + padding)
//
// Встроенный буфер — нет heap-аллокации при dispatch.
// Подписчик читает: memcpy(&pack, msg.value.s, sizeof(pack)).
// busMessageFree — NOOP (для совместности API).
//
struct BusMessage {
    Topic     topic;     // Топик сообщения
    DataType  type;      // Тип данных

    union {
        float  f;        // TYPE_FLOAT
        int    i;        // TYPE_INT
        bool   b;        // TYPE_BOOL
        double d;        // TYPE_DOUBLE
        char   s[PAYLOAD_MAX];  // TYPE_STRING/PACKET
    } value;

    CmdPayload cmd;      // TYPE_CMD — команда
};

// =============================================================================
// busMessageFree: NOOP — встроенный буфер не требует освобождения
// =============================================================================
static inline void busMessageFree(BusMessage* msg) {
    (void)msg;  // NOOP — данные встроены в struct
}

// =============================================================================
// Политика очереди
// =============================================================================

enum QueuePolicy {
    QUEUE_OVERWRITE,    // Глубина 1, перезапись (актуальное значение)
    QUEUE_FIFO_DROP     // Глубина N, FIFO, дроп при переполнении
};

// =============================================================================
// SubscriberOpts — Опции подписки
// =============================================================================

struct SubscriberOpts {
    QueuePolicy policy;     // OVERWRITE или FIFO_DROP
    uint8_t     depth;      // Глубина очереди (1-5)
    bool        retain;     // Получить retain-значение при подписке
};

// =============================================================================
// DataBus — Центральная шина данных (Singleton)
// =============================================================================

class DataBus {
public:
    static DataBus& getInstance();
    void begin();
    QueueHandle_t subscribe(Topic topic, const SubscriberOpts& opts);
    void unsubscribe(Topic topic, QueueHandle_t queue);

    bool publish(Topic topic, float value);
    bool publish(Topic topic, int value);
    bool publish(Topic topic, bool value);
    bool publish(Topic topic, double value);
    bool publish(Topic topic, const char* value);
    bool publish(Topic topic, const CmdPayload& cmd);
    bool publishPacket(Topic topic, const void* data, size_t len);

    bool getCached(Topic topic, BusMessage& out);
    uint32_t getDropCount(Topic topic);
    void printStats();

private:
    DataBus() {}

    struct SubscriberSlot {
        Topic           topic;
        QueueHandle_t   queue;
        SubscriberOpts  opts;
        uint32_t        dropCount;
    } _subscribers[BUS_MAX_SUBSCRIBERS];
    int _subscriberCount = 0;

    struct CachedValue {
        bool        valid;
        BusMessage  msg;
    } _cache[TOPIC_COUNT];

    SemaphoreHandle_t _mutex;

    QueueHandle_t _getQueueForTopic(const SubscriberOpts& opts);
    void _updateCache(Topic topic, const BusMessage& msg);
    bool _dispatchToSubscribers(Topic topic, const BusMessage& msg);
};

#endif // DATA_BUS_H
