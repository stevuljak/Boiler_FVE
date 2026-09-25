#include <Arduino.h>
#include <EEPROM.h>
#include <LiquidCrystal.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>
#include <avr/wdt.h>
#include <OneWire.h>
#include <TimeLib.h>
#include <DS1307RTC.h>

#include "config.h"

enum Screen : uint8_t {
  SCREEN_MAIN = 0,
  SCREEN_INTERVAL,
  SCREEN_STATUS,
  SCREEN_MINMAX,
  SCREEN_NAV_COUNT,
  SCREEN_SETTIME = SCREEN_NAV_COUNT
};

enum Btn : uint8_t {
  BTN_NONE = 0,
  BTN_RIGHT,
  BTN_UP,
  BTN_DOWN,
  BTN_LEFT,
  BTN_SELECT
};

static const uint8_t EV_SELECT_LONG = 100;

static LiquidCrystal lcd(PIN_LCD_RS, PIN_LCD_EN, PIN_LCD_D4, PIN_LCD_D5,
                         PIN_LCD_D6, PIN_LCD_D7);
static OneWire oneWire(PIN_DS18B20);

static const uint16_t kIntervals[INTERVAL_COUNT] = {10, 30, 60, 300, 900};

static Screen screen = SCREEN_MAIN;
static uint8_t intervalIdx = 2;
static bool loggingOn = true;
static bool rtcOk = false;
static bool rtcChipOk = false;
static bool sdOk = false;
static bool sensorOk = false;

static float tempC = TEMP_INVALID;
static bool convPending = false;
static uint32_t convStartMs = 0;
static uint32_t lastSensorMs = 0;
static uint32_t lastLcdMs = 0;
static uint32_t lastLogUnix = 0;
static uint32_t lastRtcMs = 0;
static uint32_t lastSdRetryMs = 0;

static float minC = TEMP_INVALID;
static float maxC = TEMP_INVALID;
static uint8_t minHour = 0, minMin = 0;
static uint8_t maxHour = 0, maxMin = 0;
static uint8_t minmaxDay = 255;

static tmElements_t nowTm;
static tmElements_t editTm;
static uint8_t timeField = 0;
static bool blinkOn = true;
static uint32_t lastBlinkMs = 0;

static Btn lastRaw = BTN_NONE;
static Btn stableBtn = BTN_NONE;
static uint32_t lastChangeMs = 0;
static uint32_t holdStartMs = 0;
static uint32_t lastRepeatMs = 0;
static bool longFired = false;
static bool btnNeedRelease = false;

static char lastLogName[13] = "";
static uint16_t logCountToday = 0;

static void lcdRow(uint8_t row, const char* text) {
  char buf[LCD_COLS + 1];
  memset(buf, ' ', LCD_COLS);
  buf[LCD_COLS] = '\0';
  size_t n = strlen(text);
  if (n > LCD_COLS) {
    n = LCD_COLS;
  }
  memcpy(buf, text, n);
  lcd.setCursor(0, row);
  lcd.print(buf);
}

static void formatTemp(char* out, size_t n, float t) {
  if (t <= TEMP_MIN_VALID + 0.1f || t > TEMP_MAX_VALID) {
    snprintf(out, n, "--.-");
    return;
  }
  int t10 = (int)(t * 10.0f + (t >= 0 ? 0.5f : -0.5f));
  if (t10 < 0) {
    snprintf(out, n, "-%d.%d", (-t10) / 10, (-t10) % 10);
  } else {
    snprintf(out, n, "%d.%d", t10 / 10, t10 % 10);
  }
}

static void formatInterval(char* out, size_t n, uint16_t sec) {
  if (sec >= 60 && sec % 60 == 0) {
    snprintf(out, n, "%u min", (unsigned)(sec / 60));
  } else {
    snprintf(out, n, "%u s", (unsigned)sec);
  }
}

