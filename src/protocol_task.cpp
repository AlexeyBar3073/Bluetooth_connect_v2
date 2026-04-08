// -----------------------------------------------------------------------------
// protocol_task.cpp
// Протокол связи с Android по Bluetooth (JSON-команды, фракционная телеметрия).
//
// Архитектура (Queue-based):
// - Подписка на EnginePack, TripPack, ServicePack, SettingsPack → очереди
// - Чтение из очередей → обновление локального кэша телеметрии
// - Подписка на TOPIC_MSG_INCOMING → парсинг JSON-команд
// - Фракционная отправка JSON: FAST 100мс, TRIP 500мс, SERVICE 1000мс
//
// -----------------------------------------------------------------------------
// ПРАВИЛА ФАЙЛА:
// -----------------------------------------------------------------------------
// ✅ можно:
//   - Добавлять новые команды
//   - Менять поля JSON
//   - Менять частоту фракционной отправки
//
// ❌ нельзя:
//   - Блокировать >10 мс
//   - Использовать String для outgoing JSON
//
// ВЕРСИЯ: 5.0.0 — MAJOR: Queue-архитектура, фракционный JSON
// -----------------------------------------------------------------------------

#include "protocol_task.h"
#include "data_bus.h"
#include "topics.h"
#include "packets.h"
#include "commands.h"
#include "bt_transport.h"
#include "app_config.h"
#include <ArduinoJson.h>

// =============================================================================
// Конфигурация
// =============================================================================
#define FAST_MS    100
#define TRIP_MS    500
#define SERVICE_MS 1000

// =============================================================================
// Глобальные переменные
// =============================================================================

static TaskHandle_t  taskHandle       = NULL;
static bool          isRunningFlag    = false;
static unsigned long lastHeartbeat    = 0;
static bool          isStreamingActive = false;

// --- Последний принятый msg_id (квитанция) — храним как int
// При получении команды: запоминаем msg_id
// При следующей отправке: добавляем ack_id → обнуляем
// Если квитанция не дошла — Android повторит запрос
static int lastMsgId = 0;

// --- Кэш телеметрии (обновляется из очередей пакетов) ---
static struct {
    float   speed = 0, rpm = 0, voltage = 0, inst_fuel = 0;
    bool    eng = false, hl = false, tcc = false;
    int8_t  gear = 0;
    char    sel[8] = "D";
    double  odo = 0;
    float   trip_a = 0, fuel_a = 0, trip_b = 0, fuel_b = 0;
    float   trip_cur = 0, fuel_cur = 0, fuel = 0, avg = 0;
    float   t_cool = 0, t_atf = 0, t_int = 0, t_ext = 0;
    uint8_t dtc_cnt = 0;
    char    dtc[64] = "";
    bool    tire = false, wash = false;
} tel;

// --- Кэш настроек ---
static struct {
    float   tank = 60.0f, inj_perf = 250.0f, spd_sig = 3.0f;
    uint8_t inj_cnt = 4, kl_proto = 0;
} cfg;

static char outBuffer[512];

// =============================================================================
// processEnginePack
// =============================================================================
static void processEnginePack(QueueHandle_t q) {
    BusMessage msg;
    if (xQueueReceive(q, &msg, 0) == pdTRUE && msg.type == TYPE_STRING) {
        EnginePack p;
        memcpy(&p, msg.value.s, sizeof(EnginePack));
        tel.speed = p.speed;
        tel.rpm = p.rpm;
        tel.voltage = p.voltage;
        tel.eng = p.engine_running;
        tel.hl = p.parking_lights;
        tel.inst_fuel = p.instant_fuel;
        tel.gear = p.gear;
        tel.tcc = p.tcc_lockup;
        strlcpy(tel.sel, p.selector_pos, sizeof(tel.sel));
    }
}

// =============================================================================
// processTripPack
// =============================================================================
static void processTripPack(QueueHandle_t q) {
    BusMessage msg;
    if (xQueueReceive(q, &msg, 0) == pdTRUE && msg.type == TYPE_STRING) {
        TripPack p;
        memcpy(&p, msg.value.s, sizeof(TripPack));
        tel.odo = p.odo;
        tel.trip_a = p.trip_a;
        tel.fuel_a = p.fuel_trip_a;
        tel.trip_b = p.trip_b;
        tel.fuel_b = p.fuel_trip_b;
        tel.trip_cur = p.trip_cur;
        tel.fuel_cur = p.fuel_cur;
        tel.fuel = p.fuel_level;
        tel.avg = p.avg_consumption;
    }
}

