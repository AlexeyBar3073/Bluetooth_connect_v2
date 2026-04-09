// -----------------------------------------------------------------------------
// data_router.cpp
// Реализация маршрутизатора данных с типизированными топиками.
//
// Ключевое отличие от DataBus:
// - Нет единого BusMessage — каждый топик работает со своим типом данных
// - Очередь создаётся МОДУЛЕМ, маршрутизатор только регистрирует
// - dispatch копирует данные напрямую в очереди подписчиков (memcpy)
// - RAM пропорциональна типу данных, а не максимальному буферу
//
// -----------------------------------------------------------------------------
// ПРАВИЛА ФАЙЛА:
// -----------------------------------------------------------------------------
// ✅ Можно:
//   - Оптимизировать _dispatch
//   - Добавлять новые методы publish
//
// ❌ Нельзя:
//   - Менять логику OVERWRITE vs FIFO_DROP
//   - Добавлять callback-и
//   - Блокировать publish()
//
// ВЕРСИЯ: 6.0.0 — MAJOR: Типизированные топики, очереди у модулей
// -----------------------------------------------------------------------------

#include "data_router.h"

// =============================================================================
// Singleton
// =============================================================================

DataRouter& DataRouter::getInstance() {
    static DataRouter instance;
    return instance;
}

// =============================================================================
// begin: Инициализация
// =============================================================================

void DataRouter::begin() {
    _mutex = xSemaphoreCreateMutex();
    for (int i = 0; i < TOPIC_COUNT; i++) {
        _topicRouters[i].valid = false;
        _topicRouters[i].defaultPolicy = QueuePolicy::OVERWRITE;
        _topicRouters[i].subCount = 0;
        _topicRouters[i].boolValid = false;
        _topicRouters[i].cmdValid = false;
        _topicRouters[i].packetValid = false;
        _topicRouters[i].packetLen = 0;
    }
    Serial.println("[DataRouter] Initialized (Typed topics, module-owned queues)");
}

// =============================================================================
// subscribe
// =============================================================================

bool DataRouter::subscribe(Topic topic, QueueHandle_t queue, QueuePolicy policy, bool retain) {
    if (!queue) {
        Serial.println("[DataRouter] ERROR: NULL queue!");
        return false;
    }

    xSemaphoreTake(_mutex, portMAX_DELAY);

    TopicRouter& tr = _topicRouters[topic];
    if (tr.subCount >= ROUTER_MAX_SUBS_PER_TOPIC) {
        Serial.printf("[DataRouter] ERROR: Max subscribers for topic %d!\n", topic);
        xSemaphoreGive(_mutex);
        return false;
    }

    uint8_t depth = (policy == QueuePolicy::OVERWRITE) ? 1 : ROUTER_MAX_QUEUE_DEPTH;

    // Активируем топик при первой подписке
    if (!tr.valid) {
        tr.valid = true;
        tr.defaultPolicy = policy;
    }

    tr.subs[tr.subCount].queue = queue;
    tr.subs[tr.subCount].policy = policy;
    tr.subs[tr.subCount].depth = depth;
    tr.subs[tr.subCount].dropCount = 0;
    tr.subCount++;

    // Retain-инжекция: если кэш пакета есть — кладём в очередь
    if (retain && tr.packetValid && tr.packetLen > 0) {
        xQueueOverwrite(queue, tr.packetCache);
    }

    xSemaphoreGive(_mutex);

    Serial.printf("[DataRouter] Subscribed: topic=%d, queue=%p, policy=%s, depth=%d, subs=%d, retain=%d\n",
                  topic, queue, policy == QueuePolicy::OVERWRITE ? "OVERWRITE" : "FIFO_DROP",
                  depth, tr.subCount, retain ? 1 : 0);

    return true;
}

// =============================================================================
// unsubscribe
// =============================================================================

void DataRouter::unsubscribe(Topic topic, QueueHandle_t queue) {
    xSemaphoreTake(_mutex, portMAX_DELAY);

    TopicRouter& tr = _topicRouters[topic];
    for (int i = 0; i < tr.subCount; i++) {
        if (tr.subs[i].queue == queue) {
            // НЕ удаляем очередь — она принадлежит модулю
            for (int j = i; j < tr.subCount - 1; j++) {
                tr.subs[j] = tr.subs[j + 1];
            }
            tr.subCount--;
            break;
        }
    }

    xSemaphoreGive(_mutex);
}

// =============================================================================
// _dispatch — рассылка данных подписчикам
// =============================================================================