static void formatIntervalShort(char* out, size_t n, uint16_t sec) {
  if (sec >= 60) {
    snprintf(out, n, "%um", (unsigned)(sec / 60));
  } else {
    snprintf(out, n, "%us", (unsigned)sec);
  }
}

static uint16_t intervalSec() { return kIntervals[intervalIdx]; }

static bool tempValid(float t) {
  return t > TEMP_MIN_VALID && t < TEMP_MAX_VALID && t > -126.0f;
}

static int monthFromAbbrev(const char* m) {
  static const char kMonths[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  for (int i = 0; i < 12; i++) {
    if (strncmp(kMonths + i * 3, m, 3) == 0) {
      return i + 1;
    }
  }
  return 1;
}

static void loadCompileTime(tmElements_t& tm) {
  const char* d = __DATE__;
  const char* t = __TIME__;
  tm.Year = CalendarYrToTm(atoi(d + 7));
  tm.Month = (uint8_t)monthFromAbbrev(d);
  tm.Day = (uint8_t)atoi(d + 4);
  tm.Hour = (uint8_t)atoi(t);
  tm.Minute = (uint8_t)atoi(t + 3);
  tm.Second = (uint8_t)atoi(t + 6);
}

static uint8_t daysInMonth(uint8_t month, int year) {
  static const uint8_t kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2) {
    bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    return leap ? 29 : 28;
  }
  if (month < 1 || month > 12) {
    return 31;
  }
  return kDays[month - 1];
}

static void clampEditTime() {
  int year = tmYearToCalendar(editTm.Year);
  if (year < 2020) {
    year = 2020;
  }
  if (year > 2099) {
    year = 2099;
  }
  editTm.Year = CalendarYrToTm(year);
  if (editTm.Month < 1) {
    editTm.Month = 12;
  }
  if (editTm.Month > 12) {
    editTm.Month = 1;
  }
  uint8_t dim = daysInMonth(editTm.Month, year);
  if (editTm.Day < 1) {
    editTm.Day = dim;
  }
  if (editTm.Day > dim) {
    editTm.Day = 1;
  }
  if (editTm.Hour > 23) {
    editTm.Hour = 0;
  }
  if (editTm.Minute > 59) {
    editTm.Minute = 0;
  }
  editTm.Second = 0;
}

static void loadSettings() {
  if (EEPROM.read(EEPROM_ADDR_MAGIC) != EEPROM_MAGIC) {
    intervalIdx = 2;
    loggingOn = true;
    lastLogUnix = 0;
    EEPROM.update(EEPROM_ADDR_INTERVAL, intervalIdx);
    EEPROM.update(EEPROM_ADDR_LOGGING, 1);
    EEPROM.put(EEPROM_ADDR_LASTLOG, (uint32_t)0);
    EEPROM.update(EEPROM_ADDR_MAGIC, EEPROM_MAGIC);
    return;
  }
  intervalIdx = EEPROM.read(EEPROM_ADDR_INTERVAL);
  if (intervalIdx >= INTERVAL_COUNT) {
    intervalIdx = 2;
  }
  loggingOn = EEPROM.read(EEPROM_ADDR_LOGGING) != 0;
  EEPROM.get(EEPROM_ADDR_LASTLOG, lastLogUnix);
  if (lastLogUnix == 0xFFFFFFFFUL) {
    lastLogUnix = 0;
  }
}

static void saveLastLog() {
  EEPROM.put(EEPROM_ADDR_LASTLOG, lastLogUnix);
}

static void saveSettings() {
  EEPROM.update(EEPROM_ADDR_INTERVAL, intervalIdx);
  EEPROM.update(EEPROM_ADDR_LOGGING, loggingOn ? 1 : 0);
}

