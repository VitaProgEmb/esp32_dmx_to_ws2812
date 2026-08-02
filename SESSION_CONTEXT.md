# DMX Sniffer — Context Session File

## Project Overview
- **GitHub**: https://github.com/VitaProgEmb/esp32_dmx_to_ws2812.git
- **Platform**: ESP32 original (ESP-IDF v5.5.4), C language
- **Device**: DMX512 dual-port sniffer with WS2812 LED output

## Hardware Pinout
- GPIO15: DMX RX1 (port 0)
- GPIO16: DMX RX2 (port 1)
- GPIO2: DMX TX1
- GPIO17: DMX TX2
- GPIO27: DIR2
- GPIO23: WS2812 Strip 1
- GPIO5: WS2812 Strip 2
- GPIO18: Status LED
- COM9: FT2232 VCP1 (DMX TX), COM3: CP210x (serial monitor)
- Physical setup: COM9 TX connected to both GPIO15 and GPIO16 for testing

## Key Decisions & Architecture
- DMX512: 250kbaud, 8N2, 4µs/bit, 512 channels
- WiFi STA, IP = 192.168.1.100
- ESP32 does NOT support UART DMA — only FIFO + ISR
- Asymmetric FIFO thresholds: UART1=8, UART2=16 (prevents simultaneous ISR firing)
- No spinlock in ISR — each UART reads its own FIFO
- WiFi OFF hurts accuracy (UART disable/enable disrupts reception)
- Errors likely from test wiring (COM9 TX → both GPIOs simultaneously)
- Device is for a customer — must be reliable, no flickering acceptable

## Accuracy Measurements
| Config | Accuracy | Notes |
|---|---|---|
| ck-test, WiFi ON, full frames | 96.6% | 828/857, real measurement |
| ck-test, WiFi ON, all frames | 95.3% | 828/869, includes truncated |
| WiFi-off test | ~82-84% | UART disable/enable hurts |

## Partition Layout
- nvs: 0x9000 (16KB)
- otadata: 0xD000 (8KB)
- phy_init: 0xF000 (4KB)
- ota_0: 0x10000 (1MB)
- ota_1: 0x110000 (1MB)
- storage/SPIFFS: 0x210000 (~2MB)
- Binary size: 698KB / 1024KB (68%)

## Completed Work

### Core DMX RX Module
- `dmx_rx.c/h`: HW UART ISR + double-buffer + event group
- No spinlock in ISR, asymmetric FIFO thresholds (UART1=8, UART2=16)
- Checksum ring buffer in `dmx_rx.c` + `dmx_bus.h` (1024 entries per port, 8KB)

### LED Driver
- `ws2812.c/h` (310 lines): Merged from `led_strip.c` (382 lines). RMT encoder, dual-buffer, mutex, GRB, 2 strips, shift/reverse
- `led_strip.h`: Thin shim (`#include "ws2812.h"`)
- shift/reverse stored on strip struct (not g_dmx)

### Removed Dead Code
- Deleted `uart_bypass.c` (992 lines): 3 RX modes, only Mode 2 was active
- Replaced with `dmx_rx.c` (9KB)
- `dmx_hal.c` simplified to TX-only (72 lines)

### Event Bus
- `event_bus.h/c`: Queue + dispatcher, ~400 bytes RAM
- Queue: 8 msgs × 20 bytes
- Dispatcher task: "evt_bus", CPU0, priority 5, stack 4096
- Events: EVT_DMX_FRAME, EVT_DMX_LOST, EVT_MODE_CHANGED, EVT_LEDS_UPDATED, EVT_PATCH_UPDATED, EVT_SETTING_CHANGED, EVT_OTA_START/PROGRESS/COMPLETE
- API: `event_on()`, `event_off()`, `event_emit()`, `event_bus_pending_count()`, `event_bus_emit_count()`

