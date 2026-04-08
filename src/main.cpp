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
#include "data_bus.h"
#include "topics.h"
#include "app_config.h"
#include "bt_transport.h"

// Внешние декларации
extern void simulatorStart();     extern void simulatorStop();     extern bool simulatorIsRunning();
extern void storageStart();       extern void storageStop();       extern bool storageIsRunning();
extern void calculatorStart();    extern void calculatorStop();    extern bool calculatorIsRunning();
extern void protocolStart();      extern void protocolStop();      extern bool protocolIsRunning();
extern void btTransportStart(const char*); extern void btTransportStop();
extern void klineStart();         extern void klineStop();         extern bool klineIsRunning();
extern void climateStart();       extern void climateStop();       extern bool climateIsRunning();
extern void oledStart();          extern void oledStop();          extern bool oledIsRunning();

static void restartSimulator()  { simulatorStop();  delay(100); simulatorStart(); }
static void restartStorage()    { storageStop();    delay(100); storageStart(); }
static void restartCalculator() { calculatorStop(); delay(100); calculatorStart(); }
static void restartProtocol()   { protocolStop();   delay(100); protocolStart(); }
static void restartKline()      { klineStop();      delay(100); klineStart(); }
static void restartClimate()    { climateStop();    delay(100); climateStart(); }
static void restartDisplay()    { oledStop();       delay(100); oledStart(); }

// =============================================================================
// setup
// =============================================================================

void setup() {
    Serial.begin(115200);
    delay(2000);

    Serial.println();
    Serial.println("=== Car BKV2 Starting ===");
    Serial.printf("Firmware: v%s\n", FW_VERSION_STR);
    Serial.println("Architecture: Queue-based Pub/Sub\n");

    // 1. DataBus
    Serial.println("[SETUP] 1/9: DataBus...");
    DataBus::getInstance().begin();
    delay(100);

    // 2. Simulator — ОТКЛЮЧЁН (тест BT)
    Serial.println("[SETUP] 2/9: Simulator... SKIPPED");
    // simulatorStart();
    delay(100);

    // 3. Storage — ОТКЛЮЧЁН (тест BT)
    Serial.println("[SETUP] 3/9: Storage... SKIPPED");
    // storageStart();
    delay(100);

    // 4. Calculator — ОТКЛЮЧЁН (тест BT)
    Serial.println("[SETUP] 4/9: Calculator... SKIPPED");
    // calculatorStart();
    delay(100);

    // 5. Protocol
    Serial.println("[SETUP] 5/9: Protocol...");
    protocolStart();
    delay(100);

    // 6. BT Transport (простая версия, poll из loop)
    Serial.println("[SETUP] 6/9: BT Transport...");
    btTransportStart("Car Simulator");
    delay(100);

    // 7. K-Line (отключено для отладки BT)
    Serial.println("[SETUP] 7/9: K-Line... SKIPPED");
    // klineStart();
    delay(100);

    // 8. Climate (отключено для отладки BT)
    Serial.println("[SETUP] 8/9: Climate... SKIPPED");
    // climateStart();
    delay(100);

    // 9. OLED (отключено для отладки BT)
    Serial.println("[SETUP] 9/9: OLED... SKIPPED");
    // oledStart();

    Serial.println("\n=== Setup Complete ===\n");
}

// =============================================================================
// loop — Диспетчер процессов
// =============================================================================
//
// Примечание: при отключении модулей в setup() — отключить их и здесь.
//

void loop() {
    static unsigned long lastFast = 0, lastMed = 0, lastLow = 0;
    unsigned long now = millis();

    // КРИТИЧЕСКИЕ (100 мс) — Simulator отключён для теста BT
    if (now - lastFast >= 100) {
        lastFast = now;
        // if (!simulatorIsRunning()) {
        //     Serial.println("[LOOP] CRITICAL: Simulator crashed! Restarting...");
        //     restartSimulator();
        // }
    }

    // ВЫСОКИЙ (500 мс) — Calculator отключён для теста BT
    if (now - lastMed >= 500) {
        lastMed = now;
        // if (!calculatorIsRunning()) {
        //     Serial.println("[LOOP] HIGH: Calculator crashed! Restarting...");
        //     restartCalculator();
        // }
    }

    vTaskDelay(50 / portTICK_PERIOD_MS);
}
