# STM32F407VGT6 Data Logger — Hardware Configuration Documentation

**Board:** Devebox STM32F407VGT6 (LQFP100)
**Toolchain:** STM32CubeMX → STM32CubeIDE (HAL drivers)
**Last updated:** 2026-09-16

---

## 1. System Overview

Industrial data logger built around the Devebox STM32F407VGT6 core board, with dual
wired Ethernet (redundant uplinks), WiFi/MQTT via ESP32 coprocessor, isolated RS485,
external 16-bit ADC, RTC, and dual-tier storage (onboard SD + onboard SPI flash).

| Subsystem | Component |
|---|---|
| Core MCU | STM32F407VGT6 |
| WiFi / MQTT coprocessor | ESP32-WROOM-32E |
| Power | TI LM2596S-5.0 |
| Ethernet (×2, redundant) | WIZnet W5500 |
| RS485 | Isolated, ADuM5402 |
| ADC | Texas Instruments ADS1115 ×2 |
| Analog isolation | B0505S-1W (power) + ADuM1250 (data) |
| 0–10V inputs (×2) | Precision resistor network |
| 4–20mA inputs (×2) | Precision current shunt |
| RTC | Maxim DS3231M |
| Storage Tier 1 | Onboard MicroSD (SDIO) |
| Storage Tier 2 | Onboard W25Q16 SPI flash (2MB) |
| EEPROM | Microchip AT24C256 |
| Display | Nextion / DWIN (UART) |
| Indicators | LEDs + magnetic buzzer |

**Scope changes made during design:**
- SDI-12 interface dropped (not required for this deployment).
- External W25Q128JV flash dropped — using the Devebox's onboard W25Q16 instead. Verify capacity is sufficient for your log retention math before final commit.

---

## 2. Peripheral → Pin Assignment

| Function | Peripheral | Pins | Notes |
|---|---|---|---|
| W5500 #1 (Ethernet) | SPI1 | SCK=PA5, MISO=PA6, MOSI=PA7, CS=PA4, INT=PC0, RST=PC1 | Bus shared with onboard flash (different CS) |
| W5500 #2 (Ethernet) | SPI3 | SCK=PB3, MISO=PB4, MOSI=PB5, CS=PD4, INT=PD5, RST=PD6 | Moved off PA0/PA15 — see §3 conflicts |
| Onboard W25Q16 flash | SPI1 (shared bus) | CS=PA15 | Onboard, wired at factory |
| MicroSD | SDIO (4-bit) | D0=PC8, D1=PC9, D2=PC10, D3=PC11, CK=PC12, CMD=PD2 | Onboard socket, no hardware card-detect line |
| I2C bus (shared) | I2C1 | SCL=PB6, SDA=PB7 | ADS1115 ×2 (distinct ADDR pins), DS3231M, AT24C256 |
| ESP32 link | USART1 | TX=PA9, RX=PA10 | |
| RS485 | USART2 | TX=PA2, RX=PA3, DE/RE=PA8 (GPIO) | DE pin **required** — half-duplex direction control |
| Display (Nextion/DWIN) | USART3 | TX=PB10, RX=PB11 | Default baud 9600, adjust as needed |
| ADS1115 #1 ALERT/RDY | EXTI0 | PE0 | |
| ADS1115 #2 ALERT/RDY | EXTI1 | PE1 | |
| LEDs | GPIO_Output | PE2, PE3, PE4 (confirm count matches actual hardware) | |
| Buzzer | GPIO_Output | PE5/PE6 (confirm which; PWM optional via TIM if tonal control wanted) | |

---

## 3. Board-Level Conflicts Found (from Devebox schematic)

The Devebox core board has onboard peripherals hardwired — not a bare chip. Two
planned pin assignments collided with existing traces:

| Pin | Hardwired to | Original plan | Resolution |
|---|---|---|---|
| PA0 | K1 user button (WK_UP), pull-up R14 | W5500 #2 INT | Moved to PD5 |
| PA15 | Onboard W25Q16 flash CS (F_CS), shared SPI1 bus | W5500 #2 CS | Moved to PD4; PA15 now correctly used for onboard flash CS |