static Btn readBtnRaw() {
  int v = analogRead(PIN_BUTTONS);
  if (v < BTN_RIGHT_MAX) {
    return BTN_RIGHT;
  }
  if (v < BTN_UP_MAX) {
    return BTN_UP;
  }
  if (v < BTN_DOWN_MAX) {
    return BTN_DOWN;
  }
  if (v < BTN_LEFT_MAX) {
    return BTN_LEFT;
  }
  if (v < BTN_SELECT_MAX) {
    return BTN_SELECT;
  }
  return BTN_NONE;
}

static uint8_t pollBtn() {
  uint32_t now = millis();
  Btn raw = readBtnRaw();

  if (btnNeedRelease) {
    if (raw == BTN_NONE) {
      if (raw != lastRaw) {
        lastRaw = raw;
        lastChangeMs = now;
      } else if (now - lastChangeMs >= BTN_DEBOUNCE_MS) {
        btnNeedRelease = false;
        stableBtn = BTN_NONE;
        longFired = false;
      }
    } else {
      lastRaw = raw;
      lastChangeMs = now;
    }
    return BTN_NONE;
  }

  if (raw != lastRaw) {
    lastRaw = raw;
    lastChangeMs = now;
    return BTN_NONE;
  }
  if (now - lastChangeMs < BTN_DEBOUNCE_MS) {
    return BTN_NONE;
  }

  if (raw != stableBtn) {
    Btn prev = stableBtn;
    bool wasLong = longFired;
    stableBtn = raw;
    holdStartMs = now;
    lastRepeatMs = now;
    longFired = false;

    if (raw == BTN_NONE && prev == BTN_SELECT && !wasLong) {
      return BTN_SELECT;
    }
    if (raw == BTN_NONE || raw == BTN_SELECT) {
      return BTN_NONE;
    }
    return raw;
  }

  if (stableBtn == BTN_SELECT && !longFired &&
      now - holdStartMs >= BTN_LONG_MS) {
    longFired = true;
    return EV_SELECT_LONG;
  }

  if ((stableBtn == BTN_UP || stableBtn == BTN_DOWN) &&
      now - holdStartMs >= BTN_REPEAT_DELAY_MS &&
      now - lastRepeatMs >= BTN_REPEAT_MS) {
    lastRepeatMs = now;
    return stableBtn;
  }

  return BTN_NONE;
}

static void applyTime(const tmElements_t& tm) {
  nowTm = tm;
  setTime(makeTime(nowTm));
  rtcOk = tmYearToCalendar(nowTm.Year) >= 2020;
}

static void tickSoftClock() {
  if (rtcOk) {
    breakTime(now(), nowTm);
  }
}

static bool readRtc() {
  tmElements_t tm;
  if (RTC.read(tm) && tmYearToCalendar(tm.Year) >= 2020) {
    applyTime(tm);
    rtcChipOk = true;
    return true;
  }
  rtcChipOk = false;
  if (Wire.getWireTimeoutFlag()) {
    Wire.clearWireTimeoutFlag();
  }
  tickSoftClock();
  return rtcOk;
}

static uint8_t dec2bcd(uint8_t v) {
  return (uint8_t)((v / 10 * 16) + (v % 10));
}

// Jeden I2C zápis, CH=0. DS1307RTC::write nejdřív hodiny zastaví (0x80);
// když TZT visí na I2C, smyčka se zasekne a tlačítka přestanou reagovat.
static bool writeRtcChip(const tmElements_t& tm) {
  uint8_t wday = tm.Wday;
  if (wday < 1 || wday > 7) {
    wday = 1;
  }
  Wire.beginTransmission(0x68);
  Wire.write((uint8_t)0x00);
  Wire.write(dec2bcd(tm.Second) & 0x7F);
  Wire.write(dec2bcd(tm.Minute));
  Wire.write(dec2bcd(tm.Hour) & 0x3F);
  Wire.write(dec2bcd(wday));
  Wire.write(dec2bcd(tm.Day));
  Wire.write(dec2bcd(tm.Month));
  Wire.write(dec2bcd((uint8_t)tmYearToY2k(tm.Year)));
  uint8_t err = Wire.endTransmission();
  if (Wire.getWireTimeoutFlag()) {
    Wire.clearWireTimeoutFlag();
    return false;
  }
  return err == 0;
}

