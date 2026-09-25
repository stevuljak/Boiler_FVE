#pragma once

// Hardware — viz ZAPOJENI.txt
// LCD Keypad: D4–D9, A0. Data Logger: D10–D13 (SD), A4/A5 (RTC).
// D10 je SD CS. Podsvícení LCD na D10 MUSÍ být odpojeno.

#define PIN_LCD_RS 8
#define PIN_LCD_EN 9
#define PIN_LCD_D4 4
#define PIN_LCD_D5 5
#define PIN_LCD_D6 6
#define PIN_LCD_D7 7
#define PIN_BUTTONS A0
#define PIN_SD_CS 10
#define PIN_DS18B20 A1

#define SERIAL_BAUD 115200

#define LCD_COLS 16
#define LCD_ROWS 2

// DS18B20: 12 bit = 0.0625 °C, konverze ~750 ms (asynchronně)
#define DS18_RESOLUTION 12
#define DS18_CONV_MS 800
#define SENSOR_PERIOD_MS 2000

// Prahy A0 (DFRobot / běžné klony 1602 keypad). Při špatné detekci doladit.
#define BTN_RIGHT_MAX 50
#define BTN_UP_MAX 250
#define BTN_DOWN_MAX 450
#define BTN_LEFT_MAX 650
#define BTN_SELECT_MAX 850

#define BTN_DEBOUNCE_MS 40
#define BTN_REPEAT_DELAY_MS 500
#define BTN_REPEAT_MS 180
#define BTN_LONG_MS 1500

#define EEPROM_MAGIC 0xB1
#define EEPROM_ADDR_MAGIC 0
#define EEPROM_ADDR_INTERVAL 1
#define EEPROM_ADDR_LOGGING 2
#define EEPROM_ADDR_LASTLOG 4

#define INTERVAL_COUNT 5

#define CSV_HEADER "cas;teplota_c"

#define TEMP_INVALID -127.0f
#define TEMP_MIN_VALID -20.0f
#define TEMP_MAX_VALID 125.0f
