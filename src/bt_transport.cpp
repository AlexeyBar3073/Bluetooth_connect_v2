// -----------------------------------------------------------------------------
// bt_transport.cpp
// Транспортный уровень Bluetooth Classic (SPP) + OTA обновление прошивки.
//
// Режимы работы:
//   1. Обычный — JSON команды, телеметрия
//   2. OTA — приём прошивки чанками, запись в flash
//
// ВЕРСИЯ: 6.4.0 — OTA через Bluetooth Serial
// -----------------------------------------------------------------------------

#include "bt_transport.h"
#include "data_router.h"
#include "topics.h"
#include "commands.h"
#include "ota_handler.h"
#include <BluetoothSerial.h>

BluetoothSerial SerialBT;  // Глобальный (не static) — используется в ota_handler.cpp

static TaskHandle_t    btTaskHandle = NULL;
static bool            wasConnected = false;
static bool            isRunningFlag = false;
static unsigned long   lastHeartbeat = 0;

// --- Режим OTA ---
static bool            otaMode = false;
static char            rxBuffer[256];
static uint8_t         otaBuffer[2048];  // Буфер для OTA данных

// --- Очереди (создаются модулем, регистрируются в DataRouter) ---
static QueueHandle_t   txQueue = NULL;       // Входящий JSON от Android → BT → шина
static QueueHandle_t   cmdQueue = NULL;      // Команды из шины (OTA)

// =============================================================================
// processOTA — обработка OTA команд
// =============================================================================

static void processOTA() {
    if (SerialBT.available()) {
        size_t len = SerialBT.readBytes(otaBuffer, sizeof(otaBuffer));
        otaHandle(otaBuffer, len);
    }
}

// =============================================================================
// enterOTAMode — войти в режим OTA
// =============================================================================

void enterOTAMode() {
    if (otaMode) return;

    Serial.println("[BT Transport] === ENTERING OTA MODE ===");
    Serial.println("[BT Transport] Stopping non-critical tasks...");

    // Остановить задачи, которые могут мешать OTA
    // Protocol, Calculator, Simulator/RealEngine — останавливаем
    extern void protocolStop();
    extern void calculatorStop();
    extern void simulatorStop();
    extern void realEngineStop();
    extern void klineStop();
    extern void climateStop();

    protocolStop();
    calculatorStop();
    simulatorStop();
    realEngineStop();
    klineStop();
    climateStop();

    otaMode = true;

    // Ответ Android
    SerialBT.write(OTA_RSP_READY);
    SerialBT.println("\n[OTA] Ready to receive firmware");

    Serial.println("[BT Transport] OTA MODE ACTIVE");
}

// =============================================================================
// btTransportTask — Задача
// =============================================================================

void btTransportTask(void* parameter) {
    (void)parameter;
    isRunningFlag = true;
    DataRouter& dr = DataRouter::getInstance();

    // Создаём очередь для исходящих данных (OVERWRITE, depth=1 — всегда актуальное)
    txQueue = xQueueCreate(1, 512);  // 1 слот × 512 байт (полный JSON)
    dr.subscribe(TOPIC_MSG_OUTGOING, txQueue, QueuePolicy::OVERWRITE);

    // Подписка на команды (OTA)
    cmdQueue = xQueueCreate(5, sizeof(uint8_t));
    dr.subscribe(TOPIC_CMD, cmdQueue, QueuePolicy::FIFO_DROP);

#if DEBUG_LOG
    Serial.println("[BT Transport] Task running (DataRouter-based + OTA)");
#endif

    while (1) {
        lastHeartbeat = millis();

        // --- Проверка команд из шины (OTA) ---
        if (cmdQueue) {
            uint8_t cmd;
            while (xQueueReceive(cmdQueue, &cmd, 0) == pdTRUE) {
                if ((Command)cmd == CMD_ENTER_OTA_MODE) {
                    enterOTAMode();
                }
            }
        }

        // --- Статус подключения ---
        bool isConnected = SerialBT.hasClient();
        if (isConnected != wasConnected) {
            wasConnected = isConnected;
            dr.publish(TOPIC_TRANSPORT_STATUS, isConnected);
#if DEBUG_LOG
            Serial.printf("[BT Transport] %s\n", isConnected ? "CONNECTED" : "DISCONNECTED");
#endif
        }

        // --- Режим OTA ---
        if (otaMode) {
            processOTA();
            vTaskDelay(10 / portTICK_PERIOD_MS);
            continue;
        }

        // --- Обычный режим: RX ---
        if (SerialBT.available()) {
            int len = SerialBT.readBytesUntil('\n', rxBuffer, sizeof(rxBuffer) - 1);
            rxBuffer[len] = '\0';

            char* start = rxBuffer;
            while (*start == ' ' || *start == '\r' || *start == '\t') start++;
            char* end = start + strlen(start) - 1;
            while (end > start && (*end == ' ' || *end == '\r' || *end == '\t')) *end-- = '\0';

            if (strlen(start) > 0) {
                dr.publishString(TOPIC_MSG_INCOMING, start);
            }
        }

        // --- Обычный режим: TX ---
        if (SerialBT.hasClient()) {
            char txBuffer[512];
            if (txQueue && xQueueReceive(txQueue, txBuffer, pdMS_TO_TICKS(50)) == pdTRUE) {
                size_t sent = SerialBT.print(txBuffer);
                if (sent == 0) {
                    static unsigned long lastCongestionLog = 0;
                    unsigned long now = millis();
                    if (now - lastCongestionLog > 5000) {
                        lastCongestionLog = now;
#if DEBUG_LOG
                        Serial.printf("[BT TX] SPP congested! Android не читает (%zu/%zu sent)\n",
                                      sent, strlen(txBuffer));
#endif
                    }
                }
                // --- Логирование ack_id ---
                if (sent > 0) {
                    static int lastAckId = -1;
                    static int txCount = 0;
                    txCount++;
                    const char* ackPtr = strstr(txBuffer, "\"ack_id\":");
                    if (ackPtr) {
                        ackPtr += 9;
                        int currentAck = atoi(ackPtr);
                        if (currentAck != lastAckId) {
                            lastAckId = currentAck;
#if DEBUG_LOG
                            Serial.printf("[BT TX] #%d ack_id=%d\n", txCount, lastAckId);
#endif
                        }
                    }
                }
                // --- Конец логирования ---
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
#if DEBUG_LOG
    Serial.printf("[BT Transport] Started: '%s'\n", deviceName);
#endif

    wasConnected = SerialBT.hasClient();
    DataRouter::getInstance().publish(TOPIC_TRANSPORT_STATUS, wasConnected);

    // Ядро 1 — BT_Transport (Bluetooth communication + OTA)
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
    if (!SerialBT.hasClient() || otaMode) return false;
    return SerialBT.print(data) > 0;
}