static bool writeRtc(tmElements_t tm) {
  editTm = tm;
  clampEditTime();
  tm = editTm;
  analogRead(PIN_BUTTONS);
  applyTime(tm);
  if (writeRtcChip(tm)) {
    rtcChipOk = true;
  } else {
    rtcChipOk = false;
  }
  analogRead(PIN_BUTTONS);
  return rtcOk;
}

static void sdCsIdle() {
  // D10 = AVR SS. INPUT by přepnul SPI do slave → LCD jede, karta hlásí SD?.
  digitalWrite(PIN_SD_CS, HIGH);
  pinMode(PIN_SD_CS, OUTPUT);
}

static bool initSd() {
  sdCsIdle();
  delay(100);
  SD.end();
  // 16 GB SDHC: nejdřív pomalý SPI, Arduino SD umí jen FAT16/FAT32 (ne exFAT).
  sdOk = SD.begin(250000, PIN_SD_CS);
  if (!sdOk) {
    delay(200);
    SD.end();
    sdOk = SD.begin(PIN_SD_CS);
  }
  sdCsIdle();
  return sdOk;
}

static void makeLogName(char* out, size_t n, const tmElements_t& tm) {
  snprintf(out, n, "%04d%02u%02u.CSV", tmYearToCalendar(tm.Year), tm.Month,
           tm.Day);
}

static bool writeLogLine(const tmElements_t& tm, float temp) {
  char name[13];
  makeLogName(name, sizeof(name), tm);
  char tbuf[8];
  formatTemp(tbuf, sizeof(tbuf), temp);
  char line[32];
  snprintf(line, sizeof(line), "%04d-%02u-%02u %02u:%02u:%02u;%s",
           tmYearToCalendar(tm.Year), tm.Month, tm.Day, tm.Hour, tm.Minute,
           tm.Second, tbuf);

  for (uint8_t attempt = 0; attempt < 2; attempt++) {
    if (attempt > 0) {
      delay(80);
      if (!initSd()) {
        continue;
      }
    }
    File f = SD.open(name, FILE_WRITE);
    if (!f) {
      continue;
    }
    bool ok = true;
    if (f.size() == 0) {
      ok = f.println(F(CSV_HEADER)) > 0;
      logCountToday = 0;
    }
    if (ok) {
      ok = f.println(line) > 0;
    }
    f.flush();
    if (f.getWriteError()) {
      ok = false;
    }
    f.close();
    sdCsIdle();
    if (!ok) {
      continue;
    }
    strncpy(lastLogName, name, sizeof(lastLogName) - 1);
    lastLogName[sizeof(lastLogName) - 1] = '\0';
    logCountToday++;
    Serial.print(F("LOG;"));
    Serial.println(line);
    return true;
  }
  sdOk = false;
  return false;
}

static void sendLive() {
  if (!sensorOk) {
    return;
  }
  char tbuf[8];
  formatTemp(tbuf, sizeof(tbuf), tempC);
  if (rtcOk) {
    char line[40];
    snprintf(line, sizeof(line), "LIVE;%04d-%02u-%02u %02u:%02u:%02u;%s",
             tmYearToCalendar(nowTm.Year), nowTm.Month, nowTm.Day, nowTm.Hour,
             nowTm.Minute, nowTm.Second, tbuf);
    Serial.println(line);
  } else {
    Serial.print(F("LIVE;;"));
    Serial.println(tbuf);
  }
}

