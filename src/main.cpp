// -----------------------------------------------------------------------------
// main.cpp
// Точка входа и Loop-диспетчер процессов.
//
// Порядок инициализации:
//   1. DataBus → 2. Simulator → 3. Storage → 4. Calculator
//   5. Protocol → 6. BT Transport → 7. K-Line → 8. Climate → 9. OLED
//
// Loop: heartbeat-мониторинг, перезапуск при падении.
//
// ВЕРСИЯ: 5.0.0 — MAJOR: Queue-архитектура, Loop-диспетчер
// -----------------------------------------------------------------------------

#include <Arduino.h>
#include "data_router.h"
#include "topics.h"
#include "app_config.h"
#include "bt_transport.h"

// Внешние декларации — пока только те, что для теста DataRouter
extern void simulatorStart();     extern void simulatorStop();     extern bool simulatorIsRunning();
extern void storageStart();       extern void storageStop();       extern bool storageIsRunning();
extern void calculatorStart();    extern void calculatorStop();    extern bool calculatorIsRunning();
extern void protocolStart();      extern void protocolStop();      extern bool protocolIsRunning();
extern void oledStart();          extern void oledStop();          extern bool oledIsRunning();

static void restartSimulator()   { simulatorStop();  delay(100); simulatorStart(); }
static void restartStorage()     { storageStop();    delay(100); storageStart(); }
static void restartCalculator()  { calculatorStop(); delay(100); calculatorStart(); }
static void restartProtocol()    { protocolStop();   delay(100); protocolStart(); }
static void restartDisplay()     { oledStop();       delay(100); oledStart(); }

// =============================================================================
// setup
// =============================================================================

void setup() {
    Serial.begin(115200);
    delay(2000);

    Serial.println();
    Serial.println("=== Car BKV2 Starting ===");
    Serial.printf("Firmware: v%s\n", FW_VERSION_STR);
    Serial.println("Architecture: DataRouter (typed topics)\n");

    // 1. DataRouter
    Serial.println("[SETUP] 1/7: DataRouter...");
    DataRouter::getInstance().begin();
    delay(100);

    // 2. Storage (публикует данные — кэш для остальных)
    Serial.println("[SETUP] 2/6: Storage...");
    storageStart();
    delay(100);

    // 3. Simulator
    Serial.println("[SETUP] 3/6: Simulator...");
    simulatorStart();
    delay(100);

    // 4. Calculator
    Serial.println("[SETUP] 4/6: Calculator...");
    calculatorStart();
    delay(100);

    // 5. Protocol
    Serial.println("[SETUP] 5/6: Protocol...");
    protocolStart();
    delay(100);

    // 6. BT Transport
    Serial.println("[SETUP] 6/7: BT Transport...");
    btTransportStart("Car Simulator");
    delay(100);

    // 7. OLED
    Serial.println("[SETUP] 7/7: OLED...");
    oledStart();

    // --- Остальные модули временно отключены (K-Line, Climate) ---
    Serial.println("\n=== Setup Complete (Sim/Storage/Calc/Proto/BT/OLED) ===\n");
}

// =============================================================================
// loop — Диспетчер процессов (упрощённый)
// =============================================================================

void loop() {
    static unsigned long lastCheck = 0;
    unsigned long now = millis();

    // Мониторинг Simulator
    if (now - lastCheck >= 100) {
        lastCheck = now;
        if (!simulatorIsRunning()) {
            Serial.println("[LOOP] CRITICAL: Simulator crashed! Restarting...");
            restartSimulator();
        }
        if (!storageIsRunning()) {
            Serial.println("[LOOP] HIGH: Storage crashed! Restarting...");
            restartStorage();
        }
        if (!calculatorIsRunning()) {
            Serial.println("[LOOP] CRITICAL: Calculator crashed! Restarting...");
            restartCalculator();
        }
        if (!oledIsRunning()) {
            Serial.println("[LOOP] LOW: OLED crashed! Restarting...");
            restartDisplay();
        }
        if (!protocolIsRunning()) {
            Serial.println("[LOOP] CRITICAL: Protocol crashed! Restarting...");
            restartProtocol();
        }
    }

    vTaskDelay(50 / portTICK_PERIOD_MS);
}
