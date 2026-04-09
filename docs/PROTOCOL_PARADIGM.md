# 📜 PROTOCOL_PARADIGM.md: Архитектура протокола (Protocol Task)

> **Дата:** 2026-04-09
> **Версия:** 1.0
> **Статус:** Архитектурный контракт

---

## 🎯 1. Роль Protocol в системе

**Protocol Task** — единственный модуль, который знает про JSON, `msg_id`, `ack_id` и логику обмена с Android. Все остальные модули системы работают исключительно со своими структурами данных и не имеют понятия о существовании JSON.

| Принцип | Реализация |
|---------|-----------|
| **JSON — территория Protocol** | Только Protocol сериализует JSON для Android и разбирает входящий JSON. Никто другой не создаёт, не парсит и не публикует JSON. |
| **Остальные модули не знают про протокол** | K-Line публикует `KlinePack`, Climate — `ClimatePack`, Calculator — `TripPack`. Они не формируют JSON и не оперируют `msg_id`/`ack_id`. |
| **Protocol — шлюз** | Android ↔ JSON ↔ Protocol ↔ пакеты ↔ шина. В одну сторону — парсинг, в другую — сериализация. |

---

## 🔢 2. Механика квитанций (msg_id → ack_id)

### Суть

`msg_id` и `ack_id` — это порядковые номера сообщений на уровне протокола. Android отправляет команду с номером, БК подтверждает доставку, возвращая этот же номер в поле `ack_id`.

### Правила

| Правило | Описание |
|---------|----------|
| **ack_id подтверждает доставку, а не выполнение** | Protocol отправляет ACK сразу после получения команды. Не ждёт, пока K-Line выполнит запрос или Calculator сбросит trip. |
| **msg_id не хранится дольше, чем нужно для ACK** | Protocol сохраняет `msg_id` в локальную переменную → подмешивает `ack_id` в ближайшее исходящее → обнуляет. |
| **Долгие команды не блокируют очередь** | Android может слать следующие команды, не дожидаясь результата предыдущей. |
| **Результат команды идёт отдельным сообщением (без ack_id)** | K-Line отвечает в своём пакете → Protocol формирует JSON-ответ → отправляет без `ack_id` (квитанция уже ушла в ACK). |
| **Гарантированная доставка** | Android не получил ACK → повторит запрос через таймаут. БК обработает повторно, ACK придёт с тем же номером. |

### Поток сообщений

```
Android: {"command":"kl_get_dtc","msg_id":42}
    │
    ▼
Protocol: получил → сохранил lastMsgId = 42
    │
    ├──→ Ближайшее исходящее: {"ack_id":42}  ← квитанция
    │     lastMsgId = 0  ← обнулено
    │
    └──→ публикует CMD_KL_GET_DTC в шину
            │
            ▼
        K-Line: выполняет (может 500 мс)
            │
            ▼
        публикует KlinePack в TOPIC_KLINE_PACK
            │
            ▼
        Protocol (подписан) → {"dtc":"P0135","count":2}  ← без ack_id!
```

### Реализация

```cpp
// Локальная переменная в области Protocol Task
static int lastMsgId = 0;

// При получении входящего JSON
void processIncoming(...) {
    JsonDocument doc;
    deserializeJson(doc, incoming);
    
    int msgId = doc["msg_id"] | 0;  // Извлекли номер
    if (msgId != 0) lastMsgId = msgId;  // Сохранили
    
    // Обработали команду, опубликовали в шину
    // ...
}

// При отправке ЛЮБОГО исходящего JSON
void publishOutgoing(...) {
    // Если есть квитанция — подмешиваем ack_id
    if (lastMsgId != 0) {
        doc["ack_id"] = lastMsgId;
        lastMsgId = 0;  // Обнуляем — квитанция использована
    }
    // Публикуем в шину
    db.publish(TOPIC_MSG_OUTGOING, buffer);
}
```

---

## 📦 3. Пакетная архитектура

### Разделение зон ответствен

Каждый модуль публикует **свой** пакет в **свой** топик. Protocol подписан на все пакеты и собирает из них JSON.

| Пакет | Топик | Публикует | Подписан Protocol |
|-------|-------|-----------|-------------------|
| `EnginePack` | `TOPIC_ENGINE_PACK` | Simulator / Engine | ✅ FAST JSON |
| `TripPack` | `TOPIC_TRIP_PACK` | Calculator | ✅ TRIP JSON |
| `KlinePack` | `TOPIC_KLINE_PACK` | K-Line Task | ✅ SERVICE JSON |
| `ClimatePack` | `TOPIC_CLIMATE_PACK` | Climate Task | ✅ SERVICE JSON |
| `SettingsPack` | `TOPIC_SETTINGS_PACK` | Storage Task | ✅ get_cfg ответ |

### Почему не один ServicePack?

| Проблема единого ServicePack | Решение разделения |
|------------------------------|-------------------|
| K-Line и Climate публикуют в один топик → последний перезаписывает первый | Каждый модуль в своём топике — данные не конфликтуют |
| Protocol теряет половину данных | Protocol подписан на оба топика, собирает полный SERVICE JSON |
| K-Line и Climate зависят друг от друга | Могут работать с разной периодичностью, независимо друг от друга |