static void dumpFile(const char* name, bool quiet) {
  File f = SD.open(name);
  if (!f) {
    if (!quiet) {
      Serial.print(F("ERR;DUMP;"));
      Serial.println(name);
    }
    return;
  }
  Serial.print(F("DUMP_BEGIN;"));
  Serial.println(name);
  uint32_t left = f.size();
  char buf[48];
  uint8_t i = 0;
  while (left > 0) {
    int r = f.read();
    if (r < 0) {
      break;
    }
    left--;
    char c = (char)r;
    if (c == '\r') {
      continue;
    }
    if (c == '\n' || i >= sizeof(buf) - 1) {
      buf[i] = '\0';
      if (i > 0) {
        Serial.println(buf);
      }
      i = 0;
      if (c != '\n') {
        buf[i++] = c;
      }
    } else {
      buf[i++] = c;
    }
  }
  if (i > 0) {
    buf[i] = '\0';
    Serial.println(buf);
  }
  f.close();
  sdCsIdle();
  Serial.println(F("DUMP_END"));
}

static void dumpAll() {
  Serial.println(F("DUMP_ALL_BEGIN"));
  tmElements_t tm = nowTm;
  if (!rtcOk) {
    if (lastLogName[0]) {
      dumpFile(lastLogName, true);
    }
    Serial.println(F("DUMP_ALL_END"));
    return;
  }
  for (uint8_t i = 0; i < 31; i++) {
    char name[13];
    makeLogName(name, sizeof(name), tm);
    dumpFile(name, true);
    time_t u = makeTime(tm);
    if (u < 86400L) {
      break;
    }
    breakTime(u - 86400L, tm);
  }
  Serial.println(F("DUMP_ALL_END"));
}

static void handleDump(const char* arg) {
  if (!sdOk && !initSd()) {
    Serial.println(F("ERR;SD"));
    return;
  }
  while (*arg == ' ') {
    arg++;
  }
  if (arg[0] == '\0') {
    char name[13];
    if (rtcOk) {
      makeLogName(name, sizeof(name), nowTm);
    } else if (lastLogName[0]) {
      strncpy(name, lastLogName, 12);
      name[12] = '\0';
    } else {
      Serial.println(F("ERR;DUMP"));
      return;
    }
    dumpFile(name, false);
    return;
  }
  if (strcmp(arg, "ALL") == 0) {
    dumpAll();
    return;
  }
  char name[13];
  if (strlen(arg) == 8) {
    snprintf(name, sizeof(name), "%s.CSV", arg);
  } else {
    strncpy(name, arg, 12);
    name[12] = '\0';
  }
  dumpFile(name, false);
}

static void sendStatus() {
  char tbuf[8];
  formatTemp(tbuf, sizeof(tbuf), tempC);
  Serial.print(F("STATUS;RTC="));
  Serial.print(rtcOk ? '1' : '0');
  Serial.print(F(";SD="));
  Serial.print(sdOk ? '1' : '0');
  Serial.print(F(";DS18="));
  Serial.print(sensorOk ? '1' : '0');
  Serial.print(F(";LOG="));
  Serial.print(loggingOn ? '1' : '0');
  Serial.print(F(";T="));
  Serial.println(tbuf);
}

static void updateMinMax(float t, const tmElements_t& tm) {
  if (!tempValid(t)) {
    return;
  }
  if (minmaxDay != tm.Day) {
    minC = maxC = t;
    minHour = maxHour = tm.Hour;
    minMin = maxMin = tm.Minute;
    minmaxDay = tm.Day;
    return;
  }
  if (!tempValid(minC) || t < minC) {
    minC = t;
    minHour = tm.Hour;
    minMin = tm.Minute;
  }
  if (!tempValid(maxC) || t > maxC) {
    maxC = t;
    maxHour = tm.Hour;
    maxMin = tm.Minute;
  }
}

static void startConversion() {
  if (!oneWire.reset()) {
    sensorOk = false;
    return;
  }
  oneWire.skip();
  oneWire.write(0x44);
  convStartMs = millis();
  convPending = true;
}

