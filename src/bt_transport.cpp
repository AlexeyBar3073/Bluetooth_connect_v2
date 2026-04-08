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

    // Подписка на исходящие данные (OVERWRITE, depth=1)
    // Всегда актуальное сообщение. ACK отправляется в составе телеметрии.
    SubscriberOpts txOpts = {QUEUE_OVERWRITE, 1, false};
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
        // Отправляем без проверки буфера. Если буфер полон — println() вернёт 0.
        if (SerialBT.hasClient()) {
            BusMessage msg;
            if (txQueue && xQueueReceive(txQueue, &msg, 0) == pdTRUE) {
                if (msg.type == TYPE_STRING) {
                    size_t sent = SerialBT.println(msg.value.s);
                    // --- Логирование ack_id для отладки ---
                    static int lastAckId = -1;
                    static int txCount = 0;
                    txCount++;

                    // Ищем "ack_id":N (число)
                    const char* ackPtr = strstr(msg.value.s, "\"ack_id\":");
                    if (ackPtr) {
                        ackPtr += 9;  // пропускаем "\"ack_id\":"
                        int currentAck = atoi(ackPtr);
                        if (currentAck != lastAckId) {
                            lastAckId = currentAck;
                            Serial.printf("[BT TX] #%d ack_id=%d sent=%zu\n", txCount, lastAckId, sent);
                        }
                    } else if (txCount <= 3) {
                        Serial.printf("[BT TX] #%d (нет ack_id) sent=%zu: %s\n", txCount, sent, msg.value.s);
                    }
                    // --- Конец логирования ---
                }
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