bool DataRouter::_dispatch(Topic topic, const void* data, size_t len) {
    TopicRouter& tr = _topicRouters[topic];
    if (!tr.valid || tr.subCount == 0) return false;

    bool any = false;

    for (int i = 0; i < tr.subCount; i++) {
        SubscriberSlot& sub = tr.subs[i];
        BaseType_t result;

        if (sub.policy == QueuePolicy::OVERWRITE) {
            result = xQueueOverwrite(sub.queue, data);
            if (result == pdTRUE) any = true;
        } else {
            result = xQueueSend(sub.queue, data, 0);
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
// publish(float)
// =============================================================================

bool DataRouter::publish(Topic topic, float value) {
    float buf = value;
    _dispatch(topic, &buf, sizeof(float));
    return true;
}

// =============================================================================
// publish(int)
// =============================================================================

bool DataRouter::publish(Topic topic, int value) {
    int buf = value;
    _dispatch(topic, &buf, sizeof(int));
    return true;
}

// =============================================================================
// publish(bool)
// =============================================================================

bool DataRouter::publish(Topic topic, bool value) {
    // Обновляем кэш
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _topicRouters[topic].boolValid = true;
    _topicRouters[topic].boolCache = value;
    xSemaphoreGive(_mutex);

    _dispatch(topic, &value, sizeof(bool));
    return true;
}

// =============================================================================
// publish(double)
// =============================================================================

bool DataRouter::publish(Topic topic, double value) {
    double buf = value;
    _dispatch(topic, &buf, sizeof(double));
    return true;
}

// =============================================================================
// publish(Command) — только 1 байт для TOPIC_CMD
// =============================================================================

bool DataRouter::publish(Topic topic, Command cmd) {
    uint8_t buf = (uint8_t)cmd;

    xSemaphoreTake(_mutex, portMAX_DELAY);
    _topicRouters[topic].cmdValid = true;
    _topicRouters[topic].cmdCache = cmd;
    xSemaphoreGive(_mutex);

    return _dispatch(topic, &buf, sizeof(uint8_t));
}

// =============================================================================
// publishPacket (бинарный пакет: EnginePack, TripPack...)
// =============================================================================

bool DataRouter::publishPacket(Topic topic, const void* data, size_t len) {
    // Обновляем кэш пакета
    xSemaphoreTake(_mutex, portMAX_DELAY);
    TopicRouter& tr = _topicRouters[topic];
    tr.packetValid = true;
    tr.packetLen = (len < sizeof(tr.packetCache)) ? len : sizeof(tr.packetCache);
    memcpy(tr.packetCache, data, tr.packetLen);
    xSemaphoreGive(_mutex);

    return _dispatch(topic, data, len);
}

// =============================================================================
// publishString (строка: TOPIC_MSG_INCOMING / TOPIC_MSG_OUTGOING)
// =============================================================================

bool DataRouter::publishString(Topic topic, const char* str) {
    return _dispatch(topic, str, strlen(str));
}

// =============================================================================
// getCached (Command)
// =============================================================================

bool DataRouter::getCached(Topic topic, Command& out) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool ok = _topicRouters[topic].cmdValid;
    if (ok) {
        out = _topicRouters[topic].cmdCache;
    }
    xSemaphoreGive(_mutex);
    return ok;
}

// =============================================================================
// getCached (bool)
// =============================================================================

bool DataRouter::getCached(Topic topic, bool& out) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool ok = _topicRouters[topic].boolValid;
    if (ok) {
        out = _topicRouters[topic].boolCache;
    }
    xSemaphoreGive(_mutex);
    return ok;
}

// =============================================================================
// getDropCount
// =============================================================================

uint32_t DataRouter::getDropCount(Topic topic) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    uint32_t total = 0;
    TopicRouter& tr = _topicRouters[topic];
    for (int i = 0; i < tr.subCount; i++) {
        total += tr.subs[i].dropCount;
    }
    xSemaphoreGive(_mutex);
    return total;
}

// =============================================================================
// printStats
// =============================================================================

void DataRouter::printStats() {
    Serial.println("=== DataRouter Stats ===");
    for (int t = 0; t < TOPIC_COUNT; t++) {
        TopicRouter& tr = _topicRouters[t];
        if (tr.valid) {
            uint32_t drops = 0;
            for (int i = 0; i < tr.subCount; i++) {
                drops += tr.subs[i].dropCount;
            }
            Serial.printf("Topic %d: %d subs, %lu drops\n", t, tr.subCount, drops);
        }
    }
    Serial.println("========================");
}
