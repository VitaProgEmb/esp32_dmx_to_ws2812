#pragma once

static const char index_html[] = R"rawliteral(
<!doctype html><html lang="ru">
<head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>DMX Sniffer / Tester</title>
<style>*{box-sizing:border-box;margin:0;padding:0}body{font-family:'Segoe UI',sans-serif;background:#1a1a2e;color:#eee;min-height:100vh}.header{background:#16213e;padding:12px 20px;text-align:center;font-size:1.3em;font-weight:700;letter-spacing:1px;border-bottom:2px solid #0f3460}.tabs{display:flex;background:#0f3460;overflow-x:auto}.tab{flex:1;padding:12px 8px;text-align:center;cursor:pointer;font-weight:600;transition:.2s;border-bottom:3px solid transparent;white-space:nowrap;font-size:.9em}.tab:hover{background:#1a1a3e}.tab.active{border-bottom-color:#e94560;background:#1a1a3e}.panel{display:none;padding:16px;max-width:700px;margin:0 auto}.panel.active{display:block}.card{background:#16213e;border-radius:10px;padding:14px;margin-bottom:12px;border:1px solid #0f3460;overflow:hidden}.card h3{margin-bottom:10px;color:#e94560;font-size:.95em;display:flex;align-items:center;gap:6px}.row{display:flex;align-items:center;margin-bottom:8px;gap:8px;flex-wrap:wrap}.row label{min-width:110px;font-size:.85em;color:#aaa}.row input[type=range]{flex:1;min-width:120px;accent-color:#e94560}.row select{background:#0f3460;color:#eee;border:1px solid #333;padding:5px 8px;border-radius:6px;font-size:.85em}.row input[type=number]{background:#0f3460;color:#eee;border:1px solid #333;padding:5px 8px;border-radius:6px;width:80px;font-size:.85em}.val{min-width:50px;text-align:right;font-weight:700;color:#e94560;font-size:.95em}button{background:#e94560;color:#fff;border:none;padding:8px 16px;border-radius:8px;cursor:pointer;font-weight:600;transition:.2s;font-size:.85em}button:hover{background:#c73650}button.secondary{background:#0f3460}button.secondary:hover{background:#1a1a3e}.switch-wrap{display:flex;align-items:center;gap:10px}.switch{position:relative;width:52px;height:28px}.switch input{opacity:0;width:0;height:0}.slider{position:absolute;cursor:pointer;inset:0;background:#333;border-radius:28px;transition:.3s}.slider:before{content:'';position:absolute;height:22px;width:22px;left:3px;bottom:3px;background:#eee;border-radius:50%;transition:.3s}.switch input:checked+.slider{background:#e94560}.switch input:checked+.slider:before{transform:translateX(24px)}table{width:100%;border-collapse:collapse;margin-top:8px}td,th{padding:6px;text-align:center;border:1px solid #333;font-size:.8em}th{background:#0f3460;color:#e94560}td input[type=number]{width:65px;background:#1a1a2e;color:#eee;border:1px solid #444;padding:3px;border-radius:4px;text-align:center}.status{padding:6px 10px;border-radius:6px;margin-top:8px;font-size:.82em}.status.ok{background:#0a3d0a;border:1px solid #0f0}.status.err{background:#3d0a0a;border:1px solid red}.help{font-size:.78em;color:#7a7a9a;margin-top:4px;line-height:1.4;padding:8px;background:#0d1b30;border-radius:6px;border:1px solid #1a2a4a}.help b{color:#e94560}.info-badge{display:inline-block;background:#0f3460;color:#e94560;border-radius:50%;width:16px;height:16px;text-align:center;line-height:16px;font-size:.7em;cursor:help;font-weight:700}.info-toggle{cursor:pointer;user-select:none}.info-content{display:none;margin-top:6px}.info-content.open{display:block}@media(max-width:500px){.row{flex-direction:column;align-items:stretch}.row label{min-width:auto}}.test-active{background:#1a3a1a!important;border:1px solid #0f0!important;transition:background .3s}.modal-overlay{display:none;position:fixed;inset:0;background:rgba(0,0,0,.7);z-index:1000;justify-content:center;align-items:center}.modal-overlay.open{display:flex}.modal-box{background:#16213e;border:2px solid #e94560;border-radius:12px;padding:24px;max-width:400px;width:90%;text-align:center}.modal-box h3{color:#e94560;margin-bottom:12px;font-size:1.1em}.modal-box p{color:#ccc;font-size:.9em;line-height:1.5;margin-bottom:16px}.modal-box .btn-row{display:flex;gap:10px;justify-content:center}.modal-box button{min-width:100px}.work-btn{background:#0a8f0a;color:#fff;border:none;padding:10px 20px;border-radius:8px;cursor:pointer;font-weight:700;font-size:.95em;transition:.2s;width:100%}.work-btn:hover{background:#0c6e0c}.mode-badge{display:inline-block;padding:4px 10px;border-radius:6px;font-size:.8em;font-weight:600;margin-bottom:8px}.mode-badge.safe{background:#0a3d0a;color:#0f0;border:1px solid #0f0}.mode-badge.warn{background:#3d0a0a;color:red;border:1px solid red}.mode-badge.test{background:#3d3d0a;color:#ff0;border:1px solid #ff0}.dmx-led{display:inline-block;width:16px;height:16px;border-radius:50%;background:#333;border:2px solid #555;transition:.3s}.dmx-led.on{background:#0f0;border-color:#0f0;box-shadow:0 0 8px #0f0}.ch-tbl{display:none}
.circle-grid{display:flex;flex-wrap:wrap;gap:3px;padding:4px}
.circle{width:18px;height:18px;border-radius:50%;border:1px solid #333;flex-shrink:0;transition:background .15s}
.info-mode{display:flex;gap:6px;margin-bottom:8px}
.info-mode button{flex:1;padding:6px 4px;font-size:.8em;white-space:nowrap}
.info-mode button.active{background:#0a8f0a}
.dmx-bar-wrap{display:flex;align-items:flex-end;height:140px;overflow:hidden;gap:0;padding:0;background:#0f3460;border-radius:4px}
.dmx-bar-seg{flex:1 1 0;background:#1a1a2e;transition:height .15s,background .15s}
.dmx-bar-lbl{display:flex;justify-content:space-between;font-size:.65em;color:#888;padding:2px 0 6px}
.led-preview-wrap{display:block;overflow:hidden;padding:0;background:#0f3460;border-radius:4px;height:14px}.led-preview-wrap canvas{display:block;width:100%;height:14px}
.help{line-height:1.8;word-wrap:break-word;overflow-wrap:break-word;overflow:hidden}
.ota-result{padding:6px 12px;border-radius:6px;font-weight:700;font-size:.95em;display:inline-block;margin-top:4px}
.ota-result.ok{background:#1a5c1a;color:#8f8}
.ota-result.err{background:#5c1a1a;color:#f88}
</style>
</head>
<body>
<div class="header">DMX Sniffer / Tester
</div>
<div class="tabs">
<div class="tab active" onclick='showTab("settings")'>Настройки
</div>
<div class="tab" id="tabDmx" onclick='showTab("dmx")' style="display:none">DMX Тестер
</div>
<div class="tab" id="tabWs" onclick='showTab("ws")' style="display:none">WS2812
</div>
<div class="tab" id="tabPatch" onclick='showTab("patch")' style="display:none">Патч
</div>
<div class="tab" onclick='showTab("info")'>Инфо
</div>
</div>
<div class="panel active" id="p0">
<div class="card"><h3>DMX сигнал</h3>
<div class="row"><label>Линия 1:</label><span class="dmx-led" id="dmxLed0"></span><span id="dmxLedLabel0" style="font-size:.85em;color:#aaa">нет сигнала</span>
</div>
<div class="row"><label>Линия 2:</label><span class="dmx-led" id="dmxLed1"></span><span id="dmxLedLabel1" style="font-size:.85em;color:#aaa">нет сигнала</span>
</div>
</div>
<div class="card"><h3>Режим тестирования</h3>
<div id="modeIndicator" class="mode-badge safe">Сниффер (приём DMX)
</div>
<div class="row switch-wrap"><label>DMX Тестер:</label><label class="switch"><input type="checkbox" id="dmxTestSw" onchange="toggleDmxTest()"><span class="slider"></span></label><label style="font-size:.8em;color:#aaa">Генератор DMX сигнала</label>
</div>
<div class="row switch-wrap"><label>WS2812 Тест:</label><label class="switch"><input type="checkbox" id="ws2812TestSw" onchange="toggleWs2812Test()"><span class="slider"></span></label><label style="font-size:.8em;color:#aaa">Тест RGB ленты</label>
</div>
<div class="row switch-wrap"><label>Режим патча:</label><label class="switch"><input type="checkbox" id="patchModeSw" onchange="togglePatchMode()"><span class="slider"></span></label><label style="font-size:.8em;color:#aaa">Сканирование и настройка DMX приборов</label>
</div><button onclick="switchToWorkMode()" class="work-btn" style="margin-top:10px">Рабочий режим (Сниффер)</button>
<div class="help">При включении появляется соответствующая вкладка. DMX Тестер — передача DMX данных на шину. WS2812 — прямое управление светодиодами. <b>Кнопка «Рабочий режим»</b> — безопасный возврат к снифферу.
</div>
</div>
<div class="card"><h3>LED лента</h3>
<div class="row"><label>Strip 1 (GPIO23):</label><input type="number" id="totalLeds" value="1000" min="1" max="1000"><button onclick="sendLedCount()">Применить</button>
</div>
<div class="row"><label>Strip 2 (GPIO5):</label><input type="number" id="totalLeds2" value="1000" min="0" max="1000"><button onclick="sendLedCount2()">Применить</button>
</div>
<div class="row switch-wrap" style="margin-top:8px"><label>Реверс пикселей:</label><label class="switch"><input type="checkbox" id="ledReverseSw" onchange="sendLedReverse()"><span class="slider"></span></label><label style="font-size:.8em;color:#aaa">Обратный порядок</label>
</div>
<div class="row switch-wrap"><label>Интерполяция WS2812:</label><label class="switch"><input type="checkbox" id="interpolateSw" onchange="sendInterpolate()"><span class="slider"></span></label><label style="font-size:.8em;color:#aaa">Плавное сглаживание анимации</label>
</div>
<div class="row switch-wrap"><label>Режим лент:</label><select id="ledModeSelect" onchange="sendLedMode()" style="background:#1a1a2e;color:#fff;border:1px solid #444;padding:4px 8px;border-radius:6px"><option value="parallel">Параллельно (2 порта → 2 ленты)</option><option value="sequential">Последовательно (2 порта → 1 лента 2000)</option></select></div>
<div class="row"><label>Сдвиг ленты:</label><input type="range" id="ledShiftSlider" min="0" max="999" value="0" oninput="sendLedShift()"><span class="val" id="ledShiftVal">0</span>
</div>
<div class="help"><b>WS2812B</b> — до 2000 светодиодов (2 ленты × 1000). GPIO23 (Strip1) + GPIO5 (Strip2). <b>Параллельно</b> — DMX порт 0 → Strip1, порт 1 → Strip2 (два независимых эффекта). <b>Последовательно</b> — оба DMX порта формируют одну ленту 2000 LED (первые fixtures на Strip1, остальные на Strip2).
</div>
</div>
<div class="card"><h3>Приборы (DMX)</h3>
<div class="help">Прибор=один RGB/RGBW.fixture. <b>Каналов</b> — 3 (стандарт RGB) или 4 (RGBW). <b>Порядок</b> — как DMX каналы распределяются по цветам WS2812.
</div>
</div>
<div class="card"><h3>Порядок RGB</h3>
<div class="row"><label>Порядок:</label><select id="chOrder" onchange="sendChannelOrder()"><option value="0">RGB</option><option value="1">RBG</option><option value="2">GRB</option><option value="3">GBR</option><option value="4">BRG</option><option value="5">BGR</option></select>
</div>
</div>
<div class="card"><h3>Fallback (нет DMX)</h3>
<div class="row"><label>Цвет:</label><input type="color" id="fbColor" value="#0000ff" onchange="onFallbackColorChange()" style="width:50px;height:30px;border:none;cursor:pointer"><span id="fbHex" style="font-size:.85em;color:#aaa">#0000ff</span>
</div>
<div class="row"><label>Таймаут:</label><input type="number" id="fbTimeout" value="100" min="10" max="5000" style="width:80px" onchange="sendFixtureSettings()"><span style="font-size:.85em;color:#aaa">мс</span>
</div>
<div class="help">Когда нет DMX сигнала дольше таймаута — все WS2812 показывают этот цвет. По умолчанию синий.
</div>
</div>
<div class="card"><h3>Быстрый старт</h3>
<div class="help" style="line-height:1.8"><b>1.</b> Подключи DMX источник к UART1 (GPIO4/2) или UART2 (GPIO16/17)<br><b>2.</b> Включи "DMX Тестер" для проверки передачи DMX<br><b>3.</b> Подключи WS2812 ленту к GPIO23<br><b>4.</b> Включи "WS2812 Тест" для проверки ленты<br><b>5.</b> Настрой патч приборов на вкладке "Патч"<br>
</div>
</div>
<div class="card"><h3>Сохранение</h3>
<div class="row"><button onclick="saveSettings()" style="width:100%">Сохранить настройки</button>
</div><div id="saveResult" style="margin-top:6px;font-size:.85em"></div>
<div class="help">Настройки применяются сразу, но в память NVS сохраняются только по нажатию этой кнопки. После перезагрузки восстанавливаются последние сохранённые настройки.
</div>
</div>
<div class="card"><h3>Обновление ПО</h3>
<div class="row"><input type="file" id="otaFile" accept=".bin" style="display:none" onchange="uploadOTA()"><button onclick="document.getElementById('otaFile').click()" class="secondary">Выбрать файл</button><span id="otaResult" class="ota-result"></span>
</div><progress id="otaProgress" style="width:100%;margin-top:8px;height:8px;border-radius:4px;accent-color:#e94560" value="0" max="100"></progress>
</div>
</div>
</div>
<div class="panel" id="p1">
<div class="card"><h3>Генератор DMX сигнала</h3>
<div class="row"><label>Линия:</label><select id="txLine" onchange="sendTestFull()"><option value="0">Линия 1 (GPIO2)</option><option value="1">Линия 2 (GPIO17)</option><option value="2">Оба</option></select>
</div>
<div class="help">UART линия для передачи DMX кадра. DIR пин переключает RS-485 на передачу.
</div>
</div>
<div class="card"><h3>Режим вывода</h3>
<div class="row"><label>Адресация:</label>
<div style="display:flex;gap:12px;align-items:center"><label style="font-size:.85em;display:flex;align-items:center;gap:4px;cursor:pointer"><input type="radio" name="addrMode" value="ch" checked onchange="changeAddrMode()"> По каналу</label><label style="font-size:.85em;display:flex;align-items:center;gap:4px;cursor:pointer"><input type="radio" name="addrMode" value="px" onchange="changeAddrMode()"> По пикселю</label>
</div>
</div>
<div class="row"><label id="txAddrLabel">Канал:</label><input type="range" id="txChSlider" min="1" max="512" value="1" oninput="onTxSliderChange()"><span class="val" id="txChSliderVal">1</span><span id="txAddrHint" style="font-size:.75em;color:#7a7a9a;margin-left:4px"></span>
</div>
<div class="row"><label>Режим:</label>
<div style="display:flex;gap:12px;align-items:center"><label style="font-size:.85em;display:flex;align-items:center;gap:4px;cursor:pointer"><input type="radio" name="txMode" value="point" checked onchange="changeTxMode()"> Точка</label><label style="font-size:.85em;display:flex;align-items:center;gap:4px;cursor:pointer"><input type="radio" name="txMode" value="fill" onchange="changeTxMode()"> Заливка</label>
</div>
</div>
<div class="row" id="txCountRow" style="display:none"><label id="txCountLabel">Кол-во пикселей:</label><input type="range" id="txCountSlider" min="1" max="512" value="3" oninput='document.getElementById("txCountSliderVal").textContent=this.value,sendTestFull()'><span class="val" id="txCountSliderVal">3</span>
</div>
<div class="help"><b>По каналу</b> — слайдер 1..4096 (DMX канал).<br><b>По пикселю</b> — слайдер 1..1365 (LED пиксель, авто ×3 канала).<br><b>Точка</b> — RGB на 3 канала одного пикселя.<br><b>Заливка</b> — N пикселей одинаковым цветом.
</div>
</div>
<div class="card"><h3>Цвет</h3>
<div class="row"><label>Цвет:</label><input type="color" id="txColor" value="#000000" onchange="colorFromPicker()" style="width:50px;height:35px;border:none;cursor:pointer;background:0 0"><input id="txHex" value="#000000" onchange="colorFromHex()" style="width:80px;background:#0f3460;color:#eee;border:1px solid #333;padding:5px;border-radius:6px;font-family:monospace;font-size:.9em;text-align:center">
</div>
<div class="row"><label style="color:#f44">R:</label><input type="range" id="txR" min="0" max="255" value="0" oninput="colorFromSliders()" style="accent-color:#ff4444"><span class="val" id="txRVal" style="color:#f44">0</span>
</div>
<div class="row"><label style="color:#4f4">G:</label><input type="range" id="txG" min="0" max="255" value="0" oninput="colorFromSliders()" style="accent-color:#44ff44"><span class="val" id="txGVal" style="color:#4f4">0</span>
</div>
<div class="row"><label style="color:#48f">B:</label><input type="range" id="txB" min="0" max="255" value="0" oninput="colorFromSliders()" style="accent-color:#4488ff"><span class="val" id="txBVal" style="color:#48f">0</span>
</div>
<div style="height:20px;border-radius:6px;margin-top:6px" id="txColorPreview">
</div>
<div class="help">Выберите цвет через палитру, HEX код или ползунки R/G/B. Цвет применяется к DMX каналам как R,G,B последовательно.
</div>
</div>
<div class="card"><h3>Остановить DMX</h3><button onclick='stopTx(),showTab("settings"),document.getElementById("dmxTestSw").checked=!1,document.getElementById("tabDmx").style.display="none"' style="width:100%">Завершить тест DMX</button>
</div>
<div id="status1" class="status">
</div>
</div>
<div class="panel" id="p1b">
<div class="card" style="border-color:#e9a020"><h3 style="color:#e9a020">WS2812 LED тест</h3>
<div class="help" style="margin-bottom:8px;border-color:#3a2a00;background:#1a1500">Прямое управление лентой WS2812 через RMT. Миниатюрный DMX-тестер для LED — выбираешь пиксель, цвет, режим и лента сразу реагирует.
</div>
</div>
<div class="card"><h3 style="color:#e9a020">Пиксель</h3>
<div class="row"><label>LED номер:</label><input type="range" id="ledPxSlider" min="0" max="999" value="0" oninput="onLedPxChange()"><span class="val" id="ledPxSliderVal" style="color:#e9a020">0</span><span id="ledPxHint" style="font-size:.75em;color:#7a7a9a;margin-left:4px"></span>
</div>
<div class="row"><label>Режим:</label>
<div style="display:flex;gap:12px;align-items:center"><label style="font-size:.85em;display:flex;align-items:center;gap:4px;cursor:pointer"><input type="radio" name="ledMode" value="point" checked onchange="changeLedMode()"> Точка</label><label style="font-size:.85em;display:flex;align-items:center;gap:4px;cursor:pointer"><input type="radio" name="ledMode" value="fill" onchange="changeLedMode()"> Заливка</label>
</div>
</div>
<div class="row" id="ledCountRow" style="display:none"><label>Кол-во:</label><input type="range" id="ledCountSlider" min="1" max="1000" value="10" oninput='document.getElementById("ledCountSliderVal").textContent=this.value,sendLedTest()'><span class="val" id="ledCountSliderVal">10</span>
</div>
</div>
<div class="card"><h3 style="color:#e9a020">Цвет WS2812</h3>
<div class="row"><label>Цвет:</label><input type="color" id="ledColor" value="#000000" onchange="ledColorFromPicker()" style="width:50px;height:35px;border:none;cursor:pointer;background:0 0"><input id="ledHex" value="#000000" onchange="ledColorFromHex()" style="width:80px;background:#0f3460;color:#eee;border:1px solid #333;padding:5px;border-radius:6px;font-family:monospace;font-size:.9em;text-align:center">
</div>
<div class="row"><label style="color:#f44">R:</label><input type="range" id="ledR" min="0" max="255" value="0" oninput="ledColorFromSliders()" style="accent-color:#ff4444"><span class="val" id="ledRVal" style="color:#f44">0</span>
</div>
<div class="row"><label style="color:#4f4">G:</label><input type="range" id="ledG" min="0" max="255" value="0" oninput="ledColorFromSliders()" style="accent-color:#44ff44"><span class="val" id="ledGVal" style="color:#4f4">0</span>
</div>
<div class="row"><label style="color:#48f">B:</label><input type="range" id="ledB" min="0" max="255" value="0" oninput="ledColorFromSliders()" style="accent-color:#4488ff"><span class="val" id="ledBVal" style="color:#48f">0</span>
</div>
<div style="height:20px;border-radius:6px;margin-top:6px;background:#000" id="ledColorPreview">
</div>
</div>
<div class="card"><h3 style="color:#e9a020">Завершить тест WS2812</h3><button onclick='ledStopAll(),showTab("settings"),document.getElementById("ws2812TestSw").checked=!1,document.getElementById("tabWs").style.display="none"' style="width:100%;background:#e9a020">Завершить тест WS2812</button>
</div>
<div id="status10" class="status">
</div>
</div>
<div class="panel" id="p2">
<div class="card"><h3>DMX Сканер</h3>
<div class="row"><label>Прибор #:</label>
<div style="display:flex;gap:6px;align-items:center"><button onclick="scanPrev()" class="secondary">◀</button><input type="number" id="scanAddr" value="1" min="1" max="512" style="width:70px;text-align:center" onchange="onScanAddrChange()"><button onclick="scanNext()" class="secondary">▶</button><button onclick="scanTestAddr()">Тест ▶</button>
</div>
</div>
<div class="row"><label>Линия:</label><select id="scanLine"><option value="1">Линия 1</option><option value="2">Линия 2</option></select>
</div>
<div style="display:flex;gap:8px;margin-top:8px"><button onclick='scanMark("circle")' style="background:#0a8f0a">Добавить</button><button onclick="scanSkip()" style="background:#c73650">Пропустить</button>
</div>
<div class="help" style="margin-top:8px"><b>Тест</b> — отправляет белый цвет на прибор (RGB=255,255,255). Головка загорается — определи где она стоит.<br><b>Добавить</b> — добавить прибор в патч. <b>Пропустить</b> — перейти к следующему прибору.
</div>
</div>
<div class="card"><h3>Головки на круге (<span id="patchCount">0</span> шт)</h3><table id="patchTbl"><thead><tr><th>#</th><th>Прибор</th><th>Линия</th><th style="font-size:.75em">Адрес</th><th></th><th></th><th></th><th></th></tr></thead><tbody></tbody></table>
<div style="margin-top:10px;display:flex;gap:8px;flex-wrap:wrap"><button onclick="addPatchRow()">+ Добавить</button><button onclick="savePatch()">Сохранить патч</button><button onclick="testPatchCycle()" style="background:#0a8f0a">▶ Тест патча</button>
</div>
<div class="row" style="margin-top:8px"><label>Диапазон С:</label><input type="number" id="rangeFrom" value="1" min="1" max="512" style="width:60px"><label>по:</label><input type="number" id="rangeTo" value="10" min="1" max="512" style="width:60px"><button onclick="addPatchRange()" class="secondary">Добавить диапазон</button><button onclick="delPatchRange()" style="background:#c73650;margin-left:6px">Удалить диапазон</button>
</div>
<div class="help" style="margin-top:8px"><b>Тест ▶</b> — проверить адрес (отправляет белый цвет). <b>▲▼</b> — изменить порядок. <b>X</b> — удалить.<br><b>Тест патча</b> — циклически проверяет все приборы по очереди (1.5 сек). Нажми ещё раз для остановки.<br><b>Диапазон</b> — добавить несколько приборов разом (С — По).
</div>
</div>
<div class="card" id="patchInfoCard" style="display:none"><h3>Маппинг</h3>
<div id="patchInfo" class="help">
</div>
</div>
<div id="status2" class="status">
</div>
</div>
<div class="panel" id="p4">
<div class="card">
<div class="info-mode"><button id="infoMode2row" class="active" onclick="setInfoMode('2row')">2 ряда</button><button id="infoModeCircles" onclick="setInfoMode('circles')">RGB полоса</button>
</div>
</div>
<div id="info2RowView">
<div class="card"><h3>DMX (порт 1)</h3>
<div class="dmx-bar-wrap"><canvas id="bar0a" height="140"></canvas>
</div>
<div class="dmx-bar-lbl"><span>1</span><span>64</span><span>128</span><span>192</span><span>255</span>
</div>
<div class="dmx-bar-wrap"><canvas id="bar0b" height="140"></canvas>
</div>
<div class="dmx-bar-lbl"><span>256</span><span>320</span><span>384</span><span>448</span><span>512</span>
</div>
</div>
<div class="card"><h3>DMX (порт 2)</h3>
<div class="dmx-bar-wrap"><canvas id="bar1a" height="140"></canvas>
</div>
<div class="dmx-bar-lbl"><span>1</span><span>64</span><span>128</span><span>192</span><span>255</span>
</div>
<div class="dmx-bar-wrap"><canvas id="bar1b" height="140"></canvas>
</div>
<div class="dmx-bar-lbl"><span>256</span><span>320</span><span>384</span><span>448</span><span>512</span>
</div>
</div>
</div>
<div id="infoCircleView" style="display:none">
<div class="card"><h3>DMX RGB (порт 1)</h3>
<div class="led-preview-wrap"><canvas id="circles0"></canvas>
</div>
</div>
<div class="card"><h3>DMX RGB (порт 2)</h3>
<div class="led-preview-wrap"><canvas id="circles1"></canvas>
</div>
</div>
</div>
<div class="card" style="margin-top:4px"><h3>LED strip output</h3>
<div class="led-preview-wrap"><canvas id="ledCanvas"></canvas>
</div>
</div>
</div>
<div id="warnModal" class="modal-overlay">
<div class="modal-box"><h3>⚠️ ВНИМАНИЕ!</h3><p id="warnModalText">Переход в режим передачи DMX.<br><b>Отключите контроллер от шины DMX</b> (вытащите разъём) перед продолжением!</p>
<div class="btn-row"><button onclick="handleWarnModal(!1)" class="secondary">Отмена</button><button onclick="handleWarnModal(!0)">Понял</button>
</div>
</div>
</div>
<script>var maxChVal=512,panelMap={settings:"p0",dmx:"p1",ws:"p1b",patch:"p2",info:"p4"},_warnCb=null;function showWarnModal(e,t){document.getElementById("warnModalText").innerHTML=e||"Переход в режим передачи DMX.<br><b>Отключите контроллер от шины DMX</b> (вытащите разъём) перед продолжением!",document.getElementById("warnModal").classList.add("open"),_warnCb=t}function handleWarnModal(e){if(document.getElementById("warnModal").classList.remove("open"),_warnCb){var t=_warnCb;_warnCb=null,e&&t()}}function updateModeIndicator(){var e=document.getElementById("modeIndicator");if(e){var t=document.getElementById("dmxTestSw").checked,n=document.getElementById("ws2812TestSw").checked;t?(e.className="mode-badge warn",e.textContent="Передача DMX"):n?(e.className="mode-badge test",e.textContent="WS2812 Тест"):document.getElementById("patchModeSw").checked?(e.className="mode-badge warn",e.textContent="Патч"):(e.className="mode-badge safe",e.textContent="Сниффер (приём DMX)")}}function switchToWorkMode(){document.getElementById("dmxTestSw").checked=!1,document.getElementById("ws2812TestSw").checked=!1,document.getElementById("patchModeSw").checked=!1,document.getElementById("tabPatch").style.display="none",api("POST","/api/mode",{mode:"sniffer"}),ledStopAll(),showTab("settings"),updateModeIndicator(),showStatus(0,"Рабочий режим (Сниффер)",!0)}function showTab(e){document.querySelectorAll(".tab").forEach(e=>e.classList.remove("active")),document.querySelectorAll(".panel").forEach(e=>e.classList.remove("active"));const t=["settings","dmx","ws","patch","info"],n=document.querySelectorAll(".tab");for(let d=0;d<n.length;d++)t[d]===e&&n[d].classList.add("active");const d=document.getElementById(panelMap[e]);d&&d.classList.add("active");_infoPanelVisible=(e==="info");if(e==="info"){fetchInfoData()}}function toggleDmxTest(){document.getElementById("dmxTestSw").checked?(document.getElementById("dmxTestSw").checked=!1,showWarnModal(null,function(){document.getElementById("dmxTestSw").checked=!0,document.getElementById("tabDmx").style.display="",api("POST","/api/mode",{mode:"tester"}).then(function(e){showStatus(0,e.ok?"DMX Тестер включён":"Ошибка",!!e.ok),updateModeIndicator()}),showTab("dmx")})):(document.getElementById("tabDmx").style.display="none",api("POST","/api/mode",{mode:"sniffer"}).then(function(e){showStatus(0,e.ok?"Сниффер":"Ошибка",!!e.ok),updateModeIndicator()}),stopTx(),showTab("settings"))}function toggleWs2812Test(){document.getElementById("ws2812TestSw").checked?(document.getElementById("ws2812TestSw").checked=!1,showWarnModal("Тест WS2812 светодиодной ленты.<br><b>Убедитесь, что лента подключена к GPIO23</b>",function(){document.getElementById("ws2812TestSw").checked=!0,document.getElementById("tabWs").style.display="",showTab("ws"),api("POST","/api/led_test",{pixel:0,r:0,g:0,b:0,mode:"point",count:1})})):(document.getElementById("tabWs").style.display="none",ledStopAll(),showTab("settings"))}function togglePatchMode(){var e=document.getElementById("patchModeSw");e.checked?(e.checked=!1,showWarnModal("Режим патча: устройство передаёт DMX на шину.<br><b>Отключите DMX контроллер от шины</b> перед продолжением!",function(){e.checked=!0,document.getElementById("tabPatch").style.display="",api("POST","/api/mode",{mode:"patch"}).then(function(n){showStatus(0,n.ok?"Режим патча включён":"Ошибка",!!n.ok),updateModeIndicator()}),showTab("patch")})):(document.getElementById("tabPatch").style.display="none",api("POST","/api/mode",{mode:"sniffer"}).then(function(n){showStatus(0,n.ok?"Сниффер":"Ошибка",!!n.ok),updateModeIndicator()}),showTab("settings"))}function api(e,t,n){return fetch(t,{method:e,headers:{"Content-Type":"application/json"},body:n?JSON.stringify(n):void 0}).then(e=>e.json()).catch(e=>({error:e.message}))}function uploadOTA(){
    var f=document.getElementById("otaFile").files[0];
    if(!f)return;
    var r=document.getElementById("otaResult");
    var p=document.getElementById("otaProgress");
    var sz=(f.size/1024).toFixed(0);
    r.innerHTML="<b>Загрузка... ("+sz+" KB)</b>";r.className="ota-result";
    p.value=0;
    var x=new XMLHttpRequest;
    x.open("POST","/api/ota");
    x.upload.onprogress=function(e){
        if(e.lengthComputable)p.value=Math.round(e.loaded/e.total*100)
    };
    x.onreadystatechange=function(){
        if(x.readyState==4){
            if(x.status==200){
                try{
                    var j=JSON.parse(x.responseText);
                    if(j.ok){r.innerHTML="<b>OK, перезагрузка...</b>";r.className="ota-result ok";setTimeout(function(){location.reload()},1000)}
                    else{r.innerHTML="<b>Ошибка: "+(j.error||"?")+"</b>";r.className="ota-result err"}
                }catch(e){r.innerHTML="<b>Ошибка: HTTP "+x.status+"</b>";r.className="ota-result err"}
            }else{r.innerHTML="<b>Ошибка: HTTP "+x.status+"</b>";r.className="ota-result err"}
        }
    };
    x.onerror=function(){r.innerHTML="<b>Ошибка сети</b>";r.className="ota-result err"};
    x.send(f)
}
function showStatus(e,t,n){var d=document.getElementById("status"+e);if(!d)return;d.textContent=t,d.className="status "+(n?"ok":"err"),setTimeout(()=>d.textContent="",3e3)}function sendSpeed(e){}function sendLedCount(){var e=parseInt(document.getElementById("totalLeds").value)||1e3;api("POST","/api/leds",{count:e}).then(t=>{t.ok?(document.getElementById("ledPxSlider").max=Math.max(0,e-1),document.getElementById("ledCountSlider").max=e,document.getElementById("ledShiftSlider").max=Math.max(0,e-1),showStatus(0,"Strip 1: "+e+" LED",!0),fetchInfoData()):showStatus(0,"Ошибка",!1)})}function sendLedCount2(){var e=parseInt(document.getElementById("totalLeds2").value)||0;api("POST","/api/leds2",{count:e}).then(t=>{t.ok?showStatus(0,"Strip 2: "+e+" LED",!0):showStatus(0,"Ошибка",!1)})}function sendLedReverse(){var e=document.getElementById("ledReverseSw").checked;api("POST","/api/led_reverse",{reverse:e}).then(t=>showStatus(0,t.ok?"Реверс: "+e:"Ошибка",!!t.ok))}function sendLedMode(){var e=document.getElementById("ledModeSelect").value;api("POST","/api/led_mode",{mode:e}).then(t=>showStatus(0,t.ok?"Режим: "+e:"Ошибка",!!t.ok))}function sendLedShift(){var e=parseInt(document.getElementById("ledShiftSlider").value)||0;document.getElementById("ledShiftVal").textContent=e;api("POST","/api/led_shift",{shift:e})}
function sendMaxChannels(){}function updateTxSliderMax(){var e=3;document.getElementById("txChSlider").max=512,document.getElementById("txChSliderVal").textContent=Math.min(512,parseInt(document.getElementById("txChSlider").value)),document.getElementById("txCountSlider").max=Math.floor(512/e);var t=document.querySelector("input[name=txMode]:checked");t&&"fill"===t.value?document.getElementById("txCountRow").style.display="":document.getElementById("txCountRow").style.display="none";}function colorFromSliders(){var e=parseInt(document.getElementById("txR").value),t=parseInt(document.getElementById("txG").value),n=parseInt(document.getElementById("txB").value);document.getElementById("txRVal").textContent=e,document.getElementById("txGVal").textContent=t,document.getElementById("txBVal").textContent=n;var d="#"+[e,t,n].map(e=>e.toString(16).padStart(2,"0")).join("");document.getElementById("txHex").value=d,document.getElementById("txColor").value=d,document.getElementById("txColorPreview").style.background=d,sendTestFull()}function colorFromPicker(){var e=document.getElementById("txColor").value,t=parseInt(e.slice(1,3),16),n=parseInt(e.slice(3,5),16),d=parseInt(e.slice(5,7),16);document.getElementById("txR").value=t,document.getElementById("txG").value=n,document.getElementById("txB").value=d,document.getElementById("txRVal").textContent=t,document.getElementById("txGVal").textContent=n,document.getElementById("txBVal").textContent=d,document.getElementById("txHex").value=e,document.getElementById("txColorPreview").style.background=e,sendTestFull()}function colorFromHex(){var e=document.getElementById("txHex").value;if(/^#[0-9a-fA-F]{6}$/.test(e)){document.getElementById("txColor").value=e;var t=parseInt(e.slice(1,3),16),n=parseInt(e.slice(3,5),16),d=parseInt(e.slice(5,7),16);document.getElementById("txR").value=t,document.getElementById("txG").value=n,document.getElementById("txB").value=d,document.getElementById("txRVal").textContent=t,document.getElementById("txGVal").textContent=n,document.getElementById("txBVal").textContent=d,document.getElementById("txColorPreview").style.background=e,sendTestFull()}}function changeTxMode(){var e=document.querySelector("input[name=txMode]:checked").value;document.getElementById("txCountRow").style.display="fill"===e?"flex":"none",sendTestFull()}function getTxMode(){return document.querySelector("input[name=txMode]:checked").value}function changeAddrMode(){"px"===document.querySelector("input[name=addrMode]:checked").value?(document.getElementById("txAddrLabel").textContent="Пиксель:",document.getElementById("txCountLabel").textContent="Кол-во пикселей:"):(document.getElementById("txAddrLabel").textContent="Канал:",document.getElementById("txCountLabel").textContent="Кол-во каналов:"),updateTxSliderMax(),onTxSliderChange(),sendTestFull()}function getAddrMode(){return document.querySelector("input[name=addrMode]:checked").value}function onTxSliderChange(){var e=parseInt(document.getElementById("txChSlider").value);document.getElementById("txChSliderVal").textContent=e;var t=getAddrMode(),n=document.getElementById("txAddrHint");if("px"===t){var d=3*(e-1)+1;n.textContent="→ CH"+d+","+(d+1)+","+(d+2)}else{var a=Math.floor((e-1)/3)+1;n.textContent="≈ пиксель #"+a}sendTestFull()}function sendTestFull(){var e,t=parseInt(document.getElementById("txLine").value),n=parseInt(document.getElementById("txChSlider").value)||1,d=getAddrMode();e="px"===d?3*(n-1)+1:n;var a=parseInt(document.getElementById("txR").value)||0,l=parseInt(document.getElementById("txG").value)||0,o=parseInt(document.getElementById("txB").value)||0,c=getTxMode(),u=parseInt(document.getElementById("txCountSlider").value)||1;"px"===d&&"fill"===c&&(u*=3),api("POST","/api/test",{line:t,channel:e,r:a,g:l,b:o,mode:c,count:u,tx_speed:250000,tx_packet_len:512}).then(e=>showStatus(1,e.ok?"Отправлено":"Ошибка",!!e.ok))}function stopTx(){api("POST","/api/test",{r:0,g:0,b:0,mode:"point",count:1,tx_speed:25e4,tx_packet_len:512})}function onLedPxChange(){var e=parseInt(document.getElementById("ledPxSlider").value);document.getElementById("ledPxSliderVal").textContent=e;var t=parseInt(document.getElementById("totalLeds").value)||1e3;document.getElementById("ledPxHint").textContent="из "+(t-1),sendLedTest()}function changeLedMode(){var e=document.querySelector("input[name=ledMode]:checked").value;document.getElementById("ledCountRow").style.display="fill"===e?"flex":"none",sendLedTest()}function getLedMode(){return document.querySelector("input[name=ledMode]:checked").value}function ledColorFromSliders(){var e=parseInt(document.getElementById("ledR").value),t=parseInt(document.getElementById("ledG").value),n=parseInt(document.getElementById("ledB").value);document.getElementById("ledRVal").textContent=e,document.getElementById("ledGVal").textContent=t,document.getElementById("ledBVal").textContent=n;var d="#"+[e,t,n].map(e=>e.toString(16).padStart(2,"0")).join("");document.getElementById("ledHex").value=d,document.getElementById("ledColor").value=d,document.getElementById("ledColorPreview").style.background=d,sendLedTest()}function ledColorFromPicker(){var e=document.getElementById("ledColor").value,t=parseInt(e.slice(1,3),16),n=parseInt(e.slice(3,5),16),d=parseInt(e.slice(5,7),16);document.getElementById("ledR").value=t,document.getElementById("ledG").value=n,document.getElementById("ledB").value=d,document.getElementById("ledRVal").textContent=t,document.getElementById("ledGVal").textContent=n,document.getElementById("ledBVal").textContent=d,document.getElementById("ledHex").value=e,document.getElementById("ledColorPreview").style.background=e,sendLedTest()}function ledColorFromHex(){var e=document.getElementById("ledHex").value;if(/^#[0-9a-fA-F]{6}$/.test(e)){document.getElementById("ledColor").value=e;var t=parseInt(e.slice(1,3),16),n=parseInt(e.slice(3,5),16),d=parseInt(e.slice(5,7),16);document.getElementById("ledR").value=t,document.getElementById("ledG").value=n,document.getElementById("ledB").value=d,document.getElementById("ledRVal").textContent=t,document.getElementById("ledGVal").textContent=n,document.getElementById("ledBVal").textContent=d,document.getElementById("ledColorPreview").style.background=e,sendLedTest()}}function sendInterpolate(){api("POST","/api/interpolate",{interpolate:document.getElementById("interpolateSw").checked}).then(e=>showStatus(2,e.ok?"Отправлено":"Ошибка",!!e.ok))}function sendLedTest(){api("POST","/api/led_test",{pixel:parseInt(document.getElementById("ledPxSlider").value)||0,r:parseInt(document.getElementById("ledR").value)||0,g:parseInt(document.getElementById("ledG").value)||0,b:parseInt(document.getElementById("ledB").value)||0,mode:getLedMode(),count:parseInt(document.getElementById("ledCountSlider").value)||1}).then(e=>showStatus(10,e.ok?"Отправлено":"Ошибка",!!e.ok))}function ledStopAll(){api("POST","/api/led_test",{pixel:0,r:0,g:0,b:0,mode:"clear",count:0}).then(e=>showStatus(10,e.ok?"WS2812 выключены":"Ошибка",!!e.ok))}function scanPrev(){var e=document.getElementById("scanAddr"),t=parseInt(e.value)||1;t>1&&(e.value=t-1),scanTestAddr()}function scanNext(){var e=document.getElementById("scanAddr");(parseInt(e.value)||1)<(parseInt(e.max)||512)&&(e.value=parseInt(e.value)+1),scanTestAddr()}function onScanAddrChange(){var e=document.getElementById("scanAddr"),t=parseInt(e.value)||1,n=parseInt(e.max)||512;t<1&&(t=1),t>n&&(t=n),e.value=t,scanTestAddr()}function scanTestAddr(){var e=parseInt(document.getElementById("scanAddr").value)||1,t=parseInt(document.getElementById("scanLine").value)||1,n=3,d=(e-1)*n+1;api("POST","/api/dmx_addr_test",{line:t-1,addr:d,r:255,g:255,b:255}).then(n=>showStatus(2,n.ok?"Тест прибора #"+e+" (адрес "+d+", линия "+t+")":"Ошибка",!!n.ok))}function scanMark(e){var t=parseInt(document.getElementById("scanAddr").value)||1,n=parseInt(document.getElementById("scanLine").value)||1;if("circle"===e){3;addPatchRowAt(t,n),savePatch()}scanNext(),scanTestAddr()}function scanSkip(){scanNext(),scanTestAddr()}function addPatchRow(){document.querySelector("#patchTbl tbody");addPatchRowAt(parseInt(document.getElementById("scanAddr").value)||1,parseInt(document.getElementById("scanLine").value)||1),savePatch()}function onFixtureChange(i){var t=i.closest("tr"),n=((parseInt(i.value)||1)-1)*3+1;t.children[3].textContent=n;savePatch()}
function addPatchRowAt(e,t){var n=document.querySelector("#patchTbl tbody"),d=n.rows.length+1,a=(e-1)*(3)+1,l=document.createElement("tr");l.innerHTML="<td>"+d+'</td><td><input type="number" value="'+e+'" min="1" max="512" onchange="onFixtureChange(this)"></td><td><select><option value="1"'+(1==t?" selected":"")+'>Линия 1</option><option value="2"'+(2==t?" selected":"")+'>Линия 2</option></select></td><td style="font-size:.8em;color:#aaa">'+a+'</td><td><button class="secondary" onclick="testPatchRow(this)">&#9654;</button></td><td><button class="secondary" onclick="movePatchRow(this,-1)">&#9650;</button></td><td><button class="secondary" onclick="movePatchRow(this,1)">&#9660;</button></td><td><button class="secondary" onclick="removePatchRow(this)">X</button></td>',n.appendChild(l),renumberPatch()}function removePatchRow(e){e.closest("tr").remove(),renumberPatch(),savePatch()}function movePatchRow(e,t){var n=e.closest("tr"),d=n.parentNode,a=Array.from(d.children).indexOf(n)+t;a<0||a>=d.children.length||(t<0?d.insertBefore(n,d.children[a]):d.insertBefore(n,d.children[a].nextSibling),renumberPatch(),savePatch())}function testPatchRow(e){var t=e.closest("tr"),n=t.querySelectorAll("input"),d=t.querySelector("select"),a=parseInt(n[0].value)||1,l=parseInt(d.value)||1,o=3,c=(a-1)*o+1;t.classList.add("test-active"),setTimeout(function(){t.classList.remove("test-active")},2e3),api("POST","/api/dmx_addr_test",{line:l-1,addr:c,r:255,g:255,b:255}).then(e=>showStatus(2,e.ok?"Тест прибора #"+a:"Ошибка",!!e.ok))}document.getElementById("txChSlider").addEventListener("input",onTxSliderChange);var _cycleTimer=null;function testPatchCycle(){var e=document.querySelectorAll("#patchTbl tbody tr");if(e.length){if(_cycleTimer)return clearInterval(_cycleTimer),_cycleTimer=null,void showStatus(2,"Цикл остановлен",!0);var t=0;n(),_cycleTimer=setInterval(n,1500)}function n(){if(t>=e.length)return clearInterval(_cycleTimer),_cycleTimer=null,void showStatus(2,"Цикл завершён",!0);e.forEach(function(e){e.classList.remove("test-active")}),e[t].classList.add("test-active");var n=e[t].querySelectorAll("input"),d=e[t].querySelector("select"),a=parseInt(n[0].value)||1;api("POST","/api/dmx_addr_test",{line:(parseInt(d.value)||1)-1,addr:(a-1)*(3)+1,r:255,g:255,b:255}),showStatus(2,"Цикл: прибор #"+a,t<e.length-1),t++}}function addPatchRange(){var e=parseInt(document.getElementById("rangeFrom").value)||1,t=parseInt(document.getElementById("rangeTo").value)||e,n=parseInt(document.getElementById("scanLine").value)||1;if(e>t){var d=e;e=t,t=d}var a=parseInt(document.getElementById("scanAddr").max)||512;e<1&&(e=1),t>a&&(t=a);for(var l=e;l<=t;l++)addPatchRowAt(l,n);savePatch(),showStatus(2,"Добавлено приборов: "+(t-e+1),!0)}function delPatchRange(){var e=parseInt(document.getElementById("rangeFrom").value)||1,t=parseInt(document.getElementById("rangeTo").value)||e;if(e>t){var d=e;e=t,t=d}var n=t-e+1;if(n>0){var a=document.querySelectorAll("#patchTbl tbody tr");if(a.length>=n){for(var l=a.length-1;l>=a.length-n;l--)a[l].remove();savePatch(),showStatus(2,"Удалено приборов: "+n,!0),renumberPatch()}else showStatus(2,"Недостаточно приборов для удаления",!1)}}function renumberPatch(){for(var e=document.querySelector("#patchTbl tbody"),t=0;t<e.children.length;t++)e.children[t].children[0].textContent=t+1;document.getElementById("patchCount").textContent=e.children.length,updatePatchInfo()}function updatePatchInfo(){var e=document.querySelector("#patchTbl tbody").children.length,t=parseInt(document.getElementById("totalLeds").value)||1e3,n=document.getElementById("patchInfoCard");if(e>0){n.style.display="";var d=e>0?(t/e).toFixed(1):"-";document.getElementById("patchInfo").innerHTML="<b>"+e+"</b> DMX головок &rarr; <b>"+t+"</b> WS2812 пикселей (1 головка &asymp; <b>"+d+"</b> пикс.)"}else n.style.display="none"}function savePatch(){var r=document.querySelectorAll("#patchTbl tbody tr"),c="fixture,universe\n";r.forEach(function(row){var i=row.querySelector("input[type=number]"),s=row.querySelector("select");c+=(parseInt(i.value)||1)+","+(2==parseInt(s.value)?2:1)+"\n"}),fetch("/api/patch",{method:"POST",headers:{"Content-Type":"text/plain"},body:c}).then(function(d){return d.json()}).then(function(d){showStatus(2,d.ok?"Патч сохранён ("+r.length+" приборов)":"Ошибка",!!d.ok)}).catch(function(){})}function loadPatch(){api("GET","/api/patch").then(function(e){if(Array.isArray(e)){document.querySelector("#patchTbl tbody").innerHTML="";var t=3;e.forEach(function(e,n){addPatchRowAt(Math.floor((e.dmx_addr-1)/t)+1,e.universe||1)}),renumberPatch()}})}function sendChannelOrder(){var e=parseInt(document.getElementById("chOrder").value);api("POST","/api/fixture_settings",{channel_order:e}).then(function(t){showStatus(0,t.ok?"Настройки приборов сохранены":"Ошибка",!!t.ok)})}function sendFixtureSettings(){var n=document.getElementById("fbColor").value;api("POST","/api/fixture_settings",{fallback_r:parseInt(n.slice(1,3),16),fallback_g:parseInt(n.slice(3,5),16),fallback_b:parseInt(n.slice(5,7),16),fallback_timeout_ms:parseInt(document.getElementById("fbTimeout").value)||100}).then(function(e){updateTxSliderMax(),showStatus(0,e.ok?"Настройки приборов сохранены":"Ошибка",!!e.ok)})}function onFallbackColorChange(){var e=document.getElementById("fbColor").value;document.getElementById("fbHex").textContent=e,sendFixtureSettings()}function initSettings(){api("GET","/api/settings").then(e=>{if(!e.error){if(document.getElementById("totalLeds").value=e.led_count,document.getElementById("ledReverseSw").checked=!!e.led_reverse,"fill"===e.tx_mode?document.querySelector("input[name=txMode][value=fill]").checked=!0:document.querySelector("input[name=txMode][value=point]").checked=!0,document.getElementById("dmxTestSw").checked=!1,document.getElementById("ws2812TestSw").checked=!1,e.led_count&&(document.getElementById("ledPxSlider").max=Math.max(0,e.led_count-1),document.getElementById("ledCountSlider").max=e.led_count),void 0!==e.fallback_r){var t="#"+[e.fallback_r,e.fallback_g,e.fallback_b].map(function(e){return e.toString(16).padStart(2,"0")}).join("");document.getElementById("fbColor").value=t,document.getElementById("fbHex").textContent=t}void 0!==e.channel_order&&(document.getElementById("chOrder").value=e.channel_order),e.fallback_timeout_ms&&(document.getElementById("fbTimeout").value=e.fallback_timeout_ms),updateTxSliderMax(),updateModeIndicator()}})}loadPatch(),initSettings(),document.getElementById("txColorPreview").style.background="#000000";function updateDmxLeds(){fetch("/api/settings").then(r=>r.json()).then(d=>{for(let i=0;i<2;i++){let led=document.getElementById("dmxLed"+i),lb=document.getElementById("dmxLedLabel"+i);if(led&&lb){let on=d["dmx"+i]==1;led.className="dmx-led"+(on?" on":"");lb.textContent=on?"сигнал есть":"нет сигнала";}}}).catch(()=>{});}setInterval(updateDmxLeds,500);setTimeout(updateDmxLeds,500);
function rgbToHex(r,g,b){return "#"+[r,g,b].map(function(x){return Math.min(255,Math.max(0,Math.round(x))).toString(16).padStart(2,"0")}).join("")}
var g_infoMode="2row";var _patch=null,_settings=null;
var _orderMap=[[0,1,2],[0,2,1],[1,0,2],[1,2,0],[2,0,1],[2,1,0]];
function setInfoMode(m){g_infoMode=m;
document.querySelectorAll(".info-mode button").forEach(function(b){b.classList.remove("active")});
document.getElementById("infoMode"+m.charAt(0).toUpperCase()+m.slice(1)).classList.add("active");
document.getElementById("info2RowView").style.display=m==="2row"?"":"none";
document.getElementById("infoCircleView").style.display=m==="circles"?"":"none"}
function _ensureCanvas(id,w,h){var c=document.getElementById(id);if(!c)return null;if(c.width!==w||c.height!==h){c.width=w;c.height=h}return c}
function drawBars(canvasId,ch,offset,n){var wrap=document.getElementById(canvasId);if(!wrap)return;var par=wrap.parentElement;if(!par)return;var w=par.clientWidth,h=140;var c=_ensureCanvas(canvasId,w,h);if(!c)return;var ctx=c.getContext("2d");var img=ctx.createImageData(w,h);var d=img.data;var bw=w/n;for(var x=0;x<n&&x+offset<512;x++){var v=ch[x+offset];var bh=Math.max(Math.round(v/255*h),1);var cr=Math.round(40+v*0.84),cg=cr,cb=Math.round(v*0.7);var x0=Math.floor(x*bw),x1=Math.floor((x+1)*bw);for(var px=x0;px<x1;px++){for(var y=h-bh;y<h;y++){var p=(y*w+px)*4;d[p]=cr;d[p+1]=cg;d[p+2]=cb;d[p+3]=255}for(var y=0;y<h-bh;y++){var p=(y*w+px)*4;d[p]=26;d[p+1]=26;d[p+2]=46;d[p+3]=255}}}ctx.putImageData(img,0,0)}
function drawCircles(canvasId,ch){var n=0;for(var i=0;i<170&&i*3+2<512;i++)n++;var par=document.getElementById(canvasId);if(!par)return;var w=par.parentElement?par.parentElement.clientWidth:n;var c=_ensureCanvas(canvasId,w,1);if(!c)return;var ctx=c.getContext("2d");var img=ctx.createImageData(w,1);var d=img.data;for(var px=0;px<w;px++){var si=Math.floor(px*n/w);if(si>=n)si=n-1;var p=px*4;d[p]=ch[si*3];d[p+1]=ch[si*3+1];d[p+2]=ch[si*3+2];d[p+3]=255}ctx.putImageData(img,0,0)}
function computeLedPreview(dmx){if(!_patch||!_settings)return null;var n=_settings.led_count||0,interp=_settings.interpolate,fixtures=_patch;if(!fixtures||!fixtures.length||n<1)return new Uint8Array(0);
var fcolors=[];for(var f=0;f<fixtures.length;f++){var fi=fixtures[f],port=(fi.universe===2)?1:0,addr=fi.dmx_addr-1,buf=dmx.subarray(port*512,port*512+512);fcolors.push([buf[addr]||0,buf[addr+1]||0,buf[addr+2]||0])}
var led=new Uint8Array(n*3);if(interp){if(fixtures.length<2||n<2){var c=fcolors[0]||[0,0,0];for(var i=0;i<n;i++){led[i*3]=c[0];led[i*3+1]=c[1];led[i*3+2]=c[2]}}else{for(var i=0;i<n;i++){var p=i*(fixtures.length-1)*256/(n-1),lo=p>>8,hi=lo+1,w=p&255;if(hi>=fixtures.length)hi=fixtures.length-1;led[i*3]=(fcolors[lo][0]*(256-w)+fcolors[hi][0]*w)>>8;led[i*3+1]=(fcolors[lo][1]*(256-w)+fcolors[hi][1]*w)>>8;led[i*3+2]=(fcolors[lo][2]*(256-w)+fcolors[hi][2]*w)>>8}}}else{for(var f=0;f<fixtures.length;f++){var s=Math.floor(f*n/fixtures.length),e=Math.floor((f+1)*n/fixtures.length),c=fcolors[f];for(var j=s;j<e&&j<n;j++){led[j*3]=c[0];led[j*3+1]=c[1];led[j*3+2]=c[2]}}}return led}
function renderLedPreviewFromLed(led){var c=_ensureCanvas("ledCanvas",Math.max(led.length/3,1),1);if(!c)return;var n=led.length/3;if(n<1)return;var ctx=c.getContext("2d");var img=ctx.createImageData(n,1);var d=img.data;for(var i=0;i<n;i++){var p=i*4;d[p]=led[i*3];d[p+1]=led[i*3+1];d[p+2]=led[i*3+2];d[p+3]=255}ctx.putImageData(img,0,0)}
var _fetchBusy=false;var _infoPanelVisible=false;
function fetchInfoData(){if(!_infoPanelVisible||_fetchBusy)return;_fetchBusy=true;fetch("/api/blob").then(function(r){return r.arrayBuffer()}).then(function(b){var d=new Uint8Array(b);if(d.length<1024){_fetchBusy=false;return}if(g_infoMode==="2row"){for(var port=0;port<2;port++){var off=port*512,ch=d.subarray(off,off+512);drawBars("bar"+port+"a",ch,0,255);drawBars("bar"+port+"b",ch,255,257)}}else if(g_infoMode==="circles"){for(var port=0;port<2;port++){var off=port*512,ch=d.subarray(off,off+512);drawCircles("circles"+port,ch)}}var led=computeLedPreview(d);if(led)renderLedPreviewFromLed(led);_fetchBusy=false}).catch(function(){_fetchBusy=false})}
function loadPatchAndSettings(){Promise.all([fetch("/api/patch").then(function(r){return r.json()}),fetch("/api/settings").then(function(r){return r.json()})]).then(function(a){_patch=a[0];_settings=a[1];var s=_settings;if(s&&s.led_shift!==undefined){var sl=document.getElementById("ledShiftSlider");if(sl){sl.value=s.led_shift;document.getElementById("ledShiftVal").textContent=s.led_shift}}if(s&&s.led_mode){var lm=document.getElementById("ledModeSelect");if(lm)lm.value=s.led_mode}if(s&&s.led_count2!==undefined){var l2=document.getElementById("totalLeds2");if(l2)l2.value=s.led_count2}}).catch(function(){})}
loadPatchAndSettings();setInterval(fetchInfoData,33);setInterval(loadPatchAndSettings,3000);
function saveSettings(){api("POST","/api/save").then(function(r){var d=document.getElementById("saveResult");if(r&&r.ok){d.style.color="#0a0";d.textContent="Настройки сохранены!";setTimeout(function(){d.textContent=""},3000)}else{d.style.color="#f44";d.textContent="Ошибка сохранения";setTimeout(function(){d.textContent=""},3000)}}).catch(function(){var d=document.getElementById("saveResult");d.style.color="#f44";d.textContent="Ошибка сети";setTimeout(function(){d.textContent=""},3000)})}
</script>
</body>
</html>)rawliteral";
