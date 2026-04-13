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
// ограничен PARSE_CHUNK_SIZE, чтобы быстро возвращаться к SerialBT.read().
//
// ВЕРСИЯ: 6.8.1 — Chunked processing, setRxBufferSize(16KB)
// -----------------------------------------------------------------------------

#include "bt_transport.h"
#include "data_router.h"
#include "topics.h"
#include <BluetoothSerial.h>

static BluetoothSerial SerialBT;

// =============================================================================
// Ring Buffer — общий, 8KB для burst-proof
// =============================================================================

#define RX_RING_SIZE  8192

static uint8_t rxRing[RX_RING_SIZE];
static size_t rxHead = 0;
static size_t rxTail = 0;

inline void rbPush(uint8_t c) {
    size_t next = (rxHead + 1) % RX_RING_SIZE;
    if (next != rxTail) {
        rxRing[rxHead] = c;
        rxHead = next;
    } else {
        // Переполнение — сдвигаем хвост (перезапись старых данных)
        rxTail = (rxTail + 1) % RX_RING_SIZE;
        rxRing[rxHead] = c;
        rxHead = next;
#if DEBUG_LOG
        Serial.println("[BT RX] Ring overflow");
#endif
    }
}

inline bool rbPop(uint8_t &c) {
    if (rxTail == rxHead) return false;
    c = rxRing[rxTail];
    rxTail = (rxTail + 1) % RX_RING_SIZE;
    return true;
}

// =============================================================================
// Chunked processing — макс. байт за один проход парсера
// =============================================================================

#define PARSE_CHUNK_SIZE 1024

// =============================================================================
// Состояние задачи
// =============================================================================

static TaskHandle_t    btTaskHandle = NULL;
static bool            wasConnected = false;
static bool            isRunningFlag = false;
static unsigned long   lastHeartbeat = 0;
static QueueHandle_t   txQueue = NULL;

// Мьютекс для защиты SerialBT (btSend + btTransportTask — разные потоки)
static StaticSemaphore_t sbbMutexBuf;
static SemaphoreHandle_t serialBtMutex = NULL;

// =============================================================================
// btTransportTask — одна задача, но с chunked processing
// =============================================================================

void btTransportTask(void* parameter) {
    (void)parameter;
    isRunningFlag = true;
    DataRouter& dr = DataRouter::getInstance();

    // Исходящие данные (OVERWRITE — не блокируем отправку)
    txQueue = xQueueCreate(1, 512);
    dr.subscribe(TOPIC_MSG_OUTGOING, txQueue, QueuePolicy::OVERWRITE);

    // Мьютекс для SerialBT (создаём один раз)
    if (!serialBtMutex) {
        serialBtMutex = xSemaphoreCreateMutexStatic(&sbbMutexBuf);
    }

    // Временный буфер для чтения из BT
    uint8_t temp[512];

    // Буфер для сборки строки (статический — не на стеке)
    // 4096 — достаточно для OTA-чанка JSON (~1420 байт при bin=1368 base64)
    static char lineBuffer[4096];
    static size_t linePos = 0;

#if DEBUG_LOG
    Serial.println("[BT Transport] Task running (chunked RX-first)");
#endif

    while (1) {
        lastHeartbeat = millis();

        // --- 1. Статус подключения ---
        bool isConnected = SerialBT.hasClient();
        if (isConnected != wasConnected) {
            wasConnected = isConnected;
            dr.publish(TOPIC_TRANSPORT_STATUS, isConnected);
        }

        // --- 2. RX: Чтение из Bluetooth (ПРИОРИТЕТ №1) ---
        if (isConnected && SerialBT.available()) {
            size_t len = SerialBT.readBytes((char*)temp, sizeof(temp));
            for (size_t i = 0; i < len; i++) {
                rbPush(temp[i]);
            }
            // Не парсим! Сразу возвращаемся к началу while(1) — читаем дальше.
        }

        // --- 3. Парсинг: порционный (не более PARSE_CHUNK_SIZE за проход) ---
        int processed = 0;
        uint8_t c;

        while (rbPop(c) && processed < PARSE_CHUNK_SIZE) {
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
#if DEBUG_LOG
                    Serial.println("[BT RX] Line buffer overflow");
#endif
                }
            }
        }

        // --- 4. TX: TOPIC_MSG_OUTGOING → Android (без ожидания) ---
        if (isConnected && txQueue) {
            char txBuffer[512];
            if (xQueueReceive(txQueue, txBuffer, 0) == pdTRUE) {
                if (xSemaphoreTake(serialBtMutex, portMAX_DELAY) == pdTRUE) {
                    SerialBT.print(txBuffer);
                    SerialBT.print('\n');
                    xSemaphoreGive(serialBtMutex);
                }
            }
        }

        // Пауза 1 тик — даём другим задачам поработать
        vTaskDelay(1);
    }
}

// =============================================================================
// Управление
// =============================================================================

void btTransportStart(const char* deviceName) {
    if (btTaskHandle) return;

    // Задержка перед инициализацией BT — обход бага ESP-IDF HCI HAL crash
#if DEBUG_LOG
    Serial.printf("[BT Transport] Delaying init to avoid HCI crash...\n");
#endif
    delay(500);

#if DEBUG_LOG
    Serial.printf("[BT Transport] Initializing '%s'...\n", deviceName);
#endif
    if (!SerialBT.begin(deviceName)) {
        Serial.println("[BT Transport] Init FAILED!");
        return;
    }
    SerialBT.setTimeout(1);  // 1ms — readBytes не блокируется дольше
#if DEBUG_LOG
    Serial.printf("[BT Transport] Started: '%s'\n", deviceName);
#endif

    wasConnected = SerialBT.hasClient();
    DataRouter::getInstance().publish(TOPIC_TRANSPORT_STATUS, wasConnected);

    // Ядро 1, приоритет 2
    xTaskCreatePinnedToCore(btTransportTask, "BT_Transport", 8192, NULL, 2, &btTaskHandle, 1);
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

bool btSend(const char* data) {
    if (!SerialBT.hasClient()) return false;
    bool result = false;
    if (serialBtMutex && xSemaphoreTake(serialBtMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        size_t len = SerialBT.print(data);
        if (len > 0) len += SerialBT.print('\n');
        result = (len > 0);
        xSemaphoreGive(serialBtMutex);
    }
    return result;
}
