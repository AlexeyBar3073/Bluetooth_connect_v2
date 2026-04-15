// -----------------------------------------------------------------------------
// bt_transport.cpp
// Транспортный уровень Bluetooth Classic (SPP).
//
// Архитектура: Bluetooth → RingBuffer → Chunked Parser → DataRouter
//
// Единственная задача: приём байт от Android → публикация в TOPIC_MSG_INCOMING,
// приём из TOPIC_MSG_OUTGOING → отправка байт в Android.
// Не знает про телеметрию, команды, OTA.
//
// Ключевой принцип: RX имеет приоритет над парсингом. Цикл обработки
// ограничен адаптивным размером чанка, чтобы быстро возвращаться к SerialBT.read().
//
// Оптимизации для OTA:
//   - Watermark контроль (30%/70%/90%) — backpressure при переполнении
//   - Адаптивный размер парсинга (512-4096) — быстрее при больших данных
//   - TX-очередь 2048 байт — пиковые нагрузки OTA-чанков
//   - Динамическая задержка — 0 при >70% заполнения
//
// ВЕРСИЯ: 6.8.14 — BT Transport: watermark, backpressure, adaptive parse
// -----------------------------------------------------------------------------

#include "bt_transport.h"
#include "data_router.h"
#include "topics.h"
#include "app_config.h"
#include "debug.h"
#include <BluetoothSerial.h>

static BluetoothSerial SerialBT;

// =============================================================================
// Ring Buffer — 8KB для burst-proof
// =============================================================================

#define RX_RING_SIZE  8192

static uint8_t rxRing[RX_RING_SIZE];
static size_t rxHead = 0;
static size_t rxTail = 0;

// =============================================================================
// Watermark контроль и backpressure
// =============================================================================

#define RX_WATERMARK_LOW   (RX_RING_SIZE * 3 / 10)  // 30% — можно принимать
#define RX_WATERMARK_HIGH  (RX_RING_SIZE * 7 / 10)  // 70% — пора притормозить
#define RX_WATERMARK_CRIT  (RX_RING_SIZE * 9 / 10)  // 90% — аварийный сброс

static bool rxBackpressure = false;  // Флаг паузы приёма

inline size_t rbUsed() {
    return (rxHead >= rxTail) ? (rxHead - rxTail) : (RX_RING_SIZE - rxTail + rxHead);
}

inline size_t rbFree() {
    return RX_RING_SIZE - rbUsed() - 1;
}

inline bool rbPush(uint8_t c) {
    size_t next = (rxHead + 1) % RX_RING_SIZE;
    if (next == rxTail) return false;  // Переполнение — не перезаписываем
    rxRing[rxHead] = c;
    rxHead = next;
    return true;
}

inline bool rbPop(uint8_t &c) {
    if (rxTail == rxHead) return false;
    c = rxRing[rxTail];
    rxTail = (rxTail + 1) % RX_RING_SIZE;
    return true;
}

// =============================================================================
// Адаптивный размер парсинга
// =============================================================================

#define PARSE_CHUNK_MIN  512
#define PARSE_CHUNK_MAX  4096

inline size_t getAdaptiveParseChunk() {
    size_t used = rbUsed();
    if (used > RX_WATERMARK_CRIT) return PARSE_CHUNK_MAX;
    if (used > RX_WATERMARK_HIGH)  return PARSE_CHUNK_MAX / 2;
    return PARSE_CHUNK_MIN;
}

// =============================================================================
// Состояние задачи
// =============================================================================

static TaskHandle_t    btTaskHandle = NULL;
static bool            wasConnected = false;
static bool            isRunningFlag = false;
static unsigned long   lastHeartbeat = 0;
static QueueHandle_t   txQueue = NULL;

// =============================================================================
// btTransportTask — одна задача, но с chunked processing
// =============================================================================