static void pollSensor() {
  uint32_t now = millis();
  if (!convPending && now - lastSensorMs >= SENSOR_PERIOD_MS) {
    lastSensorMs = now;
    startConversion();
  }
  if (convPending && now - convStartMs >= DS18_CONV_MS) {
    convPending = false;
    float t = TEMP_INVALID;
    if (oneWire.reset()) {
      oneWire.skip();
      oneWire.write(0xBE);
      uint8_t lsb = oneWire.read();
      uint8_t msb = oneWire.read();
      int16_t raw = (int16_t)(((uint16_t)msb << 8) | lsb);
      t = (float)raw / 16.0f;
    }
    sensorOk = tempValid(t);
    tempC = sensorOk ? t : TEMP_INVALID;
    if (sensorOk && rtcOk) {
      updateMinMax(tempC, nowTm);
    }
    sendLive();
  }
}

static void tryLog() {
  if (!loggingOn || !rtcOk || !sdOk || !sensorOk) {
    return;
  }
  if (millis() < 4000) {
    return;
  }
  time_t unix = makeTime(nowTm);
  uint16_t iv = intervalSec();
  time_t aligned = unix - (unix % (time_t)iv);
  if (lastLogUnix != 0 && aligned <= (time_t)lastLogUnix) {
    return;
  }
  if (writeLogLine(nowTm, tempC)) {
    lastLogUnix = (uint32_t)aligned;
    saveLastLog();
  }
}

static void handleSerial() {
  static char buf[36];
  static uint8_t len = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      buf[len] = '\0';
      len = 0;
      if (strncmp(buf, "SET ", 4) == 0 && strlen(buf) >= 23) {
        const char* p = buf + 4;
        tmElements_t tm;
        tm.Year = CalendarYrToTm(atoi(p));
        tm.Month = (uint8_t)atoi(p + 5);
        tm.Day = (uint8_t)atoi(p + 8);
        tm.Hour = (uint8_t)atoi(p + 11);
        tm.Minute = (uint8_t)atoi(p + 14);
        tm.Second = (uint8_t)atoi(p + 17);
        editTm = tm;
        if (writeRtc(tm)) {
          Serial.println(F("RTC OK"));
        } else {
          Serial.println(F("RTC ERR"));
        }
      } else if (strcmp(buf, "STATUS") == 0) {
        sendStatus();
      } else if (strcmp(buf, "DUMP") == 0) {
        handleDump("");
      } else if (strncmp(buf, "DUMP ", 5) == 0) {
        handleDump(buf + 5);
      }
      continue;
    }
    if (len < sizeof(buf) - 1) {
      buf[len++] = c;
    }
  }
}

static void drawMain() {
  char line[17];
  if (rtcOk) {
    snprintf(line, sizeof(line), "%02u.%02u.%02u  %02u:%02u", nowTm.Day,
             nowTm.Month, (unsigned)tmYearToY2k(nowTm.Year), nowTm.Hour,
             nowTm.Minute);
  } else {
    snprintf(line, sizeof(line), "RTC neni cas");
  }
  lcdRow(0, line);

  char tbuf[8];
  formatTemp(tbuf, sizeof(tbuf), tempC);
  const char* st;
  if (!sensorOk) {
    st = "CIDLO";
  } else if (!sdOk && loggingOn) {
    st = "SD?";
  } else if (loggingOn) {
    st = "LOG";
  } else {
    st = "PAU";
  }
  char iv[6];
  formatIntervalShort(iv, sizeof(iv), intervalSec());
  snprintf(line, sizeof(line), "%sC %s %s", tbuf, st, iv);
  lcdRow(1, line);
}

static void drawInterval() {
  lcdRow(0, "Interval zapisu");
  char iv[12];
  formatInterval(iv, sizeof(iv), intervalSec());
  char line[17];
  snprintf(line, sizeof(line), "<    %s    >", iv);
  lcdRow(1, line);
}

