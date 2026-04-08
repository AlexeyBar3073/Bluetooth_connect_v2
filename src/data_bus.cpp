// -----------------------------------------------------------------------------
// data_bus.cpp
// Реализация шины данных на основе очередей FreeRTOS.
//
// Архитектура «почтовый ящик»:
//   publish() → для каждого подписчика: xQueueSend/xQueueOverwrite → очередь
//   Модуль: xQueueReceive(своя_очередь, &msg, timeout) — разбирает почту
//
// Преимущества:
// - publish() НЕ блокируется медленными получателями (~3-5 мкс)
// - Каждый подписчик читает в своём темпе
// - OVERWRITE гарантирует актуальность (depth=1, перезапись)
// - FIFO_DROP буферизует команды (depth=3-5, дроп при переполнении)
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
//   - Менять логику OVERWRITE vs FIFO_DROP (это архитектурное решение)
//   - Добавлять callback-и (нарушает принцип очередей)
//   - Блокировать publish() (должен быть ~3-5 мкс)
//   - Удалять публичные методы
//
// ВЕРСИЯ: 5.0.0 — MAJOR: Очередная архитектура шины
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
//
// Создаёт мьютекс для защиты внутренних структур.
// Очищает кэш (все значения невалидны).
// ДОЛЖЕН вызываться ПЕРЕД subscribe/publish.
//
void DataBus::begin() {
    _mutex = xSemaphoreCreateMutex();
    _subscriberCount = 0;
    for (int i = 0; i < TOPIC_COUNT; i++) {
        _cache[i].valid = false;
    }
    Serial.println("[DataBus] Initialized (Queue-based Pub/Sub)");
}

// =============================================================================
// _getQueueForTopic: Создание очереди с нужной политикой
// =============================================================================
//
// OVERWRITE → depth=1 (xQueueCreate(1, sizeof(BusMessage)))
// FIFO_DROP → depth=opts.depth (xQueueCreate(depth, sizeof(BusMessage)))
//
// Возвращает: QueueHandle_t или NULL при ошибке
//
QueueHandle_t DataBus::_getQueueForTopic(const SubscriberOpts& opts) {
    uint8_t depth = (opts.policy == QUEUE_OVERWRITE) ? 1 : opts.depth;
    return xQueueCreate(depth, sizeof(BusMessage));
}

// =============================================================================
// subscribe: Подписка на топик — создание очереди подписчика
// =============================================================================
//
// Алгоритм:
// 1. Блокируем мьютекс
// 2. Проверяем лимит подписчиков (50)
// 3. Создаём очередь с нужной политикой
// 4. Добавляем SubscriberSlot в массив
// 5. Если opts.retain=true — инжектируем кэшированное значение в очередь
// 6. Разблокируем мьютекс
// 7. Возвращаем QueueHandle_t
//
// Подписчик читает: xQueueReceive(queue, &msg, pdMS_TO_TICKS(timeout))
//
QueueHandle_t DataBus::subscribe(Topic topic, const SubscriberOpts& opts) {
    xSemaphoreTake(_mutex, portMAX_DELAY);

    if (_subscriberCount >= BUS_MAX_SUBSCRIBERS) {
        Serial.printf("[DataBus] ERROR: Max subscribers (%d)!\n", BUS_MAX_SUBSCRIBERS);
        xSemaphoreGive(_mutex);
        return NULL;
    }

    // Создаём очередь
    QueueHandle_t q = _getQueueForTopic(opts);
    if (!q) {
        Serial.println("[DataBus] ERROR: Queue creation failed!");
        xSemaphoreGive(_mutex);
        return NULL;
    }

    // Регистрируем подписчика
    _subscribers[_subscriberCount].topic = topic;
    _subscribers[_subscriberCount].queue = q;
    _subscribers[_subscriberCount].opts = opts;
    _subscribers[_subscriberCount].dropCount = 0;
    _subscriberCount++;

    xSemaphoreGive(_mutex);

    // Retain-инжекция: если есть кэшированное значение — кладём в очередь
    if (opts.retain) {
        BusMessage cached;
        if (getCached(topic, cached)) {
            xQueueSend(q, &cached, 0);
        }
    }

    return q;
}