---

## ⏱️ 4. Фракционная отправка телеметрии

Protocol отправляет JSON на Android с нарастающей детализацией:

| Интервал | Счётчик | Содержимое |
|----------|---------|------------|
| **100 мс** | Каждый | **FAST**: spd, rpm, vlt, eng, hl, gear, sel, tcc, fuel |
| **500 мс** | `counter % 5 == 0` | FAST + **TRIP**: odo, trip_a/b, fuel_a/b, trip_cur, fuel_cur, inst, avg |
| **1000 мс** | `counter % 10 == 0` | FAST + TRIP + **SERVICE**: t_cool, t_atf, dtc_cnt, dtc, t_int, t_ext, tire, wash |

### Реализация

```cpp
unsigned long lastSend = 0;
int counter = 0;

while (1) {
    // Чтение очередей пакетов
    if (engineQ)   processEnginePack(engineQ);
    if (tripQ)     processTripPack(tripQ);
    if (klineQ)    processKlinePack(klineQ);
    if (climateQ)  processClimatePack(climateQ);
    
    unsigned long now = millis();
    if (now - lastSend >= 100) {
        lastSend = now;
        counter++;
        
        JsonDocument doc;
        buildFastJson(doc);                    // Всегда
        
        if (counter % 5 == 0) addTripFields(doc);     // Каждые 500мс
        if (counter % 10 == 0) addServiceFields(doc); // Каждые 1000мс
        
        // Подмешиваем ack_id если есть квитанция
        if (lastMsgId != 0) {
            doc["ack_id"] = lastMsgId;
            lastMsgId = 0;
        }
        
        serializeJson(doc, outBuffer, sizeof(outBuffer));
        db.publish(TOPIC_MSG_OUTGOING, outBuffer);
    }
    
    // Обработка входящих команд
    if (incomingQ) processIncoming(incomingQ, db);
    
    vTaskDelay(10 / portTICK_PERIOD_MS);
}
```

---

## 🚩 5. Красные флаги (Как распознать отклонение)

| Симптом | Причина (нарушение парадигмы) | Исправление |
|---------|------------------------------|-------------|
| K-Line публикует JSON в `TOPIC_MSG_OUTGOING` | K-Line не должен знать про JSON | K-Line публикует `KlinePack` в `TOPIC_KLINE_PACK` |
| Climate публикует JSON | Climate не должен знать про JSON | Climate публикует `ClimatePack` в `TOPIC_CLIMATE_PACK` |
| `ack_id` появляется в каждом пакете телеметрии | `lastMsgId` не обнуляется | Обнулять сразу после использования |
| Команда не получает ACK | `msg_id` не извлекается из входящего JSON | Извлекать `msg_id` в `processIncoming` |
| Телеметрия содержит `ack_id` без команды | Глобальная переменная «залипла» | Обнулять после первой же отправки |
| Protocol блокирует задачу >10 мс | Тяжёлая логика в цикле | Вынести в отдельные функции, только очереди |
| JSON обрезается на `}...` | `PAYLOAD_MAX` < размер пакета | `PAYLOAD_MAX >= 384` |

---

## 📏 6. Жёсткие правила реализации

| ✅ Разрешено | ❌ Запрещено |
|-------------|-------------|
| Protocol — единственный создатель JSON | Любой другой модуль формирует JSON |
| Protocol — единственный обработчик входящих команд | Модуль сам парсит JSON из шины |
| Публикация `enum Command` в `TOPIC_CMD` | Публикация JSON в `TOPIC_CMD` |
| `lastMsgId` — локальная переменная Protocol | `msg_id` в CmdPayload или других структурах |
| ACK сразу после получения команды | ACK после выполнения команды |
| `ack_id` подмешивается к любому исходящему (телеметрия или ответ) | Отдельная отправка ACK отдельным сообщением |
| KlinePack / ClimatePack — отдельные пакеты | Единый ServicePack от нескольких модулей |

---

## ✅ 7. Чек-лист перед коммитом

- [ ] Только Protocol публикует в `TOPIC_MSG_OUTGOING`
- [ ] Только Protocol читает `TOPIC_MSG_INCOMING`
- [ ] K-Line публикует `KlinePack` в `TOPIC_KLINE_PACK` (не JSON!)
- [ ] Climate публикует `ClimatePack` в `TOPIC_CLIMATE_PACK` (не JSON!)
- [ ] `lastMsgId` — локальная переменная Protocol, обнуляется после использования
- [ ] ACK отправляется сразу при получении команды, не после выполнения
- [ ] Фракционная отправка: FAST 100мс, TRIP 500мс, SERVICE 1000мс
- [ ] JSON-буфер ≥ 384 байт, `PAYLOAD_MAX ≥ 384`
- [ ] Нет `String` в публикациях, только `char[]`
- [ ] Нет блокировок >10 мс в цикле Protocol

---

📌 **Итог:** Protocol — это шлюз между миром JSON (Android) и миром пакетов (внутренняя шина). Он один знает оба языка. Все остальные модули говорят только на языке пакетов.
