// -----------------------------------------------------------------------------
// data_bus.cpp
// Реализация шины данных на основе очередей FreeRTOS.
//
// Архитектура «почтовый ящик»:
//   publish() → для каждого подписчика: xQueueSend/xQueueOverwrite → очередь
//   Модуль: xQueueReceive(своя_очередь, &msg, timeout) — разбирает почту
//
// BusMessage содержит встроенный буфер (PAYLOAD_MAX=384) — нет heap-аллокации.
// dispatch копирует целиком через memcpy — быстро и без фрагментации.
//
// -----------------------------------------------------------------------------
// ПРАВИЛА ФАЙЛА:
// -----------------------------------------------------------------------------
// ✅ Можно:
//   - Оптимизировать _dispatchToSubscribers для скорости
//   - Добавлять новые методы publish
//   - Менять размеры кэша и очередей
//
// ❌ Нельзя:
//   - Менять логику OVERWRITE vs FIFO_DROP
//   - Добавлять callback-и (нарушает принцип очередей)
//   - Блокировать publish() (должен быть ~3-5 мкс)
//   - Удалять публичные методы
//
// ВЕРСИЯ: 5.1.0 — MAJOR: Встроенный буфер (без heap-аллокации)
// -----------------------------------------------------------------------------

#include "data_bus.h"

// =============================================================================
// Singleton
// =============================================================================

DataBus& DataBus::getInstance() {
    static DataBus instance;
    return instance;
}

// =============================================================================
// begin: Инициализация шины
// =============================================================================

void DataBus::begin() {
    _mutex = xSemaphoreCreateMutex();
    _subscriberCount = 0;
    for (int i = 0; i < TOPIC_COUNT; i++) {
        _cache[i].valid = false;
    }
    Serial.println("[DataBus] Initialized (Queue-based Pub/Sub)");
}

// =============================================================================
// _getQueueForTopic
// =============================================================================

QueueHandle_t DataBus::_getQueueForTopic(const SubscriberOpts& opts) {
    uint8_t depth = (opts.policy == QUEUE_OVERWRITE) ? 1 : opts.depth;
    return xQueueCreate(depth, sizeof(BusMessage));
}

// =============================================================================
// subscribe
// =============================================================================

QueueHandle_t DataBus::subscribe(Topic topic, const SubscriberOpts& opts) {
    xSemaphoreTake(_mutex, portMAX_DELAY);

    if (_subscriberCount >= BUS_MAX_SUBSCRIBERS) {
        Serial.printf("[DataBus] ERROR: Max subscribers (%d)!\n", BUS_MAX_SUBSCRIBERS);
        xSemaphoreGive(_mutex);
        return NULL;
    }

    QueueHandle_t q = _getQueueForTopic(opts);
    if (!q) {
        Serial.println("[DataBus] ERROR: Queue creation failed!");
        xSemaphoreGive(_mutex);
        return NULL;
    }

    _subscribers[_subscriberCount].topic = topic;
    _subscribers[_subscriberCount].queue = q;
    _subscribers[_subscriberCount].opts = opts;
    _subscribers[_subscriberCount].dropCount = 0;
    _subscriberCount++;

    xSemaphoreGive(_mutex);

    // Retain-инжекция
    if (opts.retain) {
        BusMessage cached;
        if (getCached(topic, cached)) {
            BaseType_t result;
            if (opts.policy == QUEUE_OVERWRITE) {
                result = xQueueOverwrite(q, &cached);
            } else {
                result = xQueueSend(q, &cached, 0);
            }
            (void)result;
        }
    }

    return q;
}

// =============================================================================
// unsubscribe
// =============================================================================

void DataBus::unsubscribe(Topic topic, QueueHandle_t queue) {
    xSemaphoreTake(_mutex, portMAX_DELAY);

    for (int i = 0; i < _subscriberCount; i++) {
        if (_subscribers[i].topic == topic && _subscribers[i].queue == queue) {
            vQueueDelete(_subscribers[i].queue);

            for (int j = i; j < _subscriberCount - 1; j++) {
                _subscribers[j] = _subscribers[j + 1];
            }
            _subscriberCount--;
            break;
        }
    }

    xSemaphoreGive(_mutex);
}

// =============================================================================
// _updateCache
// =============================================================================

void DataBus::_updateCache(Topic topic, const BusMessage& msg) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _cache[topic].valid = true;
    memcpy(&_cache[topic].msg, &msg, sizeof(BusMessage));
    xSemaphoreGive(_mutex);
}

// =============================================================================
// _dispatchToSubscribers
// =============================================================================

bool DataBus::_dispatchToSubscribers(Topic topic, const BusMessage& msg) {
    bool any = false;

    for (int i = 0; i < _subscriberCount; i++) {
        if (_subscribers[i].topic != topic) continue;

        SubscriberSlot& sub = _subscribers[i];
        BaseType_t result;

        if (sub.opts.policy == QUEUE_OVERWRITE) {
            result = xQueueOverwrite(sub.queue, &msg);
            if (result == pdTRUE) any = true;
        } else {
            result = xQueueSend(sub.queue, &msg, 0);
            if (result != pdTRUE) {
                sub.dropCount++;
            } else {
                any = true;
            }
        }
    }

    return any;
}

