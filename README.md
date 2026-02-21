# shrikeOS Monitor

A dashboard system monitor for the [shrike-lite board](https://vicharak-in.github.io/shrike/introduction.html) (RP2040 + SLG47910 FPGA) built on Zephyr RTOS. Working with Zephyr for the first time so didn't took that many risks.

### Architecture

```mermaid
graph LR
    subgraph "Shrike-lite Board"
        A[RP2040 MCU] --> B["SSD1306 OLED (I2C1: GP6/GP7)"]
        A --> C["LED (GPIO 4)"]
        A --> D["ADC Ch4 (Internal Temp)"]
        A --> E["USB CDC ACM (JSON Serial)"]
    end

    subgraph "Computer"
        E <--> F["bridge.py\n(Serial ↔ WebSocket)"]
        F <--> G["Browser Dashboard\n(Retro CRT UI)"]
    end
```

## File Structure

```
~/zephyrproject/shrike_monitor/
├── CMakeLists.txt                 # zephyr build config
├── prj.conf                      # Kconfig
├── boards/
│   └── rpi_pico.overlay           # overall layout for the pins
├── src/
│   └── main.c                    # 4 threads
└── dashboard/
    ├── index.html                 # dashboard layout
    ├── style.css                  # css
    ├── app.js                     # websocket client
    └── bridge.py                  # serial to websocket bridge
```


### I2C Display Layout

```
┌────────────────────────┐
│      SHRIKE            │ ← yellow zone (dual-color atleast the one I used)
├────────────────────────┤
│ LED: ON                │ ← LED state
│                        │
│ > Ready                │ ← custom message area
│                        │   (shows typed msg from dashboard)
└────────────────────────┘
```

### Web-based Monitor

<img width="1207" height="891" alt="image" src="https://github.com/user-attachments/assets/5ca3d16e-1b73-47ea-9065-f87fd7d4629c" />


