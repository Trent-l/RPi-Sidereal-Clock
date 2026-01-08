#include <Adafruit_GPS.h>
#include <Adafruit_LiquidCrystal.h>
#include <Adafruit_seesaw.h>
#include <Wire.h>
#include <EEPROM.h>

// =======================================================
//                    Hardware Objects
// =======================================================

// GPS module over I2C (PA1010D or similar)
Adafruit_GPS GPS(&Wire);

// 16x2 I2C LCD
Adafruit_LiquidCrystal lcd(0);

// Seesaw-based rotary encoder + button
Adafruit_seesaw ss;

// =======================================================
//                    Encoder Config
// =======================================================

// I2C address for seesaw encoder
const uint8_t SEE_SAW_ADDR = 0x36;

// Set true if encoder is detected at boot
bool encoder_present = false;

// =======================================================
//            User-Configurable Settings (Menu)
// =======================================================

// Time zone offset from UTC (hours)
int TZ_OFFSET;

// Manual LST correction in seconds
int LST_ADJUST;

// 0 = 24-hour format, 1 = 12-hour format
int TIME_FORMAT;

// Invert encoder direction if desired
const bool INVERT_ENCODER = true;

// Sidereal rate relative to solar time
const double SIDEREAL_RATE = 1.00273790935;

// =======================================================
//                  EEPROM Layout
// =======================================================

const int EEPROM_ADDR_TZ  = 0;
const int EEPROM_ADDR_LST = 4;
const int EEPROM_ADDR_FMT = 8;
const int EEPROM_SIZE = 64;

// =======================================================
//                Runtime State Variables
// =======================================================

// Last millisecond timestamp
uint32_t tmark = 0;

// Track GPS second changes
int last_gps_second = -1;

// Track last displayed LST second
long last_lst_total_seconds = LONG_MIN;

// LST value at last GPS sync (hours)
double lst_hours_at_sync = 0.0;

// Timestamp (seconds) of last GPS sync
double last_sync_time_s = 0.0;

// Cached display strings
String currentTimeStr = "";
String currentLSTStr  = "";

// Last written LCD lines (used to prevent redraw flicker)
String lastTopStr = "";
String lastBotStr = "";

// =======================================================
//                     Menu State
// =======================================================

bool inMenu = false;
bool editing = false;
int menuIndex = 0;

// Number of menu entries
const int menuCount = 4;

// Encoder tracking
int lastEncoderPos = 0;

// Button debounce tracking
bool lastBtnState = LOW;
unsigned long lastButtonPress = 0;

// =======================================================
//                    Helper Functions
// =======================================================

// Wrap a floating-point hour value into [0, 24)
static inline double wrap24(double h) {
  h = fmod(h, 24.0);
  if (h < 0) h += 24.0;
  return h;
}

// Convert NMEA latitude/longitude to decimal degrees
static double nmeaToDecimal(double nmea, char hemi) {
  int deg = (int)floor(nmea / 100.0);
  double minutes = nmea - (deg * 100.0);
  double dec = deg + minutes / 60.0;
  if (hemi == 'W' || hemi == 'S') dec = -dec;
  return dec;
}

// Compute Julian Date from calendar date and UT
static double julianDate(int year, int month, int day, double UT_hours) {
  return (367.0 * year)
       - (int)((7.0 * (year + (int)((month + 9) / 12.0))) / 4.0)
       + (int)((275.0 * month) / 9.0)
       + day + 1721013.5 + (UT_hours / 24.0);
}

// Convert seconds since midnight to HH:MM:SS
static String hms_from_total_seconds(long total_seconds) {
  total_seconds = total_seconds % (24L * 3600L);
  if (total_seconds < 0) total_seconds += 24L * 3600L;
  int H = total_seconds / 3600;
  int M = (total_seconds % 3600) / 60;
  int S = total_seconds % 60;
  char buf[16];
  sprintf(buf, "%02d:%02d:%02d", H, M, S);
  return String(buf);
}

// Format local time based on TIME_FORMAT setting
static String formatLocalTimeFromInts(int H, int M, int S) {
  char buf[20];
  if (TIME_FORMAT == 0) {
    sprintf(buf, "%02d:%02d:%02d", H, M, S);
    return String(buf);
  } else {
    int displayH = H % 12;
    if (displayH == 0) displayH = 12;
    sprintf(buf, "%2d:%02d:%02d", displayH, M, S);
    String r = String(buf);
    r += (H < 12) ? " AM" : " PM";
    return r;
  }
}