static void drawStatus() {
  char line[17];
  snprintf(line, sizeof(line), "RTC:%s SD:%s",
           !rtcOk ? "ERR" : (rtcChipOk ? "OK" : "SW"),
           sdOk ? "OK" : "ERR");
  lcdRow(0, line);
  if (lastLogName[0]) {
    snprintf(line, sizeof(line), "%s n=%u", lastLogName, (unsigned)logCountToday);
  } else {
    snprintf(line, sizeof(line), "DS18:%s", sensorOk ? "OK" : "ERR");
  }
  lcdRow(1, line);
}

static void drawMinMax() {
  char tbuf[8];
  char line[17];
  if (tempValid(minC)) {
    formatTemp(tbuf, sizeof(tbuf), minC);
    snprintf(line, sizeof(line), "Min %s  %02u:%02u", tbuf, minHour, minMin);
  } else {
    snprintf(line, sizeof(line), "Min --.-  --:--");
  }
  lcdRow(0, line);
  if (tempValid(maxC)) {
    formatTemp(tbuf, sizeof(tbuf), maxC);
    snprintf(line, sizeof(line), "Max %s  %02u:%02u", tbuf, maxHour, maxMin);
  } else {
    snprintf(line, sizeof(line), "Max --.-  --:--");
  }
  lcdRow(1, line);
}

static void drawSetTime() {
  lcdRow(0, "Nastavit cas");
  char line[17];
  snprintf(line, sizeof(line), "%02u.%02u.%02u  %02u:%02u", editTm.Day,
           editTm.Month, (unsigned)tmYearToY2k(editTm.Year), editTm.Hour,
           editTm.Minute);
  if (!blinkOn) {
    static const uint8_t kPos[5] = {0, 3, 6, 10, 13};
    uint8_t p = kPos[timeField];
    line[p] = ' ';
    line[p + 1] = ' ';
  }
  lcdRow(1, line);
}

static void drawScreen() {
  switch (screen) {
    case SCREEN_INTERVAL:
      drawInterval();
      break;
    case SCREEN_STATUS:
      drawStatus();
      break;
    case SCREEN_MINMAX:
      drawMinMax();
      break;
    case SCREEN_SETTIME:
      drawSetTime();
      break;
    default:
      drawMain();
      break;
  }
}

static void bumpTimeField(int dir) {
  int year = tmYearToCalendar(editTm.Year);
  switch (timeField) {
    case 0:
      editTm.Day = (uint8_t)((int)editTm.Day + dir);
      break;
    case 1:
      editTm.Month = (uint8_t)((int)editTm.Month + dir);
      break;
    case 2:
      year += dir;
      editTm.Year = CalendarYrToTm(year);
      break;
    case 3:
      editTm.Hour = (uint8_t)((int)editTm.Hour + dir);
      break;
    case 4:
      editTm.Minute = (uint8_t)((int)editTm.Minute + dir);
      break;
    default:
      break;
  }
  if ((int)editTm.Hour < 0 || editTm.Hour > 23) {
    editTm.Hour = dir > 0 ? 0 : 23;
  }
  if ((int)editTm.Minute < 0 || editTm.Minute > 59) {
    editTm.Minute = dir > 0 ? 0 : 59;
  }
  clampEditTime();
}