// =============================================================================
// unsubscribe: Отписка — удаление очереди подписчика
// =============================================================================
//
// Находит SubscriberSlot по topic+queue, удаляет очередь,
// сдвигает оставющиеся элементы, уменьшает счётчик.
//
void DataBus::unsubscribe(Topic topic, QueueHandle_t queue) {
    xSemaphoreTake(_mutex, portMAX_DELAY);

    for (int i = 0; i < _subscriberCount; i++) {
        if (_subscribers[i].topic == topic && _subscribers[i].queue == queue) {
            vQueueDelete(_subscribers[i].queue);

            // Сдвигаем оставшиеся элементы
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
// _updateCache: Обновление кэша последнего значения
// =============================================================================
//
// Вызывается из publish() ПЕРЕД dispatch.
// Сохраняет копию BusMessage в _cache[topic].
// Используется для retain-инжекции при подписке новых подписчиков.
//
void DataBus::_updateCache(Topic topic, const BusMessage& msg) {
    _cache[topic].valid = true;
    memcpy(&_cache[topic].msg, &msg, sizeof(BusMessage));
}

// =============================================================================
// _dispatchToSubscribers: Рассылка сообщения всем подписчикам топика
// =============================================================================
//
// Для каждого подписчика с matching topic:
// - OVERWRITE: xQueueOverwrite() — атомарно перезаписывает ячейку (всегда OK)
// - FIFO_DROP: xQueueSend(..., 0) — если очередь полна → dropCount++
//
// Возвращает true если хотя бы один подписчик получил сообщение.
//
bool DataBus::_dispatchToSubscribers(Topic topic, const BusMessage& msg) {
    bool any = false;

    for (int i = 0; i < _subscriberCount; i++) {
        if (_subscribers[i].topic != topic) continue;

        SubscriberSlot& sub = _subscribers[i];
        BaseType_t result;

        if (sub.opts.policy == QUEUE_OVERWRITE) {
            // Перезаписываем — подписчик всегда получит последнее значение
            result = xQueueOverwrite(sub.queue, &msg);
            if (result == pdTRUE) any = true;
        } else {
            // FIFO: кладём в очередь, если полна — дроп
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
// getCached: Получение последнего значения из кэша
// =============================================================================
//
// Используется:
// - Для retain-инжекции при subscribe(retain=true)
// - Модулями для получения начальных значений (например, Calculator читает base)
//
// Возвращает true если кэш валиден, false если топик ещё не публиковался.
//
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
// getDropCount: Количество отброшенных сообщений для топика
// =============================================================================
//
// Суммирует dropCount всех подписчиков данного топика.
// Используется в Loop-диспетчере для мониторинга здоровья шины.
//
uint32_t DataBus::getDropCount(Topic topic) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    uint32_t total = 0;
    for (int i = 0; i < _subscriberCount; i++) {
        if (_subscribers[i].topic == topic) {
            total += _subscribers[i].dropCount;
        }
    }
    xSemaphoreGive(_mutex);
    return total;
}

// =============================================================================
// publish(float): Публикация числа с плавающей точкой
// =============================================================================
//
// Алгоритм:
// 1. Создать BusMessage (topic, type=TYPE_FLOAT, value.f)
// 2. Обновить кэш (_updateCache)
// 3. Разослать подписчикам (_dispatchToSubscribers)
// 4. Вернуть результат dispatch
//
// Время выполнения: ~3-5 мкс (зависит от кол-ва подписчиков)
//
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
// publish(int): Публикация целого числа
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
// publish(bool): Публикация логического значения
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
// publish(double): Публикация double (для одометра)
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
// publish(const char*): Публикация строки
// =============================================================================
//
// Копирует строку в BusMessage.value.s (до PAYLOAD_MAX-1 байт).
// Гарантирует null-termination.
//
bool DataBus::publish(Topic topic, const char* value) {
    BusMessage msg;
    msg.topic = topic;
    msg.type = TYPE_STRING;
    memset(&msg.value, 0, sizeof(msg.value));
    strncpy(msg.value.s, value, PAYLOAD_MAX - 1);
    msg.value.s[PAYLOAD_MAX - 1] = '\0';
    memset(&msg.cmd, 0, sizeof(CmdPayload));

    _updateCache(topic, msg);
    return _dispatchToSubscribers(topic, msg);
}

// =============================================================================
// publish(CmdPayload): Публикация команды
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
// publishPacket: Публикация бинарного пакета
// =============================================================================
//
// Копирует бинарные данные в BusMessage.value.s (до PAYLOAD_MAX-1 байт).
// Сохраняет размер в первом байте для восстановления при чтении.
//
// Подписчик читает:
//   BusMessage msg;
//   xQueueReceive(queue, &msg, timeout);
//   if (msg.type == TYPE_STRING) {
//       memcpy(&pack, msg.value.s, sizeof(pack));
//   }
//
bool DataBus::publishPacket(Topic topic, const void* data, size_t len) {
    if (len > PAYLOAD_MAX) {
        Serial.printf("[DataBus] Packet too large: %zu > %d\n", len, PAYLOAD_MAX);
        return false;
    }

    BusMessage msg;
    msg.topic = topic;
    msg.type = TYPE_STRING;
    memset(&msg.value, 0, sizeof(msg.value));
    memset(&msg.cmd, 0, sizeof(CmdPayload));

    // Копируем бинарные данные в value.s
    memcpy(msg.value.s, data, len);
    // Null-terminator не нужен для бинарных данных, но value.s[PAYLOAD_MAX-1] = '\0' уже стоит из memset

    _updateCache(topic, msg);
    return _dispatchToSubscribers(topic, msg);
}

// =============================================================================
// printStats: Статистика шины (для отладки)
// =============================================================================
//
// Выводит: кэш-валидность, подписчики, drop-счётчики.
// Вызывается из Loop каждые 10 секунд.
//
void DataBus::printStats() {
    Serial.println("=== DataBus Stats ===");

    // Кэш
    int cached = 0;
    for (int i = 0; i < TOPIC_COUNT; i++) {
        if (_cache[i].valid) cached++;
    }
    Serial.printf("Cached topics: %d/%d\n", cached, TOPIC_COUNT);

    // Подписчики
    Serial.printf("Subscribers: %d/%d\n", _subscriberCount, BUS_MAX_SUBSCRIBERS);

    // Дропы
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