// =============================================================================
// processServicePack
// =============================================================================
static void processServicePack(QueueHandle_t q) {
    BusMessage msg;
    if (xQueueReceive(q, &msg, 0) == pdTRUE && msg.type == TYPE_STRING) {
        ServicePack p;
        memcpy(&p, msg.value.s, sizeof(ServicePack));
        tel.t_cool = p.coolant_temp;
        tel.t_atf = p.atf_temp;
        tel.dtc_cnt = p.dtc_count;
        strlcpy(tel.dtc, p.dtc_codes, sizeof(tel.dtc));
        tel.t_int = p.interior_temp;
        tel.t_ext = p.exterior_temp;
        tel.tire = p.tire_pressure;
        tel.wash = p.washer_level;
    }
}

// =============================================================================
// processSettingsPack
// =============================================================================
static void processSettingsPack(QueueHandle_t q) {
    BusMessage msg;
    if (xQueueReceive(q, &msg, 0) == pdTRUE && msg.type == TYPE_STRING) {
        SettingsPack p;
        memcpy(&p, msg.value.s, sizeof(SettingsPack));
        cfg.tank = p.tank_capacity;
        cfg.inj_perf = p.injector_flow;
        cfg.inj_cnt = p.injector_count;
        cfg.spd_sig = p.pulses_per_meter;
        cfg.kl_proto = p.kline_protocol;
    }
}

// =============================================================================
// Forward declarations
// =============================================================================
static void sendFast(DataBus& db);