void btTransportTask(void* parameter) {
    (void)parameter;
    isRunningFlag = true;
    DataRouter& dr = DataRouter::getInstance();

    // Исходящие данные (OVERWRITE — не блокируем отправку)
    txQueue = xQueueCreate(1, 2048);  // Увеличено до 2048 для пиковых нагрузок OTA
    dr.subscribe(TOPIC_MSG_OUTGOING, txQueue, QueuePolicy::OVERWRITE);

    // Временный буфер для чтения из BT — static, чтобы не占用 стек задачи
    static uint8_t temp[512];

    // Буфер для сборки строки — 2048 достаточно для OTA-чанка JSON (~1450 байт)
    static char lineBuffer[2048];
    static size_t linePos = 0;

    DBG_PRINTLN("[BT Transport] Task running (watermark, adaptive parse)");

    while (1) {
        lastHeartbeat = millis();

        // --- 1. Статус подключения ---
        bool isConnected = SerialBT.hasClient();
        if (isConnected != wasConnected) {
            wasConnected = isConnected;
            dr.publish(TOPIC_TRANSPORT_STATUS, isConnected);
        }

        // --- 2. RX: Чтение из Bluetooth (ПРИОРИТЕТ №1, backpressure-aware) ---
        if (isConnected && !rxBackpressure && SerialBT.available()) {
            size_t available = SerialBT.available();
            size_t toRead = (available > sizeof(temp)) ? sizeof(temp) : available;
            if (toRead > rbFree()) toRead = rbFree();  // Не читать больше свободного места

            if (toRead > 0) {
                size_t actuallyRead = SerialBT.readBytes((char*)temp, toRead);
                for (size_t i = 0; i < actuallyRead; i++) {
                    if (!rbPush(temp[i])) {
                        rxBackpressure = true;
                        DBG_PRINTF("[BT] Buffer full (%d/%d), backpressure ON\n",
                                  (int)rbUsed(), RX_RING_SIZE);
                        break;
                    }
                }
            }
        }

        // --- 3. Парсинг: адаптивный размер ---
        size_t parseLimit = getAdaptiveParseChunk();
        int processed = 0;
        uint8_t c;

        while (rbPop(c) && processed < (int)parseLimit) {
            processed++;

            if (c == '\r') continue;

            if (c == '\n') {
                if (linePos > 0) {
                    lineBuffer[linePos] = '\0';

                    char* start = lineBuffer;
                    while (*start == ' ' || *start == '\t') start++;

                    if (*start) {
                        dr.publishString(TOPIC_MSG_INCOMING, start);
                    }

                    linePos = 0;
                }
            } else {
                if (linePos < sizeof(lineBuffer) - 1) {
                    lineBuffer[linePos++] = (char)c;
                } else {
                    linePos = 0;
                    DBG_PRINTLN("[BT RX] Line buffer overflow");
                }
            }
        }

        // --- Backpressure: возобновление при draining ---
        if (rxBackpressure && rbUsed() < RX_WATERMARK_LOW) {
            rxBackpressure = false;
            DBG_PRINTLN("[BT] Buffer drained, backpressure OFF");
        }

        // --- 4. TX: TOPIC_MSG_OUTGOING → Android (без ожидания) ---
        if (isConnected && txQueue) {
            static char txBuffer[2048];  // static — не占用 стек цикла
            if (xQueueReceive(txQueue, txBuffer, 0) == pdTRUE) {
                size_t len = strlen(txBuffer) + 1;  // +1 за '\n'
                // Отправляем только если в SPP-буфере есть место под всё сообщение.
                // Если места нет — пакет дропается (OVERWRITE: новый заменит старый).
                // Это предотвращает SPP Write Congested при заторах.
                int avail = SerialBT.availableForWrite();
                if (avail <= 0 || (size_t)avail >= len) {
                    SerialBT.print(txBuffer);
                    SerialBT.print('\n');
                }
            }
        }

        // --- Динамическая задержка: 0 при >70% заполнения, иначе 1 тик ---
        vTaskDelay(rbUsed() > RX_WATERMARK_HIGH ? 0 : 1);
    }
}

// =============================================================================
// Управление
// =============================================================================

void btTransportStart(const char* deviceName) {
    if (btTaskHandle) return;

    // Проверка кучи перед BT-init — Bluedroid требует ≥30 КБ
    if (ESP.getFreeHeap() < 30000) {
        DBG_PRINTF("[BT] Low heap (%u < 30000). Waiting for stabilization...\n", ESP.getFreeHeap());
        delay(2000);
        if (ESP.getFreeHeap() < 28000) {
            DBG_PRINTF("[BT] Init ABORTED (heap still too low: %u)\n", ESP.getFreeHeap());
            return;
        }
    }

    // Увеличенная задержка — обход бага ESP-IDF HCI HAL crash
    DBG_PRINTF("[BT Transport] Initializing '%s'...\n", deviceName);
    delay(800);

    DBG_PRINTF("[BT Transport] Free heap: %u, max_alloc: %u\n",
              (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

    if (!SerialBT.begin(deviceName)) {
        DBG_PRINTLN("[BT Transport] Init FAILED!");
        return;
    }
    SerialBT.setTimeout(1);  // 1ms — readBytes не блокируется дольше
    DBG_PRINTF("[BT Transport] Started: '%s'\n", deviceName);

    wasConnected = SerialBT.hasClient();
    DataRouter::getInstance().publish(TOPIC_TRANSPORT_STATUS, wasConnected);

    // Ядро 1, приоритет 2
    xTaskCreatePinnedToCore(btTransportTask, "BT_Transport", TASK_STACK_BT, NULL, 2, &btTaskHandle, 1);
}

void btTransportStop() {
    if (btTaskHandle) {
        vTaskDelete(btTaskHandle);
        btTaskHandle = NULL;
        isRunningFlag = false;
        SerialBT.end();
    }
}

bool btIsConnected() { return SerialBT.hasClient(); }