// =============================================================================
// getCached
// =============================================================================

bool DataBus::getCached(Topic topic, BusMessage& out) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool ok = _cache[topic].valid;
    if (ok) {
        memcpy(&out, &_cache[topic].msg, sizeof(BusMessage));
    }
    xSemaphoreGive(_mutex);
    return ok;
}

// =============================================================================
// getDropCount
// =============================================================================

uint32_t DataBus::getDropCount(Topic topic) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    uint32_t total = 0;
    for (int i = 0; i < _subscriberCount; i++) {
        if (_subscribers[i].topic == (Topic)topic) {
            total += _subscribers[i].dropCount;
        }
    }
    xSemaphoreGive(_mutex);
    return total;
}

// =============================================================================
// publish(float)
// =============================================================================

bool DataBus::publish(Topic topic, float value) {
    BusMessage msg;
    msg.topic = topic;
    msg.type = TYPE_FLOAT;
    msg.value.f = value;
    memset(&msg.cmd, 0, sizeof(CmdPayload));

    _updateCache(topic, msg);
    return _dispatchToSubscribers(topic, msg);
}

// =============================================================================
// publish(int)
// =============================================================================

bool DataBus::publish(Topic topic, int value) {
    BusMessage msg;
    msg.topic = topic;
    msg.type = TYPE_INT;
    msg.value.i = value;
    memset(&msg.cmd, 0, sizeof(CmdPayload));

    _updateCache(topic, msg);
    return _dispatchToSubscribers(topic, msg);
}

// =============================================================================
// publish(bool)
// =============================================================================

bool DataBus::publish(Topic topic, bool value) {
    BusMessage msg;
    msg.topic = topic;
    msg.type = TYPE_BOOL;
    msg.value.b = value;
    memset(&msg.cmd, 0, sizeof(CmdPayload));

    _updateCache(topic, msg);
    return _dispatchToSubscribers(topic, msg);
}

// =============================================================================
// publish(double)
// =============================================================================

bool DataBus::publish(Topic topic, double value) {
    BusMessage msg;
    msg.topic = topic;
    msg.type = TYPE_DOUBLE;
    msg.value.d = value;
    memset(&msg.cmd, 0, sizeof(CmdPayload));

    _updateCache(topic, msg);
    return _dispatchToSubscribers(topic, msg);
}

// =============================================================================
// publish(const char*)
// =============================================================================

bool DataBus::publish(Topic topic, const char* value) {
    BusMessage msg;
    msg.topic = topic;
    msg.type = TYPE_STRING;
    memset(&msg.value, 0, sizeof(msg.value));
    memset(&msg.cmd, 0, sizeof(CmdPayload));

    size_t len = strlen(value);
    if (len >= PAYLOAD_MAX) {
        Serial.printf("[DataBus] WARN: String truncated (%zu >= %d)\n", len, PAYLOAD_MAX);
        len = PAYLOAD_MAX - 1;
    }
    memcpy(msg.value.s, value, len);
    msg.value.s[len] = '\0';

    _updateCache(topic, msg);
    return _dispatchToSubscribers(topic, msg);
}

// =============================================================================
// publish(CmdPayload)
// =============================================================================

bool DataBus::publish(Topic topic, const CmdPayload& cmd) {
    BusMessage msg;
    msg.topic = topic;
    msg.type = TYPE_CMD;
    memset(&msg.value, 0, sizeof(msg.value));
    memcpy(&msg.cmd, &cmd, sizeof(CmdPayload));

    _updateCache(topic, msg);
    return _dispatchToSubscribers(topic, msg);
}

// =============================================================================
// publishPacket
// =============================================================================

bool DataBus::publishPacket(Topic topic, const void* data, size_t len) {
    if (len > PAYLOAD_MAX) {
        Serial.printf("[DataBus] ERROR: Packet too large: %zu > %d\n", len, PAYLOAD_MAX);
        return false;
    }

    BusMessage msg;
    msg.topic = topic;
    msg.type = TYPE_STRING;
    memset(&msg.value, 0, sizeof(msg.value));
    memset(&msg.cmd, 0, sizeof(CmdPayload));

    memcpy(msg.value.s, data, len);

    _updateCache(topic, msg);
    return _dispatchToSubscribers(topic, msg);
}

// =============================================================================
// printStats
// =============================================================================

void DataBus::printStats() {
    Serial.println("=== DataBus Stats ===");

    int cached = 0;
    for (int i = 0; i < TOPIC_COUNT; i++) {
        if (_cache[i].valid) cached++;
    }
    Serial.printf("Cached topics: %d/%d\n", cached, TOPIC_COUNT);
    Serial.printf("Subscribers: %d/%d\n", _subscriberCount, BUS_MAX_SUBSCRIBERS);

    for (int t = 0; t < TOPIC_COUNT; t++) {
        uint32_t drops = 0;
        for (int i = 0; i < _subscriberCount; i++) {
            if (_subscribers[i].topic == (Topic)t) {
                drops += _subscribers[i].dropCount;
            }
        }
        if (drops > 0) {
            Serial.printf("Topic %d: %lu drops\n", t, drops);
        }
    }

    Serial.println("=====================");
}