// =============================================================================
// processIncoming: Обработка входящих JSON-команд
// =============================================================================
static void processIncoming(QueueHandle_t q, DataBus& db) {
    BusMessage msg;
    while (xQueueReceive(q, &msg, 0) == pdTRUE) {
        Serial.printf("[Protocol] RX msg type=%d, content='%s'\n", msg.type, msg.value.s);

        if (msg.type != TYPE_STRING) continue;

        JsonDocument doc;
        if (deserializeJson(doc, msg.value.s)) {
            db.publish(TOPIC_MSG_OUTGOING, "{\"error\":\"Invalid JSON\"}\n");
            lastMsgId = 0;  // Не можем ответить квитанцией — JSON невалиден
            continue;
        }

        const char* cmd = doc["command"];
        if (!cmd) cmd = doc["cmd"];
        if (!cmd) continue;

        // msg_id теперь числовой (int) — сохраняем как квитанцию
        JsonVariant msgIdVar = doc["msg_id"];
        if (!msgIdVar.isNull()) {
            if (msgIdVar.is<int>()) {
                lastMsgId = msgIdVar.as<int>();
            }
        }
        // Логируем для отладки
        Serial.printf("[Protocol] Command='%s', msg_id=%d\n", cmd, lastMsgId);

        // --- get_cfg ---
        if (strcmp(cmd, "get_cfg") == 0) {
            JsonDocument resp;
            resp["ack_id"] = lastMsgId;  // Число, не строка!
            lastMsgId = 0;  // Квитанция отправлена, сбрасываем
            JsonObject c = resp["cfg"].to<JsonObject>();
            c["tank"] = cfg.tank; c["inj_perf"] = cfg.inj_perf;
            c["inj_cnt"] = cfg.inj_cnt; c["spd_sig"] = cfg.spd_sig;
            c["kl_proto"] = cfg.kl_proto; c["fw"] = FW_VERSION_STR;
            serializeJson(resp, outBuffer, sizeof(outBuffer));
            strlcat(outBuffer, "\n", sizeof(outBuffer));
            outBuffer[sizeof(outBuffer) - 1] = '\0';  // Гарантированная null-терминация
            Serial.printf("[Protocol] <<< TX: %s", outBuffer);
            db.publish(TOPIC_MSG_OUTGOING, outBuffer);
            continue;
        }

        // --- set_cfg ---
        if (strcmp(cmd, "set_cfg") == 0) {
            JsonObject data = doc["data"];
            if (!data) data = doc.as<JsonObject>();

            SettingsPack pack;
            memset(&pack, 0, sizeof(pack));
            pack.version = 1;
            pack.tank_capacity = cfg.tank;
            pack.injector_count = cfg.inj_cnt;
            pack.injector_flow = cfg.inj_perf;
            pack.pulses_per_meter = cfg.spd_sig;
            pack.kline_protocol = cfg.kl_proto;

            if (data["tank"].is<float>()) pack.tank_capacity = data["tank"].as<float>();
            if (data["inj_perf"].is<float>()) pack.injector_flow = data["inj_perf"].as<float>();
            if (data["inj_cnt"].is<int>()) pack.injector_count = data["inj_cnt"].as<int>();
            if (data["spd_sig"].is<float>()) pack.pulses_per_meter = data["spd_sig"].as<float>();
            if (data["kl_proto"].is<int>()) pack.kline_protocol = data["kl_proto"].as<int>();

            db.publishPacket(TOPIC_SETTINGS_PACK, &pack, sizeof(pack));

            // ACK для set_cfg
            {
                JsonDocument resp;
                resp["ack_id"] = lastMsgId;
                lastMsgId = 0;  // Квитанция отправлена
                serializeJson(resp, outBuffer, sizeof(outBuffer));
                strlcat(outBuffer, "\n", sizeof(outBuffer));
                outBuffer[sizeof(outBuffer) - 1] = '\0';  // Гарантированная null-терминация
            }
            db.publish(TOPIC_MSG_OUTGOING, outBuffer);
            continue;
        }

        // --- start/stop_telemetry ---
        if (strcmp(cmd, "start_telemetry") == 0) {
            isStreamingActive = true;
            Serial.println("[Protocol] Streaming STARTED");
            // Сразу отправляем телеметрию с квитанцией (не ждём 100мс)
            sendFast(db);
            continue;
        }
        else if (strcmp(cmd, "stop_telemetry") == 0) {
            isStreamingActive = false;
            Serial.println("[Protocol] Streaming STOPPED");
            continue;
        }
        else {
            // --- Команды в TOPIC_CMD ---
            CmdPayload payload;
            memset(&payload, 0, sizeof(payload));
            payload.msg_id = lastMsgId;  // Квитанция для CmdPayload

            if (strcmp(cmd, "reset_trip_a") == 0) payload.cmd = CMD_RESET_TRIP_A;
            else if (strcmp(cmd, "reset_trip_b") == 0) payload.cmd = CMD_RESET_TRIP_B;
            else if (strcmp(cmd, "reset_avg") == 0) payload.cmd = CMD_RESET_AVG;
            else if (strcmp(cmd, "full_tank") == 0) payload.cmd = CMD_FULL_TANK;
            else if (strcmp(cmd, "correct_odo") == 0) {
                payload.cmd = CMD_CORRECT_ODO;
                payload.correct_odo.odo_value = doc["data"]["value"] | 0.0;
            }
            else if (strcmp(cmd, "kl_get_dtc") == 0) payload.cmd = CMD_KL_GET_DTC;
            else if (strcmp(cmd, "kl_clear_dtc") == 0) payload.cmd = CMD_KL_CLEAR_DTC;
            else if (strcmp(cmd, "kl_reset_adapt") == 0) payload.cmd = CMD_KL_RESET_ADAPT;
            else if (strcmp(cmd, "kl_pump_atf") == 0) payload.cmd = CMD_KL_PUMP_ATF;
            else if (strcmp(cmd, "kl_detect_protocol") == 0) payload.cmd = CMD_KL_DETECT_PROTO;
            else {
                JsonDocument resp;
                resp["ack_id"] = lastMsgId;
                resp["error"] = "Unknown command";
                lastMsgId = 0;  // Квитанция отправлена (с ошибкой)
                serializeJson(resp, outBuffer, sizeof(outBuffer));
                strlcat(outBuffer, "\n", sizeof(outBuffer));
                outBuffer[sizeof(outBuffer) - 1] = '\0';  // Гарантированная null-терминация
                db.publish(TOPIC_MSG_OUTGOING, outBuffer);
                continue;
            }

            db.publish(TOPIC_CMD, payload);
        }

        // ACK для команд
        {
            JsonDocument resp;
            resp["ack_id"] = lastMsgId;
            lastMsgId = 0;  // Квитанция отправлена
            serializeJson(resp, outBuffer, sizeof(outBuffer));
            strlcat(outBuffer, "\n", sizeof(outBuffer));
            outBuffer[sizeof(outBuffer) - 1] = '\0';  // Гарантированная null-терминация
        }
        db.publish(TOPIC_MSG_OUTGOING, outBuffer);
    }
}

// =============================================================================
// sendTelemetryFast / Trip / Service
// =============================================================================

