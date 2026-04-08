#include <Arduino.h>
#include <unity.h>
#include "data_bus.h"
#include "topics.h"

// =============================================================================
// DataBus Unit Tests для ESP32
// =============================================================================

// Глобальный экземпляр для тестов
DataBus* testBus = nullptr;

// -----------------------------------------------------------------------------
// Setup / Teardown
// -----------------------------------------------------------------------------

void setUp(void) {
    testBus = &DataBus::getInstance();
}

void tearDown(void) {
    // Нет очистки для простоты
}

// -----------------------------------------------------------------------------
// Тест 1: Singleton
// -----------------------------------------------------------------------------
void test_singleton() {
    DataBus& bus1 = DataBus::getInstance();
    DataBus& bus2 = DataBus::getInstance();
    TEST_ASSERT_EQUAL_PTR(&bus1, &bus2);
}

// -----------------------------------------------------------------------------
// Тест 2: Публикация/получение float
// -----------------------------------------------------------------------------
void test_publish_get_float() {
    testBus->publish(TOPIC_SPEED, 60.5f);
    testBus->publish(TOPIC_RPM, 2500.0f);
    
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 60.5f, testBus->getFloat(TOPIC_SPEED));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 2500.0f, testBus->getFloat(TOPIC_RPM));
}

// -----------------------------------------------------------------------------
// Тест 3: Публикация/получение bool
// -----------------------------------------------------------------------------
void test_publish_get_bool() {
    testBus->publish(TOPIC_ENGINE_RUNNING, true);
    TEST_ASSERT_TRUE(testBus->getBool(TOPIC_ENGINE_RUNNING));
    
    testBus->publish(TOPIC_ENGINE_RUNNING, false);
    TEST_ASSERT_FALSE(testBus->getBool(TOPIC_ENGINE_RUNNING));
}

// -----------------------------------------------------------------------------
// Тест 4: Значения по умолчанию
// -----------------------------------------------------------------------------
void test_default_values() {
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, testBus->getFloat(TOPIC_DEBUG));
    TEST_ASSERT_FALSE(testBus->getBool(TOPIC_DEBUG));
}

// -----------------------------------------------------------------------------
// Тест 5: Callback
// -----------------------------------------------------------------------------
static float receivedValue = 0.0f;
static bool callbackCalled = false;

void testCallback(Topic topic, float value) {
    receivedValue = value;
    callbackCalled = true;
}

void test_callback() {
    receivedValue = 0.0f;
    callbackCalled = false;
    
    testBus->subscribeFloat(TOPIC_SPEED, testCallback);
    testBus->publish(TOPIC_SPEED, 100.5f);
    delay(10);
    
    TEST_ASSERT_TRUE(callbackCalled);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.5f, receivedValue);
}

// -----------------------------------------------------------------------------
// Arduino setup / loop
// -----------------------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    delay(2000);
    
    Serial.println("\n=================================");
    Serial.println("DataBus Unit Tests (ESP32)");
    Serial.println("=================================\n");
    
    UNITY_BEGIN();
    
    Serial.println("Running: test_singleton");
    RUN_TEST(test_singleton);
    
    Serial.println("Running: test_publish_get_float");
    RUN_TEST(test_publish_get_float);
    
    Serial.println("Running: test_publish_get_bool");
    RUN_TEST(test_publish_get_bool);
    
    Serial.println("Running: test_default_values");
    RUN_TEST(test_default_values);
    
    Serial.println("Running: test_callback");
    RUN_TEST(test_callback);
    
    Serial.println("\n=================================");
    Serial.println("All tests complete!");
    Serial.println("=================================\n");
    
    UNITY_END();
}

void loop() {
    vTaskDelay(portMAX_DELAY);
}
