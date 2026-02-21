/* ============================================================
   ShrikeOS Monitor — Dashboard Logic
   Connects via WebSocket (bridge.py) or WebSerial (Chrome)
   ============================================================ */

// --- Config ---
const WS_URL = 'ws://localhost:8765';

// --- State ---
let ws = null;
let port = null;
let reader = null;
let writer = null;
let readBuffer = '';
let connected = false;
let connectionMode = '';

// --- DOM Elements ---
const connectBtn = document.getElementById('connect-btn');
const connStatus = document.getElementById('connection-status');
const tempValue = document.getElementById('temp-value');
const tempGauge = document.getElementById('temp-gauge');
const upH = document.getElementById('up-h');
const upM = document.getElementById('up-m');
const upS = document.getElementById('up-s');
const threadCount = document.getElementById('thread-count');
const memBar = document.getElementById('mem-bar');
const memValue = document.getElementById('mem-value');
const ledDot = document.getElementById('led-dot');
const ledState = document.getElementById('led-state');
const blinkRate = document.getElementById('blink-rate');
const ledToggle = document.getElementById('led-toggle');
const blinkSlider = document.getElementById('blink-slider');
const blinkSliderVal = document.getElementById('blink-slider-val');
const oledMsgInput = document.getElementById('oled-msg-input');
const oledMsgSend = document.getElementById('oled-msg-send');
const terminal = document.getElementById('terminal');
const clearTermBtn = document.getElementById('clear-terminal');
const clockEl = document.getElementById('clock');

let ledIsOn = true;

// --- Clock ---
function updateClock() {
    const now = new Date();
    clockEl.textContent = now.toLocaleTimeString('en-US', { hour12: false });
}
setInterval(updateClock, 1000);
updateClock();

// --- Temperature Gauge (Canvas) ---
const ctx = tempGauge.getContext('2d');
const gaugeW = tempGauge.width;
const gaugeH = tempGauge.height;
const cx = gaugeW / 2;
const cy = gaugeH / 2;
const gaugeR = 80;

function drawGauge(temp) {
    ctx.clearRect(0, 0, gaugeW, gaugeH);

    ctx.beginPath();
    ctx.arc(cx, cy, gaugeR, Math.PI * 0.75, Math.PI * 2.25, false);
    ctx.strokeStyle = '#2a2200';
    ctx.lineWidth = 10;
    ctx.lineCap = 'round';
    ctx.stroke();

    const clampedTemp = Math.max(0, Math.min(80, temp));
    const angle = (clampedTemp / 80) * (Math.PI * 1.5);
    const startAngle = Math.PI * 0.75;

    if (temp > 0) {
        ctx.beginPath();
        ctx.arc(cx, cy, gaugeR, startAngle, startAngle + angle, false);
        if (temp > 60) ctx.strokeStyle = '#ff3333';
        else if (temp > 40) ctx.strokeStyle = '#ffb347';
        else ctx.strokeStyle = '#ff8c00';
        ctx.lineWidth = 10;
        ctx.lineCap = 'round';
        ctx.shadowBlur = 15;
        ctx.shadowColor = ctx.strokeStyle;
        ctx.stroke();
        ctx.shadowBlur = 0;
    }

    for (let i = 0; i <= 8; i++) {
        const tickAngle = startAngle + (i / 8) * Math.PI * 1.5;
        const inner = gaugeR - 18;
        const outer = gaugeR - 12;
        ctx.beginPath();
        ctx.moveTo(cx + Math.cos(tickAngle) * inner, cy + Math.sin(tickAngle) * inner);
        ctx.lineTo(cx + Math.cos(tickAngle) * outer, cy + Math.sin(tickAngle) * outer);
        ctx.strokeStyle = '#5a3100';
        ctx.lineWidth = 2;
        ctx.stroke();

        if (i % 4 === 0) {
            const lx = cx + Math.cos(tickAngle) * (inner - 12);
            const ly = cy + Math.sin(tickAngle) * (inner - 12);
            ctx.fillStyle = '#5a3100';
            ctx.font = '12px VT323, monospace';
            ctx.textAlign = 'center';
            ctx.textBaseline = 'middle';
            ctx.fillText((i * 10) + '°', lx, ly);
        }
    }

    const needleAngle = startAngle + (clampedTemp / 80) * Math.PI * 1.5;
    const needleLen = gaugeR - 25;
    ctx.beginPath();
    ctx.moveTo(cx, cy);
    ctx.lineTo(cx + Math.cos(needleAngle) * needleLen, cy + Math.sin(needleAngle) * needleLen);
    ctx.strokeStyle = '#ffb347';
    ctx.lineWidth = 2;
    ctx.shadowBlur = 6;
    ctx.shadowColor = '#ff8c00';
    ctx.stroke();
    ctx.shadowBlur = 0;

    ctx.beginPath();
    ctx.arc(cx, cy, 4, 0, Math.PI * 2);
    ctx.fillStyle = '#ff8c00';
    ctx.fill();
}

drawGauge(0);

// --- Terminal ---
function termLog(text, cls = '') {
    const line = document.createElement('div');
    line.className = 'terminal-line ' + cls;
    line.textContent = text;
    terminal.appendChild(line);
    while (terminal.children.length > 200) {
        terminal.removeChild(terminal.firstChild);
    }
    terminal.scrollTop = terminal.scrollHeight;
}

clearTermBtn.addEventListener('click', () => {
    terminal.innerHTML = '';
    termLog('[ Terminal cleared ]', 'system');
});

// === CONNECTION ===

