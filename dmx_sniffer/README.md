# ESP32 DMX Sniffer & Tester

## Архитектура модулей

```
                              ┌──────────────────┐
                              │     main.c       │
                              │  только init     │
                              └────────┬─────────┘
                                       │
                    ┌──────────────────┼──────────────────┐
                    │                  │                  │
                    ▼                  ▼                  ▼
           ┌──────────────┐   ┌──────────────┐   ┌──────────────┐
           │  network/    │   │   dmx.c      │   │  dmx_led.c   │
           │              │   │  (UART HAL)  │   │  (DMX→LED)   │
           │  wifi_ap.c   │   │              │   │              │
           │  web_server.c│   │ dmx_init()   │   │  callback    │
           │  web_page.h  │   │ dmx_read()   │   │  API         │
           │              │   │ dmx_write()  │   │              │
           │  HTTP API    │   │ dmx_set_mode │   │  patch→LED   │
           │  GET/POST    │   │ dmx_on_frame │   │  interp.     │
           └──────────────┘   └──────────────┘   └──────────────┘
                  │                                     │
                  │    ┌────────────────────────────────┤
                  │    │                                │
                  ▼    ▼                                ▼
           ┌──────────────┐                     ┌──────────────┐
           │ app_handlers │                     │ led_driver/  │
           │     .c       │                     │  ws2812.c    │
           │              │                     │              │
           │ handler_*()  │                     │  led_init()  │
           │              │                     │  led_set_px  │
           └──────────────┘                     │  led_show()  │
                  │                             └──────────────┘
                  ▼
           ┌──────────────┐
           │  utilite/    │
           │              │
           │  effects.c   │
           │  patch_mgr.c │
           │  settings_mgr│
           │  udp_test.c  │
           └──────────────┘
```

## Модули

### dmx.c — UART DMX512 (чистый HAL)
Только приём/передача через UART.

```c
void dmx_init(int tx1, int tx2, int rx1, int rx2, int dir);
void dmx_read(int port, uint8_t *buf, int len);
void dmx_write(int port, const uint8_t *frame, int len);
void dmx_set_mode(dmx_mode_t m);
void dmx_on_frame(dmx_frame_cb_t cb);
void dmx_lock(void);
void dmx_unlock(void);
```

**Не знает про:** LED, fallback, effects, patch, web

---

### dmx_led.c — DMX → LED мост
Callback API (function pointers).

```c
void dmx_led_init(const dmx_led_cbs_t *cbs);
void dmx_led_recompute(void);
void dmx_led_apply_fallback(void);
```

**Ответственность:** патч → RGB, interpolation, sequential/parallel, fallback timer, led_refresh_task

---

### ws2812.c — LED-драйвер RMT
Двойная буферизация, GRB conversion.

**Публичный:** `led_init`, `led_set_pixel`, `led_show`, `led_fill`, `led_clear`
**Внутренний:** `led_lock`, `led_get_colors`, `led_swap_banks`, `led_refresh`

---

### effects.c — Анимации
Rainbow, breathe, running, wave.

**API:** `effects_init`, `effects_set`, `effects_clear`, `effects_render`, `effects_is_active`

---

### app_handlers.c — Обработчики команд
Реализация handler'ов для web сервера и dmx_led callback'ов.

```c
void app_handlers_init(void);  // вызывает dmx_led_init() с callback'ами
```

---

### network/wifi_ap.c — WiFi + Status LED
WiFi STA/AP toggle + status LED task.

```c
esp_err_t wifi_init(void);
esp_err_t wifi_toggle(void);
esp_err_t wifi_stop(void);
esp_err_t wifi_start(void);
bool wifi_is_on(void);
void wifi_status_led_init(void);
```

---

### network/web_server.c — HTTP REST API
Проект-специфичный, вызывает `handler_*` функции из app_handlers.c напрямую.

---

### patch_manager.c — Таблица патчей
CSV в SPIFFS. `g_patch` — массив entries[340].

---

### settings_manager.c — NVS
Load/save с debounce 500ms.

---

### udp_test.c — UDP отладка/OTA
Порт 5124.

---

## Потоки данных

```
UART RX ISR → dmx.c (double-buffer) → callback → dmx_led.c → ws2812.c → LED
                                                       ↑
HTTP POST → web_server.c → handler_*() → g_dmx.field = value ┘
```

## Задачи FreeRTOS

| Задача     | Ядро | Приоритет | Модуль       |
|------------|------|-----------|--------------|
| dmx_rx0    | 1    | 4         | dmx.c        |
| dmx_rx1    | 1    | 4         | dmx.c        |
| dmx_tx     | 1    | 5         | dmx.c        |
| led_ref    | 1    | 5         | dmx_led.c    |
| udp_test   | 1    | 5         | udp_test.c   |
| status_led | 0    | 1         | wifi_ap.c    |

## Структура файлов

```
main/
├── main.c            ← только init вызовы
├── dmx.c/.h          ← чистый UART HAL
├── dmx_led.c/.h      ← callback API, DMX→LED мост
├── settings.h        ← hardware config
├── led_strip.h       ← compatibility redirect
├── favicon.ico
├── led_driver/
│   └── ws2812.c/.h
├── network/
│   ├── web_server.c/.h
│   ├── wifi_ap.c/.h
│   └── web_page.h
└── utilite/
    ├── app_handlers.c/.h  ← handler'ы + dmx_led init
    ├── effects.c/.h
    ├── patch_manager.c/.h
    ├── settings_manager.c/.h
    └── udp_test.c/.h
```
