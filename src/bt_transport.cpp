// -----------------------------------------------------------------------------
// bt_transport.cpp
// Транспортный уровень Bluetooth Classic (SPP).
// -----------------------------------------------------------------------------

#include "bt_transport.h"
#include "data_bus.h"
#include "topics.h"
#include <BluetoothSerial.h>

static BluetoothSerial SerialBT;

static TaskHandle_t    btTaskHandle = NULL;
static bool            wasConnected = false;
static bool            isRunningFlag = false;
static unsigned long   lastHeartbeat = 0;
static char            rxBuffer[256];

// =============================================================================
// btTransportTask — Задача
// =============================================================================

void btTransportTask(void* parameter) {
    (void)parameter;
    isRunningFlag = true;
    DataBus& db = DataBus::getInstance();

    // Подписка на исходящие данные (FIFO_DROP, depth=5)
    // ACK-квитанции и телеметрия идут в одной очереди.
    // FIFO_DROP, depth=3 — достаточно для буферизации ACK + телеметрии
    SubscriberOpts txOpts = {QUEUE_FIFO_DROP, 3, false};
    QueueHandle_t txQueue = db.subscribe(TOPIC_MSG_OUTGOING, txOpts);

    Serial.println("[BT Transport] Task running (Queue-based)");

    while (1) {
        lastHeartbeat = millis();

        bool isConnected = SerialBT.hasClient();
        if (isConnected != wasConnected) {
            wasConnected = isConnected;
            db.publish(TOPIC_TRANSPORT_STATUS, isConnected);
            Serial.printf("[BT Transport] %s\n", isConnected ? "CONNECTED" : "DISCONNECTED");
        }

        if (SerialBT.available()) {
            int len = SerialBT.readBytesUntil('\n', rxBuffer, sizeof(rxBuffer) - 1);
            rxBuffer[len] = '\0';

            char* start = rxBuffer;
            while (*start == ' ' || *start == '\r' || *start == '\t') start++;
            char* end = start + strlen(start) - 1;
            while (end > start && (*end == ' ' || *end == '\r' || *end == '\t')) *end-- = '\0';

            if (strlen(start) > 0) {
                db.publish(TOPIC_MSG_INCOMING, start);
            }
        }

        // --- TX: DataBus → SerialBT ---
        // SerialBT.availableForWrite() не работает для SPP — всегда 0.
        // Отправляем напрямую, проверяем результат записи.
        if (SerialBT.hasClient()) {
            BusMessage msg;
            if (txQueue && xQueueReceive(txQueue, &msg, pdMS_TO_TICKS(50)) == pdTRUE) {
                if (msg.type == TYPE_STRING) {
                    // Данные уже содержат \n от Protocol. Используем print(),
                    // чтобы избежать двойного \r\n от println().
                    size_t sent = SerialBT.print(msg.value.s);
                    if (sent == 0) {
                        // SPP буфер переполнен — Android не потребляет данные.
                        // Не логируем каждый раз, чтобы не спамить Serial.
                        static unsigned long lastCongestionLog = 0;
                        unsigned long now = millis();
                        if (now - lastCongestionLog > 5000) {
                            lastCongestionLog = now;
                            Serial.printf("[BT TX] SPP congested! Android не читает (%zu/%zu sent)\n",
                                          sent, strlen(msg.value.s));
                        }
                    }
                    // --- Логирование ack_id (только при успешной отправке) ---
                    if (sent > 0) {
                        static int lastAckId = -1;
                        static int txCount = 0;
                        txCount++;
                        const char* ackPtr = strstr(msg.value.s, "\"ack_id\":");
                        if (ackPtr) {
                            ackPtr += 9;
                            int currentAck = atoi(ackPtr);
                            if (currentAck != lastAckId) {
                                lastAckId = currentAck;
                                Serial.printf("[BT TX] #%d ack_id=%d\n", txCount, lastAckId);
                            }
                        }
                    }
                    // --- Конец логирования ---
                }
                busMessageFree(&msg);
            }
        }

        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

// =============================================================================
// Управление
// =============================================================================

void btTransportStart(const char* deviceName) {
    if (btTaskHandle) return;

    // Задержка перед инициализацией BT — обход бага ESP-IDF HCI HAL crash
    Serial.printf("[BT Transport] Delaying init to avoid HCI crash...\n");
    delay(500);

    Serial.printf("[BT Transport] Initializing '%s'...\n", deviceName);
    if (!SerialBT.begin(deviceName)) {
        Serial.println("[BT Transport] Init FAILED!");
        return;
    }
    Serial.printf("[BT Transport] Started: '%s'\n", deviceName);

    wasConnected = SerialBT.hasClient();
    DataBus::getInstance().publish(TOPIC_TRANSPORT_STATUS, wasConnected);

    xTaskCreatePinnedToCore(btTransportTask, "BT_Transport", 4096, NULL, 2, &btTaskHandle, 1);
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
    return SerialBT.print(data) > 0;
}