connectBtn.addEventListener('click', async () => {
    if (connected) {
        await disconnect();
    } else {
        stopDemo();
        await connectWebSocket();
    }
});

// --- WebSocket (via bridge.py) ---
async function connectWebSocket() {
    try {
        termLog('[ Connecting to ' + WS_URL + '... ]', 'system');
        ws = new WebSocket(WS_URL);

        ws.onopen = () => {
            connected = true;
            connectionMode = 'ws';
            connStatus.textContent = '◉ CONNECTED';
            connStatus.className = 'status-badge connected';
            connectBtn.textContent = '⏏ DISCONNECT';
            termLog('[ Connected to Shrike-lite! ]', 'system');
        };

        ws.onmessage = (event) => {
            const line = event.data.trim();
            if (line) {
                termLog(line);
                try {
                    const data = JSON.parse(line);
                    updateDashboard(data);
                } catch (e) { /* not JSON */ }
            }
        };

        ws.onerror = () => {
            termLog('[ Bridge not running! Start: python3 bridge.py ]', 'error');
            ws = null;
        };

        ws.onclose = () => {
            if (connected) {
                connected = false;
                connStatus.textContent = '◉ DISCONNECTED';
                connStatus.className = 'status-badge disconnected';
                connectBtn.textContent = '⚡ CONNECT';
                termLog('[ Disconnected ]', 'system');
            }
        };
    } catch (err) {
        termLog('Error: ' + err.message, 'error');
    }
}

async function disconnect() {
    connected = false;
    connectionMode = '';
    if (ws) { ws.close(); ws = null; }
    try {
        if (reader) { await reader.cancel(); reader = null; }
        if (writer) { await writer.close(); writer = null; }
        if (port) { await port.close(); port = null; }
    } catch (e) { /* ignore */ }
    connStatus.textContent = '◉ DISCONNECTED';
    connStatus.className = 'status-badge disconnected';
    connectBtn.textContent = '⚡ CONNECT';
    termLog('[ Disconnected ]', 'system');
}

// --- Update Dashboard ---
function updateDashboard(data) {
    if (data.temp !== undefined) {
        tempValue.textContent = data.temp.toFixed(1);
        drawGauge(data.temp);
    }

    if (data.up !== undefined) {
        const h = Math.floor(data.up / 3600);
        const m = Math.floor((data.up % 3600) / 60);
        const s = data.up % 60;
        upH.textContent = String(h).padStart(2, '0');
        upM.textContent = String(m).padStart(2, '0');
        upS.textContent = String(s).padStart(2, '0');
    }

    if (data.thds !== undefined) {
        threadCount.textContent = data.thds;
    }

    if (data.led !== undefined) {
        ledIsOn = !!data.led;
        ledDot.className = 'led-dot ' + (ledIsOn ? 'on' : 'off');
        ledState.textContent = ledIsOn ? 'ON' : 'OFF';
        ledToggle.textContent = ledIsOn ? '● ON' : '○ OFF';
        ledToggle.className = 'btn btn-toggle ' + (ledIsOn ? 'active' : 'inactive');
    }

    if (data.blink !== undefined) {
        blinkRate.textContent = data.blink + ' ms';
        blinkSlider.value = data.blink;
        blinkSliderVal.textContent = data.blink + 'ms';
    }
}

// --- Send Command ---
async function sendCommand(cmd, val) {
    if (!connected) return;

    const payload = JSON.stringify({ cmd, val });
    try {
        if (connectionMode === 'ws' && ws && ws.readyState === WebSocket.OPEN) {
            ws.send(payload);
            termLog('TX: ' + payload, 'tx');
        } else if (connectionMode === 'serial' && writer) {
            await writer.write(payload + '\n');
            termLog('TX: ' + payload, 'tx');
        }
    } catch (err) {
        termLog('Send failed: ' + err.message, 'error');
    }
}

// --- Controls ---

ledToggle.addEventListener('click', () => {
    ledIsOn = !ledIsOn;
    sendCommand('led', ledIsOn ? 1 : 0);
    ledDot.className = 'led-dot ' + (ledIsOn ? 'on' : 'off');
    ledState.textContent = ledIsOn ? 'ON' : 'OFF';
    ledToggle.textContent = ledIsOn ? '● ON' : '○ OFF';
    ledToggle.className = 'btn btn-toggle ' + (ledIsOn ? 'active' : 'inactive');
});

blinkSlider.addEventListener('input', () => {
    blinkSliderVal.textContent = blinkSlider.value + 'ms';
});
blinkSlider.addEventListener('change', () => {
    sendCommand('blink', parseInt(blinkSlider.value));
});

oledMsgSend.addEventListener('click', () => {
    const msg = oledMsgInput.value.trim();
    if (msg) {
        sendCommand('oled_msg', msg);
        oledMsgInput.value = '';
    }
});
oledMsgInput.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') oledMsgSend.click();
});

// --- Demo Mode ---
let demoInterval = null;

function startDemo() {
    let tick = 0;
    demoInterval = setInterval(() => {
        tick++;
        updateDashboard({
            temp: 28 + Math.sin(tick * 0.1) * 5 + Math.random() * 2,
            up: tick,
            thds: 4,
            led: Math.floor(tick / 2) % 2,
            blink: 250
        });
    }, 1000);
}

function stopDemo() {
    if (demoInterval) {
        clearInterval(demoInterval);
        demoInterval = null;
    }
}

termLog('[ ShrikeOS Monitor v1.0 ]', 'system');
termLog('[ Demo mode — click CONNECT to link to Shrike-lite ]', 'system');
startDemo();