static void handleBtn(uint8_t ev) {
  if (ev == 0) {
    return;
  }

  if (screen == SCREEN_SETTIME) {
    if (ev == BTN_LEFT) {
      if (timeField == 0) {
        screen = SCREEN_MAIN;
      } else {
        timeField--;
      }
    } else if (ev == BTN_RIGHT) {
      if (timeField < 4) {
        timeField++;
      }
    } else if (ev == BTN_UP) {
      bumpTimeField(1);
    } else if (ev == BTN_DOWN) {
      bumpTimeField(-1);
    } else if (ev == BTN_SELECT) {
      editTm.Second = 0;
      btnNeedRelease = true;
      if (writeRtc(editTm)) {
        lcdRow(0, "Cas ulozen");
        lcdRow(1, rtcChipOk ? "" : "bez RTC cipu");
        screen = SCREEN_MAIN;
        lastLogUnix = 0;
        saveLastLog();
        Serial.println(F("RTC ulozen"));
        delay(400);
      } else {
        lcdRow(1, "RTC chyba");
        Serial.println(F("RTC ERR"));
        delay(400);
      }
    }
    drawScreen();
    return;
  }

  if (ev == EV_SELECT_LONG) {
    btnNeedRelease = true;
    editTm = rtcOk ? nowTm : editTm;
    if (!rtcOk) {
      loadCompileTime(editTm);
    }
    timeField = 0;
    screen = SCREEN_SETTIME;
    drawScreen();
    return;
  }

  if (ev == BTN_LEFT) {
    screen = (Screen)((screen + SCREEN_NAV_COUNT - 1) % SCREEN_NAV_COUNT);
    drawScreen();
    return;
  }
  if (ev == BTN_RIGHT) {
    screen = (Screen)((screen + 1) % SCREEN_NAV_COUNT);
    drawScreen();
    return;
  }

  if (screen == SCREEN_INTERVAL && (ev == BTN_UP || ev == BTN_DOWN)) {
    if (ev == BTN_UP) {
      intervalIdx = (intervalIdx + 1) % INTERVAL_COUNT;
    } else {
      intervalIdx = (uint8_t)(intervalIdx + INTERVAL_COUNT - 1) % INTERVAL_COUNT;
    }
    saveSettings();
    lastLogUnix = 0;
    saveLastLog();
    drawScreen();
    return;
  }

  if (ev == BTN_SELECT) {
    if (screen == SCREEN_MAIN) {
      loggingOn = !loggingOn;
      saveSettings();
    } else if (screen == SCREEN_MINMAX) {
      minC = maxC = tempC;
      if (rtcOk) {
        minHour = maxHour = nowTm.Hour;
        minMin = maxMin = nowTm.Minute;
        minmaxDay = nowTm.Day;
      }
    }
    drawScreen();
  }
}

void setup() {
  wdt_disable();
  sdCsIdle();
  Serial.begin(SERIAL_BAUD);
  Serial.println(F("Boiler_FVE"));

  lcd.begin(LCD_COLS, LCD_ROWS);
  lcdRow(0, "Boiler FVE");
  lcdRow(1, "startuji...");

  loadSettings();
  loadCompileTime(editTm);

  Wire.begin();
  Wire.setWireTimeout(25000, true);
  if (readRtc()) {
    Serial.println(rtcChipOk ? F("RTC OK") : F("RTC SW"));
  } else {
    Serial.println(F("RTC neni nastaven"));
  }

  lcdRow(1, "SD karta...");
  delay(500);
  if (initSd()) {
    Serial.println(F("SD OK"));
  } else {
    Serial.println(F("SD ERR (FAT32, ne exFAT)"));
  }
  sdCsIdle();

  if (oneWire.reset()) {
    Serial.println(F("DS18B20 OK"));
    startConversion();
  } else {
    Serial.println(F("DS18B20 ERR"));
    sensorOk = false;
  }

  if (!rtcOk) {
    screen = SCREEN_SETTIME;
    timeField = 0;
  }

  delay(400);
  drawScreen();
  Serial.println(F("READY;Boiler_FVE"));
}

void loop() {
  handleSerial();
  handleBtn(pollBtn());
  pollSensor();

  uint32_t now = millis();

  if (now - lastRtcMs >= 1000) {
    lastRtcMs = now;
    readRtc();
    tryLog();
  }

  if (!sdOk && now - lastSdRetryMs >= 15000) {
    lastSdRetryMs = now;
    initSd();
  }

  if (screen == SCREEN_SETTIME && now - lastBlinkMs >= 400) {
    lastBlinkMs = now;
    blinkOn = !blinkOn;
    drawSetTime();
  } else if (screen != SCREEN_SETTIME && now - lastLcdMs >= 500) {
    lastLcdMs = now;
    drawScreen();
  }
}