// Print to LCD only if content has changed (prevents flicker)
void lcdPrintLineIfChanged(int row, const String &s) {
  String padded = s;
  if (padded.length() > 16) padded = padded.substring(0, 16);
  while (padded.length() < 16) padded += ' ';
  if (row == 0) {
    if (padded != lastTopStr) {
      lcd.setCursor(0,0);
      lcd.print(padded);
      lastTopStr = padded;
    }
  } else {
    if (padded != lastBotStr) {
      lcd.setCursor(0,1);
      lcd.print(padded);
      lastBotStr = padded;
    }
  }
}

// =======================================================
//                     Menu Content
// =======================================================

const char* menuNames[] = {
  "TZ Offset",
  "Time Format",
  "LST Adjust",
  "Exit Menu..."
};

// Return formatted value string for menu display
String menuValueString(int idx) {
  char buf[32];
  switch (idx) {
    case 0: sprintf(buf, "%+d", TZ_OFFSET); return String(buf);
    case 1: return (TIME_FORMAT == 0) ? "24h" : "12h";
    case 2: sprintf(buf, "%+d s", LST_ADJUST); return String(buf);
    default: return "";
  }
}

// =======================================================
//                     EEPROM I/O
// =======================================================

// Load saved settings and sanity-check them
void loadSettings() {
  Serial.println("DEBUG: Loading settings from EEPROM...");
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(EEPROM_ADDR_TZ, TZ_OFFSET);
  EEPROM.get(EEPROM_ADDR_LST, LST_ADJUST);
  EEPROM.get(EEPROM_ADDR_FMT, TIME_FORMAT);

  if (TZ_OFFSET < -12 || TZ_OFFSET > 14) TZ_OFFSET = -7;
  if (LST_ADJUST < -60 || LST_ADJUST > 60) LST_ADJUST = 0;
  if (TIME_FORMAT < 0 || TIME_FORMAT > 1) TIME_FORMAT = 1;
}

// Save current settings to EEPROM
void saveSettings() {
  EEPROM.put(EEPROM_ADDR_TZ, TZ_OFFSET);
  EEPROM.put(EEPROM_ADDR_LST, LST_ADJUST);
  EEPROM.put(EEPROM_ADDR_FMT, TIME_FORMAT);
  EEPROM.commit();
}

// =======================================================
//                        Setup
// =======================================================

void setup() {
  Serial.begin(115200);
  delay(800);

  lcd.begin(16, 2);
  loadSettings();

  // GPS initialization
  GPS.begin(0x10);
  GPS.sendCommand(PMTK_SET_NMEA_OUTPUT_RMCGGA);
  GPS.sendCommand(PMTK_SET_NMEA_UPDATE_1HZ);
  GPS.sendCommand(PGCMD_ANTENNA);
  delay(200);
  GPS.println(PMTK_Q_RELEASE);

  // Encoder detection and setup
  if (ss.begin(SEE_SAW_ADDR)) {
    encoder_present = true;
    ss.pinMode(24, INPUT_PULLUP);
    ss.setEncoderPosition(0);
    lastEncoderPos = ss.getEncoderPosition();
  } else {
    encoder_present = false;
    lcdPrintLineIfChanged(0, "No encoder found");
    lcdPrintLineIfChanged(1, "Clock runs only");
    delay(1200);
    lastTopStr = ""; lastBotStr = "";
  }
}

// =======================================================
//                   Timekeeping Logic
// =======================================================

// Handles GPS sync and sidereal extrapolation
void handleTimekeeping(double now_s) {
  if (!GPS.fix) return;

  // New GPS second received
  if (GPS.seconds != last_gps_second) {
    last_gps_second = GPS.seconds;

    // Apply timezone offset
    int H = GPS.hour + TZ_OFFSET;
    int M = GPS.minute;
    int S = GPS.seconds;

    while (S >= 60) { S -= 60; M++; }
    while (S < 0)  { S += 60; M--; }
    while (M >= 60) { M -= 60; H++; }
    while (M < 0)  { M += 60; H--; }
    while (H >= 24) H -= 24;
    while (H < 0)   H += 24;

    currentTimeStr = formatLocalTimeFromInts(H, M, S);

    // Compute LST from GPS time and longitude
    int year  = GPS.year + 2000;
    int month = GPS.month;
    int day   = GPS.day;
    double UT = GPS.hour + GPS.minute/60.0 + GPS.seconds/3600.0;

    double JD = julianDate(year, month, day, UT);
    double GMST = 18.697374558 + 24.06570982441908 * (JD - 2451545.0);
    GMST = wrap24(GMST);

    double lon_deg = nmeaToDecimal(GPS.longitude, GPS.lon);
    double LST = wrap24(GMST + lon_deg/15.0 + (LST_ADJUST / 3600.0));

    lst_hours_at_sync = LST;
    last_sync_time_s = now_s;

    long lst_total_seconds = (long)(LST * 3600.0);
    last_lst_total_seconds = lst_total_seconds;
    currentLSTStr = hms_from_total_seconds(lst_total_seconds);

  } else {
    // Sidereal extrapolation between GPS ticks
    double elapsed = now_s - last_sync_time_s;
    double lst_now_hours = lst_hours_at_sync + (elapsed * SIDEREAL_RATE) / 3600.0;
    lst_now_hours = wrap24(lst_now_hours);
    long lst_total_seconds_now = (long)(lst_now_hours * 3600.0);

    if (lst_total_seconds_now != last_lst_total_seconds) {
      currentLSTStr = hms_from_total_seconds(lst_total_seconds_now);
      last_lst_total_seconds = lst_total_seconds_now;
    }
  }
}