static void sendFast(DataBus& db) {
    // Не отправляем если BT не подключён — экономим ресурсы
    if (!btIsConnected()) return;

    JsonDocument doc;
    // Квитанция: если есть непринятый msg_id — добавляем ack_id и сбрасываем
    if (lastMsgId != 0) {
        doc["ack_id"] = lastMsgId;
        lastMsgId = 0;
    }
    JsonObject t = doc["tel"].to<JsonObject>();
    t["spd"] = (int)tel.speed; t["rpm"] = (int)tel.rpm;
    t["vlt"] = roundf(tel.voltage * 10) / 10; t["eng"] = tel.eng;
    t["hl"] = tel.hl; t["gear"] = tel.gear;
    t["sel"] = tel.sel; t["tcc"] = tel.tcc;

    // Отладка: если данных от Calculator еще нет (odo=0), покажем размер бака
    float fuelToSend = (tel.odo == 0) ? cfg.tank : roundf(tel.fuel * 10) / 10;
    t["fuel"] = fuelToSend;

    serializeJson(doc, outBuffer, sizeof(outBuffer));
    strlcat(outBuffer, "\n", sizeof(outBuffer));
    outBuffer[sizeof(outBuffer) - 1] = '\0';  // Гарантированная null-терминация
    db.publish(TOPIC_MSG_OUTGOING, outBuffer);

    // Отладка: первое сообщение — выводим JSON полностью
    static int fastCount = 0;
    if (++fastCount == 1) {
        Serial.printf("[Protocol] >>> FIRST FAST JSON: %s", outBuffer);
    } else if (fastCount % 100 == 0) {
        Serial.printf("[Protocol] >>> FAST #%d sent\n", fastCount);
    }
}

static void sendTrip(DataBus& db) {
    if (!btIsConnected()) return;
    JsonDocument doc;
    doc["ack_id"] = lastMsgId;
    JsonObject t = doc["tel"].to<JsonObject>();
    t["spd"] = (int)tel.speed; t["rpm"] = (int)tel.rpm;
    t["vlt"] = roundf(tel.voltage * 10) / 10; t["eng"] = tel.eng;
    t["hl"] = tel.hl; t["gear"] = tel.gear;
    t["sel"] = tel.sel; t["tcc"] = tel.tcc;
    t["odo"] = (int)tel.odo;
    t["trip_a"] = roundf(tel.trip_a * 10) / 10;
    t["fuel_a"] = roundf(tel.fuel_a * 10) / 10;
    t["trip_b"] = roundf(tel.trip_b * 10) / 10;
    t["fuel_b"] = roundf(tel.fuel_b * 10) / 10;
    float fuelToSend = (tel.odo == 0) ? cfg.tank : roundf(tel.fuel * 10) / 10;
    t["trip_cur"] = roundf(tel.trip_cur * 10) / 10;
    t["fuel_cur"] = roundf(tel.fuel_cur * 10) / 10;
    t["fuel"] = fuelToSend;
    t["inst"] = roundf(tel.inst_fuel * 10) / 10;
    t["avg"] = roundf(tel.avg * 10) / 10;
    serializeJson(doc, outBuffer, sizeof(outBuffer));
    strlcat(outBuffer, "\n", sizeof(outBuffer));
    outBuffer[sizeof(outBuffer) - 1] = '\0';  // Гарантированная null-терминация
    db.publish(TOPIC_MSG_OUTGOING, outBuffer);
}

// =============================================================================
// protocolTask — Главная задача
// =============================================================================

void protocolTask(void* parameter) {
    (void)parameter;
    isRunningFlag = true;
    DataBus& db = DataBus::getInstance();

    // Подписки на пакеты (FAST only — EnginePack + TripPack для odo/fuel)
    SubscriberOpts pktOpts = {QUEUE_OVERWRITE, 1, false};
    QueueHandle_t engineQ = db.subscribe(TOPIC_ENGINE_PACK, pktOpts);
    QueueHandle_t tripQ = db.subscribe(TOPIC_TRIP_PACK, pktOpts);

    // Подписка на входящие (FIFO_DROP, depth=3)
    SubscriberOpts inOpts = {QUEUE_FIFO_DROP, 3, false};
    QueueHandle_t incomingQ = db.subscribe(TOPIC_MSG_INCOMING, inOpts);

    Serial.println("[Protocol] Task started (FAST only, 100ms)");

    unsigned long lastFast = 0;

    while (1) {
        lastHeartbeat = millis();

        // Чтение очередей пакетов
        if (engineQ) processEnginePack(engineQ);
        if (tripQ) processTripPack(tripQ);
        if (incomingQ) processIncoming(incomingQ, db);

        if (!isStreamingActive) { vTaskDelay(50 / portTICK_PERIOD_MS); continue; }

        unsigned long now = millis();
        if (now - lastFast >= FAST_MS) { lastFast = now; sendFast(db); }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

// =============================================================================
// Управление
// =============================================================================

void protocolStart() {
    if (!taskHandle) {
        xTaskCreatePinnedToCore(protocolTask, "Protocol", 8192, NULL, 3, &taskHandle, 1);
        Serial.println("[Protocol] Started");
    }
}

void protocolStop() {
    if (taskHandle) {
        vTaskDelete(taskHandle);
        taskHandle = NULL;
        isRunningFlag = false;
        Serial.println("[Protocol] Stopped");
    }
}

bool protocolIsRunning() {
    return isRunningFlag && (millis() - lastHeartbeat) < 3000;
}