### Web Server
- `web_server.c` (1046 lines): SPIFFS init, gzip serving, 26 existing endpoints
- SPIFFS: `esp_vfs_spiffs_register()` with "storage" partition
- Serves `/spiffs/index.html.gz` with `Content-Encoding: gzip`, fallback to embedded `index_html`
- `web_server_init()` wired into `main.c`

### SPIFFS HTML Pipeline
- HTML extracted from `web_page.h` (54KB) to `www/index.html`, gzipped to ~13KB
- CMakeLists.txt: `spiffs_create_partition_image(storage www FLASH_IN_PROJECT)`
- `build/storage.bin` = 1984KB

### UDP OTA
- 714KB in 4.7s, 167 KB/s, 0 retries
- Protocol: 0x01=blob(1024B), 0x02=OTA begin, 0x03=OTA chunk, 0x04=OTA end, 0x05=checksum(8B), 0x06=ck report(6B/entry)

### Test Scripts
- `dmx_stress_test.py`: blob/checksum/ck-test modes, counter/increment/random patterns
- `udp_ota.py`: UDP OTA with `--ip` and `--bin` flags
- Cleaned up old test scripts

### New Web UI (www/index.html)
- Single HTML file with inline CSS/JS (869 lines)
- GitHub-dark theme (#0d1117 background)
- JS architecture: `Bus` (event bus), `Store` (reactive state), `API` (fetch layer), `registerModule()`, `initTabs()`
- Mock data for preview without ESP32
- Header: "DMX Sniffer" centered, larger font (1.4em bold)
- Tabs: full-width, distributed evenly
- Cards: 2px border (#444c56), centered titles (1.1em bold)
- DMX Signal card: Two rectangular boxes with "DMX PORT 1/2", XLR connector SVG icons, animated arrows
- Mode selector: 4 tab-style buttons (Конвертер=#58a6ff, LED Test=#f85149, DMX Test=#d29922, Patch=#a371f7)
- Confirmation modal for DMX Test/Patch/LED Test modes
- Inline panels: DMX Test, LED Test, Patch cards below mode selector

### Frontend API Spec
- GET: `/`, `/api/state`, `/api/blob`, `/api/patch`, `/api/stream` (SSE)
- POST: `/api/mode`, `/api/leds`, `/api/led_config`, `/api/led_test`, `/api/dmx_test`, `/api/fixture`, `/api/patch`, `/api/save`, `/api/ota`, `/api/reboot`

## Red Arrow Bug — FIXED

### Root Cause
In `_setMode()`, order was wrong:
```js
// OLD (broken):
Store.set('mode', mode);  // 1. subscribers set arrows on OLD DOM
initTabs();                // 2. destroys old DOM, creates new with defaults
```

### Fix Applied
Two changes to `www/index.html`:

1. **Fixed `_setMode()` order** — `initTabs()` now called BEFORE `Store.set('mode')`:
```js
async _setMode(mode) {
    await API.post('/api/mode', { mode });
    initTabs();           // re-render DOM first
    Store.set('mode', mode); // then trigger subscribers on fresh DOM
}
```

2. **Added arrow init in `mounted()` initial render block** — checks `Store.get('mode')` and sets correct arrow state when panel is freshly rendered:
```js
// After inline panel show/hide in "Trigger initial mode render":
const isOutInit = (m === 'tester' || m === 'patch');
[0,1].forEach(p => {
    const arrow = ie('dmxArrow'+p);
    if (!arrow) return;
    const path = arrow.querySelector('path');
    if (isOutInit) {
      arrow.style.opacity = '1';
      arrow.classList.remove('arrow-anim');
      arrow.classList.add('arrow-anim-out');
      if (path) path.setAttribute('stroke', '#f85149');
    } else {
      arrow.style.opacity = '0';
      arrow.classList.remove('arrow-anim-out');
      arrow.classList.add('arrow-anim');
      if (path) path.setAttribute('stroke', '#3fb950');
    }
});
```

## API Mismatch — NOT YET FIXED

Frontend calls APIs that don't exist on web_server.c:

| Frontend calls | Server has | Status |
|---|---|---|
| `GET /api/state` | `GET /api/settings` | ❌ name mismatch |
| `POST /api/led_config` | `/api/led_reverse`, `/api/led_mode`, `/api/led_shift` | ❌ separate endpoints |
| `POST /api/dmx_test` | `POST /api/test` | ❌ name mismatch |
| `POST /api/fixture` | `POST /api/fixture_settings` | ❌ name mismatch |
| `GET /api/stream` (SSE) | — | ❌ not implemented |
| `POST /api/leds` `{strip, count}` | `{count}` only | ❌ missing `strip` param |
| `POST /api/save` | ✅ | ✅ |
| `POST /api/reboot` | ✅ | ✅ |
| `POST /api/ota` | ✅ | ✅ |
| `GET /api/blob` | ✅ | ✅ |

## TODO — Next Steps

1. **Add missing endpoints to `web_server.c`** — `/api/state`, `/api/led_config`, `/api/dmx_test`, `/api/fixture`, fix `/api/leds` to accept `strip` param
2. **Add SSE `/api/stream`** — subscribe to `EVT_DMX_FRAME`, push DMX data + state to connected clients via `httpd_resp_send_chunk()`
3. **Switch frontend from mock to real** — `startPolling()` → fetch `/api/blob` periodically + connect to `/api/stream` SSE
4. **Merge test `main.c` → full `dmx.c`** — replace test firmware with production application (has TX tasks, LED processing, patch, fallback timer, mode switching)
5. **DMX TX in test firmware** — wire `dmx_hal_send()` for TESTER mode testing

## Critical File Paths

### Core
- `main/dmx.c` (904 lines): Full application — RX tasks, TX tasks, LED processing, fallback, patch, mode switching
- `main/dmx.h`: Types — `dmx_state_t`, `dmx_mode_t`, `tx_mode_t`, `channel_order_t`, function declarations
- `main/dmx_rx.c/h`: HW UART ISR + double-buffer + checksum ring buffer
- `main/dmx_bus.h`: Shared types — `dmx_raw_frame_t`, `g_dmx_events`, `g_raw_frames[2]`, `dmx_ck_entry_t`
- `main/dmx_hal.c/h`: TX only — `dmx_hal_send()` via direct UART registers

### LED
- `main/led_driver/ws2812.c/h`: Full LED driver — RMT encoder, dual-buffer, mutex, GRB, 2 strips
- `main/led_strip.h`: Thin shim

### Web
- `main/web_server.c`: HTTP server with 26 endpoints, SPIFFS, gzip serving
- `main/web_server.h`: API — `web_server_init()`, `web_server_stop()`
- `main/web_page.h`: Original HTML/CSS/JS (53KB) — kept as fallback
- `www/index.html`: New web UI — single HTML, Bus/Store/Module, mock data

### Infrastructure
- `main/utilite/event_bus.h/c`: Event queue + dispatcher
- `main/wifi_ap.c/h`: WiFi STA/AP — `wifi_stop()`, `wifi_start()`, `wifi_toggle()`
- `main/udp_test.c`: UDP server — commands 0x01-0x06, OTA
- `main/settings.h`: Pin defs, WiFi, LED strip GPIOs, FIFO thresholds
- `main/patch_manager.h`: Patch table types and functions
- `main/settings_manager.h`: NVS save/load

### Build
- `CMakeLists.txt`: Root — `spiffs_create_partition_image(storage www FLASH_IN_PROJECT)`
- `main/CMakeLists.txt`: SRCS list — includes `utilite/event_bus.c`
- `partitions.csv`: Custom partition table

### Test
- `dmx_stress_test.py`: DMX stress test with blob/checksum/ck-test modes
- `udp_ota.py`: UDP OTA flasher
- `main/main.c` (160 lines): TEST firmware — WiFi + DMX RX + UDP + web server + event bus (NO TX, NO dmx.c)

## ESP-IDF Build
```batch
C:\Espressif\frameworks\esp-idf-v5.5.4\export.bat
idf.py build
idf.py -p COM3 flash monitor
```