// =======================================================
//                   Display Rendering
// =======================================================

void drawClock() {
  String topLabel = (TZ_OFFSET == 0) ? "UTC:   " : "Local: ";
  lcdPrintLineIfChanged(0, topLabel + currentTimeStr);
  lcdPrintLineIfChanged(1, "LST:   " + currentLSTStr);
}

void drawMenu() {
  String name = String(menuNames[menuIndex]);
  String val  = menuValueString(menuIndex);

  char topBuf[17], botBuf[17];
  for (int i=0;i<16;i++) { topBuf[i]=' '; botBuf[i]=' '; }
  topBuf[16]=0; botBuf[16]=0;

  if (!editing) {
    topBuf[0] = '>';
    strncpy(topBuf+2, name.c_str(), min(14, (int)name.length()));
    strncpy(botBuf+2, val.c_str(), min(14, (int)val.length()));
  } else {
    botBuf[0] = '>';
    strncpy(topBuf+2, name.c_str(), min(14, (int)name.length()));
    strncpy(botBuf+2, val.c_str(), min(14, (int)val.length()));
  }

  lcdPrintLineIfChanged(0, String(topBuf));
  lcdPrintLineIfChanged(1, String(botBuf));
}

// =======================================================
//                    Menu Input Logic
// =======================================================

void handleMenuInput() {
  if (!encoder_present) return;

  int pos = ss.getEncoderPosition();
  long rawDelta = (long)pos - (long)lastEncoderPos;

  // Clamp runaway or wrapped deltas
  long normDelta = rawDelta;
  if (normDelta > 2)  normDelta = 1;
  if (normDelta < -2) normDelta = -1;

  int delta = (int)normDelta;

  if (delta != 0) {
    lastEncoderPos = pos;
    if (INVERT_ENCODER) delta = -delta;

    if (!editing) {
      menuIndex = (menuIndex + delta + menuCount) % menuCount;
    } else {
      if (menuIndex == 0) {
        TZ_OFFSET += delta;
        if (TZ_OFFSET > 14) TZ_OFFSET = -12;
        if (TZ_OFFSET < -12) TZ_OFFSET = 14;
      } else if (menuIndex == 1) {
        TIME_FORMAT = 1 - TIME_FORMAT;
      } else if (menuIndex == 2) {
        LST_ADJUST += delta;
        if (LST_ADJUST > 60) LST_ADJUST = -60;
        if (LST_ADJUST < -60) LST_ADJUST = 60;
      }
      saveSettings();
    }
  }

  // Button handling (menu enter / edit / exit)
  bool btn = (ss.digitalRead(24) == 0);
  unsigned long now = millis();
  if (btn != lastBtnState) {
    if (!btn && (now - lastButtonPress) > 200) {
      lastButtonPress = now;

      if (!inMenu) {
        inMenu = true;
        editing = false;
        menuIndex = 0;
        lastTopStr = ""; lastBotStr = "";
        drawMenu();
      } else {
        if (menuIndex == 3) {
          inMenu = false;
          editing = false;
          lastTopStr = ""; lastBotStr = "";
        } else {
          editing = !editing;
        }
      }
    }
  }
  lastBtnState = btn;
}

// =======================================================
//                         Loop
// =======================================================

void loop() {
  // Read GPS characters continuously
  char c = GPS.read();
  if (GPS.newNMEAreceived()) {
    GPS.parse(GPS.lastNMEA());
  }

  double now_s = millis() / 1000.0;

  handleTimekeeping(now_s);
  handleMenuInput();

  if (inMenu && encoder_present) {
    drawMenu();
  } else {
    if (GPS.fix) {
      drawClock();
    } else {
      lcdPrintLineIfChanged(0, "Waiting for fix ");
      lcdPrintLineIfChanged(1, "                ");
    }
  }
}