**Constraint noted:** PA0 (button) and PE0 (ADS1115 #1 alert) both sit on shared
EXTI line 0. Not currently a conflict (PA0 isn't configured as EXTI), but flag this
if button-wake logic is added later — only one EXTI0 source can be active.

---

## 4. System Core Configuration

| Setting | Value | Why |
|---|---|---|
| RCC — HSE | Crystal/Ceramic Resonator, **8 MHz** | Matches Devebox Y2 crystal (confirmed via schematic, not 25MHz) |
| SYS — Debug | **Serial Wire (SWD)** | Required — frees PB3/PB4 from JTAG-reserved function for SPI3 use |
| RTC (internal) | **Disabled** | External DS3231M used instead — more accurate, battery-backed |
| Analog (ADC1/2/3) | **Disabled** | All analog acquisition handled externally via ADS1115 over I2C |
| Timers | Default / IWDG optional | IWDG recommended for field reliability (auto-reset on firmware hang); PWM only needed if buzzer requires tonal patterns |
| Connectivity (unused) | CAN1/2, FSMC, USB_OTG, I2C2/3 — disabled | Not required by BOM |
| Multimedia, Security, Computing | Disabled | Not required by BOM (CRC optional if hardware-accelerated integrity checks wanted later) |

---

## 5. Clock Tree

- Input (HSE): 8 MHz
- System Clock Mux: **PLLCLK**
- Target HCLK: **168 MHz** (CubeMX auto-solves PLL M/N/P/Q and AHB/APB prescalers)
- APB1: auto-capped ≤ 42 MHz
- APB2: auto-capped ≤ 84 MHz
- 48 MHz output: needed for SDIO high-speed / USB if ever enabled

**Verify after configuring:** no red-outlined boxes in Clock Configuration tab (indicates a bus-speed violation).

---

## 6. Networking Design Note

Two W5500 modules provide **two independent wired uplinks**, not a WAN/LAN router
split — the logger does not perform NAT/routing between them. Interface selection
(which uplink sends what) is handled at the application layer. ESP32 handles
WiFi/MQTT as a third, independent uplink path, communicating with the STM32 over
USART1.

Native STM32 Ethernet (RMII + external PHY) was **not used** — avoids depending on
an unconfirmed PHY footprint on this board, avoids running two different network
stacks (lwIP + W5500 driver) simultaneously, and W5500's throughput is more than
sufficient for sensor logging traffic.

---

## 7. Project Manager Settings

| Setting | Value |
|---|---|
| Toolchain | STM32CubeIDE |
| Code Generator | "Generate peripheral initialization as pair of .c/.h files per peripheral" — enabled |
| HAL/LL | HAL (default) |
| Linker (Heap/Stack) | Defaults initially (0x200/0x400) — revisit once TCP/IP + FatFs + display buffer RAM usage is measured |

---

## 8. Editing the Config After Generation

1. Open the `.ioc` file (double-click in CubeIDE Project Explorer) to reopen the CubeMX GUI.
2. Make changes, click **Generate Code** again.
3. **Only code inside `/* USER CODE BEGIN */ ... /* USER CODE END */` blocks survives regeneration.** Never write application logic outside these markers.
4. Keep the `.ioc` under version control; diff generated files after each regeneration to catch unexpected changes.

---

## 9. Open Items / TODO

- [ ] Confirm actual LED/buzzer pin count and mapping matches hardware (currently PE2–PE6 reserved, verify usage)
- [ ] Confirm onboard W25Q16 (2MB) capacity is sufficient for log retention target — compute bytes/record × records/day × retention days
- [ ] Set ADS1115 ADDR pins to distinct addresses (e.g. 0x48 / 0x49) before wiring
- [ ] Decide whether IWDG (watchdog) is enabled for production build
- [ ] Verify SDIO has no hardware card-detect — confirm firmware handles SD presence via init/mount return code instead of a GPIO
