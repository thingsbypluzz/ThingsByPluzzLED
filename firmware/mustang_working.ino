// =========================================================================
// MUSTANG LED CONTROLLER
// v6.24 - Wadliwe LED: osobna tabela PROGMEM na lampę lewą i prawą.
// v6.23 - Tabela wadliwych LED (PROGMEM): pominięcie + przesunięcie indeksów fizycznych.
// v6.22 - Krok 2: mapa 2×8×8 (blok0=0–63 wewn., blok1=64–127); row-major; lewa lustrzana.
// v6.21 - Krok 1: 128 LED/pin (2× blok 64); TURN_COLS 16. Mapa bloków → krok 2.
// v6.20 - Etykiety configMenu w PROGMEM (osobne stringi); OLED czyta strncpy_P.
// v6.19 - updateOLED + F() (flash); NeoPixel→begin() (heap, nie BSS — więcej miejsca na stos).
// v6.18 - 2× osobny bufor statyczny (nie współdzielony); beginStatic bez cichego fail.
// v6.17 - 1× bufor NeoPixel (render+show/strip); configMenu w PROGMEM; colorConfig×1.
// v6.16 - Bufory NeoPixel statyczne (BSS, bez malloc); mapa inline (−192 B RAM).
// v6.15 - Układ 12×8 (96 LED/pin); TURN_COLS 8→12.
// v6.14 - Cofnięto statyczne bufory NeoPixel (korupcja RAM); stripy→begin() na starcie setup.
// v6.13 - Statyczne bufory NeoPixel (bez malloc); stripy przed OLED; poprawiony boot-check.
// v6.12 - 8×8/pin: pełne pętle TURN_COLS; mapa row-major; boot-check stripów + RAM na OLED.
// v6.11 - Układ 16×8 (2× matryca 8×8, 128 LED/pin); mapa row-major; blok0 = wewnętrzna prawa.
// v6.10 - Menu CHECK LED: podmenu TEST/BACK; sekwencja R-G-B-W × kolumna × lewa/prawa (300 ms/krok).
// =========================================================================

#include <Adafruit_NeoPixel.h>
#include <Wire.h>
#include <EEPROM.h>
#include <avr/pgmspace.h>
#include <avr/wdt.h>
#include "SSD1306Ascii.h"
#include "SSD1306AsciiWire.h"
#include "mustang_settings.h"
#include "mustang_perceptual.h"  // po settings — prototypy Arduino po całym bloku #include

#define WATCHDOG_TIMEOUT WDTO_4S

// --- PINY ENKODERA ---
#define ENC_CLK 2
#define ENC_DT  3
#define ENC_SW  5

// --- KONFIGURACJA OLED ---
#define I2C_ADDRESS 0x3C
SSD1306AsciiWire oled;

// --- PINY ---
#define STRIP_PIN_TURN_LEFT      6
#define STRIP_PIN_TURN_RIGHT     8
#define BUTTON_PIN_RIGHT        11
#define BUTTON_PIN_LEFT         12
#define BUTTON_PIN_STOP          7
#define BUTTON_PIN_LIGHTS       10

#define STRIP_TURN_RIGHT         0
#define STRIP_TURN_LEFT          1
#define TURN_BLOCK_COLS          8
#define TURN_BLOCK_ROWS          8
#define LEDS_PER_BLOCK    (TURN_BLOCK_COLS * TURN_BLOCK_ROWS)  // 64
#define TURN_COLS               16
#define TURN_ROWS                8
#define NUMPIXELS_TURN    (2 * LEDS_PER_BLOCK)  // 128 = 2× matryca 8×8
#define TURN_CENTER_X    ((TURN_COLS - 1) * 0.5f)
#define TURN_CENTER_Y    ((TURN_ROWS - 1) * 0.5f)
#define TURN_MAX_DIST     8.3f

// --- PARAMETRY JASNOŚCI ---
#define TEST_MAX_BRIGHT        10
#define F1_FLASH_BRIGHT        255

// --- KONFIGURACJA F1 STOP ---
const int stopBlinkNumber   = 3;
const int stopBlinkDuration = 50;
const int stopGap           = 50;

// --- TIMINGI ---
const unsigned long DURATION_TURN_ON  = 1500;
const unsigned long DURATION_TURN_OFF = 600;



// --- TIMINGI DLA FILTRA KIERUNKOWSKAZÓW ---
unsigned long lastLeftPulseTime  = 0;
unsigned long lastRightPulseTime = 0;

// --- SYSTEM READY DELAY ---
bool systemReady = false;
unsigned long bootTime = 0;
const unsigned long STABILIZATION_DELAY = 750;

// --- DEBOUNCE WEJŚĆ Z AUTA (INPUT_PULLUP, aktywny = LOW) ---
const unsigned long INPUT_DEBOUNCE_MS = 20;

enum { VIN_LEFT = 0, VIN_RIGHT, VIN_STOP, VIN_LIGHTS, VIN_COUNT };

static const uint8_t VIN_PINS[VIN_COUNT] = {
  BUTTON_PIN_LEFT, BUTTON_PIN_RIGHT, BUTTON_PIN_STOP, BUTTON_PIN_LIGHTS
};

static bool     vinStable[VIN_COUNT];
static bool     vinPending[VIN_COUNT];
static unsigned long vinPendingSince[VIN_COUNT];

static void vehicleInputsInit() {
  unsigned long t = millis();
  for (int i = 0; i < VIN_COUNT; i++) {
    bool s = digitalRead(VIN_PINS[i]) == LOW;
    vinPending[i] = vinStable[i] = s;
    vinPendingSince[i] = t;
  }
}

static void vehicleInputsUpdate(unsigned long now) {
  for (int i = 0; i < VIN_COUNT; i++) {
    bool sample = digitalRead(VIN_PINS[i]) == LOW;
    if (sample != vinPending[i]) {
      vinPending[i] = sample;
      vinPendingSince[i] = now;
    }
    if ((unsigned long)(now - vinPendingSince[i]) >= INPUT_DEBOUNCE_MS)
      vinStable[i] = vinPending[i];
  }
}

// --- CENTRALNA KONFIGURACJA KOLORÓW ---
struct LampColors {
  uint32_t turn;
  uint32_t stop;
  uint32_t lightsOn;
};



LampColors colorConfig = { 0xFF5500, 0xFFFFFF, 0x400000 };

// Kolory DEMO — tylko flash (oszczędność RAM)
static const uint32_t demoColors_PGM[] PROGMEM = {
  0xFFFFFFUL, 0xFF0000UL, 0xFFFFFFUL, 0xFFFFFFUL
};

// =========================================================================
// --- SYSTEM MENU ---
// =========================================================================


enum MenuItemType {
  MENU_INT_RANGE,
  MENU_ENUM,
  MENU_ACTION,
  MENU_YESNO,
  MENU_DEMO_SUBMENU,
  MENU_DIAG_SUBMENU
};


struct MenuItem {
  const char*    lineNav;   // wiersz 0: nazwa w głównym menu (nawigacja)
  const char*    lineSub1;  // wiersz 1: pierwszy wiersz podmenu
  const char*    lineSub2;  // wiersz 2: drugi wiersz podmenu (pytanie YES/NO, może być NULL)
  MenuItemType   type;
  int            minVal;
  int            maxVal;
  int*           valuePointer;
  void           (*onSelect)();
  const char**   enumLabels;
  int            enumCount;
};

// --- PALETA KOLORÓW ENUM ---

const char* colorEnumLabels[] = { "RED", "ORANGE", "WHITE", "YELLOW" };
const int   COLOR_ENUM_COUNT  = 4;
static const uint32_t colorEnumValues_PGM[] PROGMEM = {
  0xFF0000UL, 0xFF5500UL, 0xFFFFFFUL, 0xFFAA00UL
};

// --- ZMIENNE STERUJĄCE ---
int demoModeFlag   = 0;
int f1StopEnabled  = 1;
int brightStop     = 30;
int brightTurn     = 30;
int brightLights   = 25;
int turnTimeout    = 600;
int colorStopIdx   = 2;
int colorTurnIdx   = 1;
int colorLightsIdx = 0;
int turnAnimIdx    = 0;
int turnAnimSpeed  = 900;  // ms — globalne tempo wszystkich animacji kierunkowskazów
int turnPosUnderlay = 1;   // 1 = podkład pozycyjnych pod TURN (gdy światła ON z auta)


void syncColors() {
  uint8_t si = (uint8_t)constrain(colorStopIdx, 0, COLOR_ENUM_COUNT - 1);
  uint8_t ti = (uint8_t)constrain(colorTurnIdx, 0, COLOR_ENUM_COUNT - 1);
  uint8_t li = (uint8_t)constrain(colorLightsIdx, 0, COLOR_ENUM_COUNT - 1);
  uint32_t sv = pgm_read_dword(&colorEnumValues_PGM[si]);
  uint32_t tv = pgm_read_dword(&colorEnumValues_PGM[ti]);
  uint32_t lv = pgm_read_dword(&colorEnumValues_PGM[li]);
  colorConfig.stop     = sv;
  colorConfig.turn     = tv;
  colorConfig.lightsOn = lv;
}

// =========================================================================
// --- REJESTR ANIMACJI KIERUNKOWSKAZÓW ---
// =========================================================================

typedef void (*TurnAnimFunc)(int stripIdx, uint32_t color);

// Forward declarations
void renderDiamondPulse(int sIdx, uint32_t color);
void renderStarFillPulse(int sIdx, uint32_t color);
void renderScanningAura(int sIdx, uint32_t color);
void renderContinuousOrganicTurn(int sIdx, uint32_t color, float ghosting);
void renderContinuousOrganicTurn2(int sIdx, uint32_t color);

void renderRadarTurnCalculated(int sIdx, uint32_t color, unsigned long duration, float headWidth, float tailLength);
void renderColumnFillPulse(int sIdx, uint32_t color);
void renderColumnWipe(int sIdx, uint32_t color);

// Wrappery — ujednolicona sygnatura (int, uint32_t), używają globalnego turnAnimSpeed
void anim_Diamond(int sIdx, uint32_t color)      {
  renderDiamondPulse(sIdx, color);
}
void anim_StarFill(int sIdx, uint32_t color)     {
  renderStarFillPulse(sIdx, color);
}
void anim_Scan(int sIdx, uint32_t color)         {
  renderScanningAura(sIdx, color);
}
void anim_ContOrganic(int sIdx, uint32_t color)  {
  renderContinuousOrganicTurn(sIdx, color, 10.0);
}
void anim_ContOrganic2(int sIdx, uint32_t color) {
  renderContinuousOrganicTurn2(sIdx, color);
}
void anim_Radar(int sIdx, uint32_t color)        {
  renderRadarTurnCalculated(sIdx, color, (unsigned long)turnAnimSpeed, 3.0, 3.0);
}
void anim_FillPulse(int sIdx, uint32_t color)    {
  renderColumnFillPulse(sIdx, color);
}
void anim_Wipe(int sIdx, uint32_t color)       {
  renderColumnWipe(sIdx, color);
}

struct TurnAnimDef {
  TurnAnimFunc func;
};

const TurnAnimDef turnAnimations[] = {
  { anim_Diamond     },
  { anim_StarFill    },
  { anim_Scan        },
  { anim_ContOrganic },
  { anim_ContOrganic2},
  { anim_Radar       },
  { anim_FillPulse   },
  { anim_Wipe        },
};
const int TURN_ANIM_COUNT = sizeof(turnAnimations) / sizeof(TurnAnimDef);


const char* turnAnimLabels[] = {
  "DIAMOND", "STAR", "SCAN",
  "CONT.ORG", "CONT.ORG2", "RADAR", "FILLPULS", "WIPE"
};

const char* turnPosUnderlayLabels[] = { "OFF", "ON" };
const int   TURN_POS_UNDERLAY_COUNT = 2;

// Forward declarations akcji menu
void menuActionBack();
void menuActionSave();
void menuActionResetDefault();

// --- Etykiety menu w flash (wskaźniki w configMenu, nie kopie w RAM) ---
const char menuLbl_demo_mode[]      PROGMEM = "DEMO MODE";
const char menuLbl_check_led[]      PROGMEM = "CHECK LED";
const char menuLbl_stop_bright[]    PROGMEM = "STOP BRIGHTNESS";
const char menuLbl_stop_color[]     PROGMEM = "STOP COLOR";
const char menuLbl_f1_break[]       PROGMEM = "F1 STYLE BREAK";
const char menuLbl_turn_bright[]    PROGMEM = "TURN BRIGHTNESS";
const char menuLbl_turn_color[]     PROGMEM = "TURN COLOR";
const char menuLbl_turn_underlay[]  PROGMEM = "TURN UNDERLAY";
const char menuLbl_turn_anim[]      PROGMEM = "TURN ANIMATION";
const char menuLbl_turn_speed[]     PROGMEM = "TURN SPEED MS";
const char menuLbl_pos_bright[]     PROGMEM = "POSITION BRIGHTNESS";
const char menuLbl_pos_color[]      PROGMEM = "POSITION COLOR";
const char menuLbl_flasher_to[]     PROGMEM = "FLASHER TIMEOUT MS";
const char menuLbl_save_set[]       PROGMEM = "SAVE SETTINGS";
const char menuLbl_reset_def[]      PROGMEM = "RESET DEFAULT";
const char menuLbl_back[]           PROGMEM = "BACK";

const char menuLbl_demo[]           PROGMEM = "DEMO";
const char menuLbl_check[]          PROGMEM = "CHECK";
const char menuLbl_stop[]           PROGMEM = "STOP";
const char menuLbl_f1_style[]       PROGMEM = "F1 STYLE";
const char menuLbl_turn[]           PROGMEM = "TURN";
const char menuLbl_position[]       PROGMEM = "POSITION";
const char menuLbl_flasher[]        PROGMEM = "FLASHER";
const char menuLbl_save[]           PROGMEM = "SAVE";
const char menuLbl_reset[]          PROGMEM = "RESET";

const char menuLbl_mode[]           PROGMEM = "MODE";
const char menuLbl_led[]            PROGMEM = "LED";
const char menuLbl_brightness[]     PROGMEM = "BRIGHTNESS";
const char menuLbl_color[]          PROGMEM = "COLOR";
const char menuLbl_f1_stop_q[]      PROGMEM = "F1 STOP?";
const char menuLbl_underlay[]       PROGMEM = "UNDERLAY";
const char menuLbl_animation[]      PROGMEM = "ANIMATION";
const char menuLbl_speed_ms[]       PROGMEM = "SPEED MS";
const char menuLbl_timeout_ms[]     PROGMEM = "TIMEOUT MS";
const char menuLbl_save_q[]         PROGMEM = "SAVE?";
const char menuLbl_reset_q[]        PROGMEM = "RESET?";

const MenuItem configMenu[] PROGMEM = {
  { menuLbl_demo_mode,     menuLbl_demo,     menuLbl_mode,       MENU_DEMO_SUBMENU, 0, 0,    NULL,            NULL,                   NULL,            0 },
  { menuLbl_check_led,     menuLbl_check,    menuLbl_led,        MENU_DIAG_SUBMENU, 0, 0,    NULL,            NULL,                   NULL,            0 },
  { menuLbl_stop_bright,   menuLbl_stop,     menuLbl_brightness, MENU_INT_RANGE,    0, 100,  &brightStop,     NULL,                   NULL,            0 },
  { menuLbl_stop_color,    menuLbl_stop,     menuLbl_color,      MENU_ENUM,         0, 0,    &colorStopIdx,   NULL,                   colorEnumLabels, COLOR_ENUM_COUNT },
  { menuLbl_f1_break,      menuLbl_f1_style, menuLbl_f1_stop_q,  MENU_YESNO,        0, 1,    &f1StopEnabled,  NULL,                   NULL,            0 },
  { menuLbl_turn_bright,   menuLbl_turn,     menuLbl_brightness, MENU_INT_RANGE,    0, 100,  &brightTurn,     NULL,                   NULL,            0 },
  { menuLbl_turn_color,    menuLbl_turn,     menuLbl_color,      MENU_ENUM,         0, 0,    &colorTurnIdx,   NULL,                   colorEnumLabels, COLOR_ENUM_COUNT },
  { menuLbl_turn_underlay, menuLbl_turn,     menuLbl_underlay,   MENU_ENUM,         0, 0,    &turnPosUnderlay,NULL,                   turnPosUnderlayLabels, TURN_POS_UNDERLAY_COUNT },
  { menuLbl_turn_anim,     menuLbl_turn,     menuLbl_animation,  MENU_ENUM,         0, 0,    &turnAnimIdx,    NULL,                   turnAnimLabels,  TURN_ANIM_COUNT },
  { menuLbl_turn_speed,    menuLbl_turn,     menuLbl_speed_ms,   MENU_INT_RANGE,    200, 2000, &turnAnimSpeed, NULL,                   NULL,            0 },
  { menuLbl_pos_bright,    menuLbl_position, menuLbl_brightness, MENU_INT_RANGE,    0, 100,  &brightLights,   NULL,                   NULL,            0 },
  { menuLbl_pos_color,     menuLbl_position, menuLbl_color,      MENU_ENUM,         0, 0,    &colorLightsIdx, NULL,                   colorEnumLabels, COLOR_ENUM_COUNT },
  { menuLbl_flasher_to,    menuLbl_flasher,  menuLbl_timeout_ms, MENU_INT_RANGE,    100, 2000, &turnTimeout,  NULL,                   NULL,            0 },
  { menuLbl_save_set,      menuLbl_save,     menuLbl_save_q,     MENU_YESNO,        0, 0,    NULL,            menuActionSave,         NULL,            0 },
  { menuLbl_reset_def,     menuLbl_reset,    menuLbl_reset_q,    MENU_YESNO,        0, 0,    NULL,            menuActionResetDefault, NULL,            0 },
  { menuLbl_back,          menuLbl_back,     NULL,               MENU_ACTION,       0, 0,    NULL,            menuActionBack,         NULL,            0 }
};

const int MENU_SIZE = sizeof(configMenu) / sizeof(MenuItem);

#define menuItemLoad(dst, idx) memcpy_P(&(dst), &configMenu[(idx)], sizeof(MenuItem))

// --- ZMIENNE STANU MENU ---
enum MenuMode { NAVIGATE, EDIT };
MenuMode currentMenuMode = NAVIGATE;
int menuIndex = 0;

volatile int encoderPos = 0;
int lastEncoderPos = 0;
bool isInMenu = false;
bool menuNeedsUpdate = true;
MenuMode lastMenuMode = NAVIGATE;

bool menuSaveAckActive = false;
unsigned long menuSaveAckEndMs = 0;

// --- STAN YESNO ---
bool pendingYesNo   = false;
int  yesNoSelection = 0;
int  yesNoSourceIdx = -1;

// --- PODMENU DEMO: obrót = wybór NXT/BCK, klik = zatwierdzenie ---
bool pendingDemoSub   = false;
int  demoSubSelection = 0;  // 0 = NXT, 1 = BCK

// --- CHECK LED: TEST / BACK (jak DEMO); sekwencja diagnostyczna bez delay() ---
bool pendingDiagSub   = false;
int  diagSubSelection = 0;  // 0 = TEST, 1 = BACK
bool diagTestActive   = false;
unsigned long diagRunStartMs = 0;

static const unsigned long DIAG_LED_STEP_MS = 300;
static const int DIAG_LED_STEPS = 2 * TURN_COLS * 4;  // lewa + prawa × kolumny × 4 barwy

void menuActionBack() {
  pendingDemoSub = false;
  pendingDiagSub = false;
  diagTestActive = false;
  demoModeFlag = 0;
  isInMenu = false;
  oled.clear();
}

void loadSettings() {
  Settings s;
  EEPROM.get(0, s);

  // Legacy: magic 0xAA — jasności zapisane liniowo 0–255 → % (LUT w mustang_perceptual.h)
  if (s.magic == 0xAA) {
    s.brightStop     = snapPercentStep5(constrain(percentFromStoredLinearNeo((uint8_t)constrain(s.brightStop, 0, 255)), 0, 100));
    s.brightTurn     = snapPercentStep5(constrain(percentFromStoredLinearNeo((uint8_t)constrain(s.brightTurn, 0, 255)), 0, 100));
    s.brightLights   = snapPercentStep5(constrain(percentFromStoredLinearNeo((uint8_t)constrain(s.brightLights, 0, 255)), 0, 100));
    s.magic          = SETTINGS_MAGIC;
    s.turnPosUnderlay = 1;
    EEPROM.put(0, s);
  } else if (s.magic != SETTINGS_MAGIC) {
    memcpy_P(&s, &DEFAULT_SETTINGS, sizeof(Settings));
  }

  if (s.turnPosUnderlay != 0 && s.turnPosUnderlay != 1)
    s.turnPosUnderlay = 1;

  brightStop     = snapPercentStep5(constrain(s.brightStop, 0, 100));
  brightTurn     = snapPercentStep5(constrain(s.brightTurn, 0, 100));
  brightLights   = snapPercentStep5(constrain(s.brightLights, 0, 100));
  turnTimeout    = s.turnTimeout;
  colorStopIdx   = s.colorStopIdx;
  colorTurnIdx   = s.colorTurnIdx;
  colorLightsIdx = s.colorLightsIdx;
  f1StopEnabled  = s.f1StopEnabled;
  turnAnimIdx    = constrain(s.turnAnimIdx, 0, TURN_ANIM_COUNT - 1);
  turnAnimSpeed  = constrain(s.turnAnimSpeed, 200, 2000);
  turnPosUnderlay = constrain(s.turnPosUnderlay, 0, 1);
}


Settings currentSettings() {
  Settings s;
  s.magic          = SETTINGS_MAGIC;
  s.brightStop     = brightStop;
  s.brightTurn     = brightTurn;
  s.brightLights   = brightLights;
  s.turnTimeout    = turnTimeout;
  s.colorStopIdx   = colorStopIdx;
  s.colorTurnIdx   = colorTurnIdx;
  s.colorLightsIdx = colorLightsIdx;
  s.f1StopEnabled  = f1StopEnabled;
  s.turnAnimIdx    = turnAnimIdx;
  s.turnAnimSpeed  = turnAnimSpeed;
  s.turnPosUnderlay = turnPosUnderlay;
  return s;
}


void menuActionSave() {
  Settings s = currentSettings();
  EEPROM.put(0, s);

  menuSaveAckActive = true;
  menuSaveAckEndMs = millis() + 1000;
  menuNeedsUpdate = true;
}


void menuActionResetDefault() {
  Settings def;
  memcpy_P(&def, &DEFAULT_SETTINGS, sizeof(Settings));
  brightStop     = def.brightStop;
  brightTurn     = def.brightTurn;
  brightLights   = def.brightLights;
  turnTimeout    = def.turnTimeout;
  colorStopIdx   = def.colorStopIdx;
  colorTurnIdx   = def.colorTurnIdx;
  colorLightsIdx = def.colorLightsIdx;
  f1StopEnabled  = def.f1StopEnabled;
  turnAnimIdx    = def.turnAnimIdx;
  turnAnimSpeed  = def.turnAnimSpeed;
  turnPosUnderlay = def.turnPosUnderlay;
  syncColors();
}


void readEncoder() {
  int clkValue = digitalRead(ENC_CLK);
  int dtValue  = digitalRead(ENC_DT);
  if (clkValue == dtValue) encoderPos--;
  else                     encoderPos++;
}


bool isButtonPressed() {
  static unsigned long lastPress = 0;
  if (digitalRead(ENC_SW) == LOW && millis() - lastPress > 300) {
    lastPress = millis();
    return true;
  }
  return false;
}

// =========================================================================
// --- ZMIENNE GLOBALNE ---
// =========================================================================

Adafruit_NeoPixel stripArray[] = {
  Adafruit_NeoPixel(NUMPIXELS_TURN, STRIP_PIN_TURN_RIGHT, NEO_GRB + NEO_KHZ800),
  Adafruit_NeoPixel(NUMPIXELS_TURN, STRIP_PIN_TURN_LEFT,  NEO_GRB + NEO_KHZ800),
};

// Skalowanie koloru jasnością (0–255), strip brightness = 255 — używane pod kierunki + podkład pozycyjnych.
static uint32_t scaleRgbBrightness(uint32_t color, int bright) {
  bright = constrain(bright, 0, 255);
  uint16_t r = ((uint16_t)(color >> 16) & 0xFF) * bright / 255;
  uint16_t g = ((uint16_t)(color >> 8) & 0xFF) * bright / 255;
  uint16_t b = ((uint16_t)color & 0xFF) * bright / 255;
  return stripArray[0].Color((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

// Odtwarza wcześniejsze: Color(rM*heat,...) + jasność kierunku z neoFromPercent(brightTurn), strip = 255.
static uint32_t turnLedColor(uint8_t rM, uint8_t gM, uint8_t bM, float heat) {
  int bt = (int)neoFromPercent(brightTurn);
  float e = constrain(heat, 0.0f, 1.0f) * (float)bt / 255.0f;
  return stripArray[0].Color(
           constrain((int)((float)rM * e + 0.5f), 0, 255),
           constrain((int)((float)gM * e + 0.5f), 0, 255),
           constrain((int)((float)bM * e + 0.5f), 0, 255));
}

enum VehicleState { IDLE, TURN_LEFT, TURN_RIGHT, HAZARD, BRAKING, DEMO, ANIMATING_LIGHTS_ON, ANIMATING_LIGHTS_OFF };
VehicleState currentState = IDLE;

bool flagLightsOn = false, prevLightsOnFlag = false, prevBrakeInput = false;
unsigned long animationStartTime = 0, brakeStartTime = 0;

// =========================================================================
// --- MAPOWANIE ---
// =========================================================================

/** Wadliwe / pominięte diody (mostek, brak chipa) — osobno na każdą lampę.
 *  Numery FIZYCZNE na linii danych (0 … NUMPIXELS_TURN−1), rosnąco, −1 = koniec.
 *  Przykład: { 42, -1 }   Brak usterek: { -1 } */
static const int8_t FAULTY_LED_RIGHT_PGM[] PROGMEM = { 31, -1 };
static const int8_t FAULTY_LED_LEFT_PGM[]  PROGMEM = { -1 };

/** Indeks poza zakresem — setPixelColor() ignoruje (dziura w siatce logicznej). */
#define MAPPED_PIXEL_NONE  NUMPIXELS_TURN

/** Idealny indeks fizyczny → indeks wysyłany do NeoPixel (korekta po wadliwych). */
static inline int remapStripIndex(int stripIdx, int ideal) {
  const int8_t* table = (stripIdx == STRIP_TURN_RIGHT)
                        ? FAULTY_LED_RIGHT_PGM
                        : FAULTY_LED_LEFT_PGM;
  int8_t shift = 0;
  for (uint8_t i = 0; ; i++) {
    int8_t fault = pgm_read_byte(&table[i]);
    if (fault < 0) break;
    if (ideal == fault) return MAPPED_PIXEL_NONE;
    if (ideal > fault) shift++;
  }
  return ideal - shift;
}

/** Logiczna siatka 16×8 → 2 bloki 8×8 na linii danych (128 LED).
 *  Blok 0 (col 0–7, wewnętrzna): LED 0–63; blok 1 (col 8–15): LED 64–127.
 *  W bloku: wiersz 0 u góry, row-major (idx = row×8 + col_w_bloku).
 *  Lewa lampa: lustrzane odbicie (col → 15−col). */
static inline int getMappedPixel(int stripIdx, int col, int row) {
  if (stripIdx == STRIP_TURN_LEFT)
    col = TURN_COLS - 1 - col;
  int ideal = (col / TURN_BLOCK_COLS) * LEDS_PER_BLOCK
              + row * TURN_BLOCK_COLS
              + (col % TURN_BLOCK_COLS);
  return remapStripIndex(stripIdx, ideal);
}

// =========================================================================
// --- CHECK LED (diagnostyka kolumn, menu) ---
// =========================================================================

static void diagLedTestStart() {
  demoModeFlag       = 0;
  diagTestActive     = true;
  diagRunStartMs     = millis();
}

/** Jedna pętla: lewa lampa kolumny 0..N−1 (R,G,B,W każda), potem prawa — bez delay(). */
static void renderLedCheckDiag(unsigned long now) {
  if (!diagTestActive) return;

  unsigned long t = now - diagRunStartMs;
  unsigned step = (unsigned)(t / DIAG_LED_STEP_MS);
  if (step >= (unsigned)DIAG_LED_STEPS) {
    diagTestActive = false;
    for (int s = 0; s < 2; s++) {
      stripArray[s].clear();
      stripArray[s].show();
    }
    menuNeedsUpdate = true;
    return;
  }

  const int colorsPerCol = 4;
  const int stepsPerStrip = TURN_COLS * colorsPerCol;
  uint8_t stripIdx = (step < (unsigned)stepsPerStrip) ? (uint8_t)STRIP_TURN_LEFT : (uint8_t)STRIP_TURN_RIGHT;
  unsigned local = (step < (unsigned)stepsPerStrip) ? step : (step - (unsigned)stepsPerStrip);
  int col = (int)(local / (unsigned)colorsPerCol);
  int colorIdx = (int)(local % (unsigned)colorsPerCol);

  uint32_t pal[4] = {
    stripArray[0].Color(255, 0, 0),
    stripArray[0].Color(0, 255, 0),
    stripArray[0].Color(0, 0, 255),
    stripArray[0].Color(255, 255, 255)
  };

  for (int s = 0; s < 2; s++) {
    stripArray[s].clear();
    stripArray[s].setBrightness(255);
  }
  uint32_t c = pal[colorIdx];
  for (int row = 0; row < TURN_ROWS; row++)
    stripArray[stripIdx].setPixelColor(getMappedPixel(stripIdx, col, row), c);
  for (int s = 0; s < 2; s++)
    stripArray[s].show();
}

// =========================================================================
// --- SILNIKI RENDERUJĄCE ---
// =========================================================================

void renderOrganicTurn(int sIdx, float progress, uint32_t color, float ghosting) {
  byte rM = (color >> 16) & 0xFF; byte gM = (color >> 8) & 0xFF; byte bM = color & 0xFF;
  const float colStep = 0.7f / (float)(TURN_COLS - 1);  // 0.1 przy 8 kol.; skaluje się z TURN_COLS
  for (int c = 0; c < TURN_COLS; c++) {
    float colProg = constrain((progress - (c * colStep)) * 2.5, 0.0, 1.0);
    if (colProg > 0) {
      for (int r = 0; r < TURN_ROWS; r++) {
        float heat = constrain(colProg * 1.5 - (abs(r - 3.5) * 0.2), 0.0, 1.0);
        if (heat > 0.02)
          stripArray[sIdx].setPixelColor(getMappedPixel(sIdx, c, r), stripArray[sIdx].Color(rM * heat, gM * heat, bM * heat));

      }
    }
  }
}

// Wszystkie funkcje poniżej używają globalnego turnAnimSpeed zamiast lokalnych stałych.

void renderDiamondPulse(int sIdx, uint32_t color) {
  byte rM = (color >> 16) & 0xFF, gM = (color >> 8) & 0xFF, bM = color & 0xFF;





  float globalProgress = (float)(millis() % (unsigned long)turnAnimSpeed) / turnAnimSpeed;

  float centerX = TURN_CENTER_X, centerY = TURN_CENTER_Y, maxDist = TURN_MAX_DIST;

  for (int c = 0; c < TURN_COLS; c++) {
    for (int r = 0; r < TURN_ROWS; r++) {
      float dist = sqrt(pow(c - centerX, 2) + pow(r - centerY, 2));

      float waveFront = globalProgress * 1.4;
      float heat = constrain(1.0 - abs(dist / maxDist - waveFront) / 0.4, 0.0, 1.0);

      if (globalProgress > 0.7) heat *= (1.0 - (globalProgress - 0.7) * 3.33);

      if (heat > 0.01)


        stripArray[sIdx].setPixelColor(getMappedPixel(sIdx, c, r), turnLedColor(rM, gM, bM, heat));


    }
  }
}

void renderStarFillPulse(int sIdx, uint32_t color) {
  byte rM = (color >> 16) & 0xFF, gM = (color >> 8) & 0xFF, bM = color & 0xFF;





  float globalProgress = (float)(millis() % (unsigned long)turnAnimSpeed) / turnAnimSpeed;
  float growth = sin(globalProgress * PI);

  float centerX = TURN_CENTER_X, centerY = TURN_CENTER_Y, maxDist = TURN_MAX_DIST;

  float currentRadius = growth * maxDist;

  for (int c = 0; c < TURN_COLS; c++) {
    for (int r = 0; r < TURN_ROWS; r++) {
      float dist = sqrt(pow(c - centerX, 2) + pow(r - centerY, 2));

      float heat = constrain((currentRadius - dist) / 0.8 + 0.5, 0.0, 1.0);
      if (heat > 0.02)


        stripArray[sIdx].setPixelColor(getMappedPixel(sIdx, c, r), turnLedColor(rM, gM, bM, heat));


    }
  }
}

void renderScanningAura(int sIdx, uint32_t color) {
  byte rM = (color >> 16) & 0xFF, gM = (color >> 8) & 0xFF, bM = color & 0xFF;





  float globalProgress = (float)(millis() % (unsigned long)turnAnimSpeed) / turnAnimSpeed;
  float centerRow = 3.5 + (sin(globalProgress * 2.0 * PI) * 5.0);



  for (int col = 0; col < TURN_COLS; col++) {
    for (int row = 0; row < TURN_ROWS; row++) {

      float heat = constrain(1.0 - (abs((float)row - centerRow) / 2.2), 0.0, 1.0);
      if (heat > 0.02)


        stripArray[sIdx].setPixelColor(getMappedPixel(sIdx, col, row), turnLedColor(rM, gM, bM, heat));


    }
  }
}

void renderRadarTurnCalculated(int sIdx, uint32_t color, unsigned long duration, float headWidth, float tailLength) {
  byte rM = (color >> 16) & 0xFF, gM = (color >> 8) & 0xFF, bM = color & 0xFF;




  float globalProgress = (float)(millis() % duration) / duration;
  float waveHead = globalProgress * ((float)TURN_COLS + tailLength);

  for (int col = 0; col < TURN_COLS; col++) {
    float dist = waveHead - (float)col;
    float heat = 0.0;


    if (dist >= 0 && dist <= headWidth) {
      heat = 1.0;
    } else if (dist > headWidth && dist <= (headWidth + tailLength)) {

      heat = pow(constrain(1.0 - ((dist - headWidth) / tailLength), 0.0, 1.0), 1.5);

    }

    if (heat > 0.01) {
      for (int row = 0; row < TURN_ROWS; row++)


        stripArray[sIdx].setPixelColor(getMappedPixel(sIdx, col, row), turnLedColor(rM, gM, bM, heat));


    }
  }
}

void renderContinuousOrganicTurn(int sIdx, uint32_t color, float ghosting) {
  byte rM = (color >> 16) & 0xFF, gM = (color >> 8) & 0xFF, bM = color & 0xFF;
  // turnAnimSpeed = czas trwania fali; przerwa = połowa tego czasu
  unsigned long waveDur = (unsigned long)turnAnimSpeed;
  unsigned long waveGap = waveDur / 2;
  unsigned long cycle   = waveDur + waveGap;
  for (int c = 0; c < TURN_COLS; c++) {
    float offset = (float)c * 0.12 * waveDur;
    unsigned long localTime = (millis() - (unsigned long)offset) % cycle;
    float progress = (localTime < waveDur) ? (float)localTime / (waveDur / 2.0) : 0;
    if (progress > 0) {
      for (int r = 0; r < TURN_ROWS; r++) {
        float heat = constrain(progress - (abs(r - 3.5) * 0.25), 0.0, 1.0);

        if (heat > 1.0) heat = max(0.0f, 1.0f - (heat - 1.0f) * (2.0f / ghosting));
        if (heat > 0.02)
          stripArray[sIdx].setPixelColor(getMappedPixel(sIdx, c, r), turnLedColor(rM, gM, bM, heat));

      }
    }
  }
}

void renderContinuousOrganicTurn2(int sIdx, uint32_t color) {
  byte rM = (color >> 16) & 0xFF, gM = (color >> 8) & 0xFF, bM = color & 0xFF;

  float globalProgress = (float)(millis() % (unsigned long)turnAnimSpeed) / turnAnimSpeed;
  const float colStep = 0.7f / (float)(TURN_COLS - 1);  // 0.1 przy 8 kol.; skaluje się z TURN_COLS

  for (int c = 0; c < TURN_COLS; c++) {
    float colProg = constrain((globalProgress - (c * colStep)) * 2.5, 0.0, 1.0);

    if (globalProgress > 0.8) colProg *= (1.0 - (globalProgress - 0.8) * 5.0);

    if (colProg > 0) {
      for (int r = 0; r < TURN_ROWS; r++) {
        float heat = constrain(colProg * 1.5 - (abs(r - 3.5) * 0.2), 0.0, 1.0);
        if (heat > 0.02)

          stripArray[sIdx].setPixelColor(getMappedPixel(sIdx, c, r), turnLedColor(rM, gM, bM, heat));

      }
    }
  }
}

void renderColumnFillPulse(int sIdx, uint32_t color) {
  byte rM = (color >> 16) & 0xFF, gM = (color >> 8) & 0xFF, bM = color & 0xFF;

  const float fillFrac = 0.3f;  // szybkie wypełnienie 0→15, wolniejsze wygaszanie 15→0
  float cycle = (float)(millis() % (unsigned long)turnAnimSpeed) / turnAnimSpeed;
  float edge;

  if (cycle < fillFrac)
    edge = (cycle / fillFrac) * (float)TURN_COLS;
  else
    edge = (1.0f - (cycle - fillFrac) / (1.0f - fillFrac)) * (float)TURN_COLS;

  for (int col = 0; col < TURN_COLS; col++) {
    if ((float)col >= edge) continue;
    for (int row = 0; row < TURN_ROWS; row++)
      stripArray[sIdx].setPixelColor(getMappedPixel(sIdx, col, row), turnLedColor(rM, gM, bM, 1.0f));
  }
}

void renderColumnWipe(int sIdx, uint32_t color) {
  byte rM = (color >> 16) & 0xFF, gM = (color >> 8) & 0xFF, bM = color & 0xFF;

  const float tFillEnd = 0.30f;
  const float tHoldOn  = 0.40f;  // +10% pauza: wszystkie kolumny włączone
  const float tWipeEnd = 0.90f;  // +50% wygaszanie 0→15
  float cycle = (float)(millis() % (unsigned long)turnAnimSpeed) / turnAnimSpeed;

  for (int col = 0; col < TURN_COLS; col++) {
    bool on = false;
    if (cycle < tFillEnd) {
      float edge = (cycle / tFillEnd) * (float)TURN_COLS;
      on = ((float)col < edge);
    } else if (cycle < tHoldOn) {
      on = true;
    } else if (cycle < tWipeEnd) {
      float wipeProg = (cycle - tHoldOn) / (tWipeEnd - tHoldOn);
      float darkEdge = wipeProg * (float)TURN_COLS;
      on = ((float)col >= darkEdge);
    }
    if (!on) continue;
    for (int row = 0; row < TURN_ROWS; row++)
      stripArray[sIdx].setPixelColor(getMappedPixel(sIdx, col, row), turnLedColor(rM, gM, bM, 1.0f));
  }
}

// =========================================================================
// --- DEMO (auto: hold → fade → następna scena; NXT z menu = skok + reset timera) ---
// =========================================================================

static const unsigned long DEMO_ANIM_MS   = 20000;
static const unsigned long DEMO_FADE_MS  = 5000;
static const unsigned long DEMO_CYCLE_MS = DEMO_ANIM_MS + DEMO_FADE_MS;

static uint8_t         demoSceneIdx   = 0;
static unsigned long   demoAnchorMs   = 0;
static uint32_t        demoAutoCycles = 0;

static void demoSubmenuEnter() {
  diagTestActive   = false;
  demoModeFlag     = 1;
  demoSceneIdx     = 0;
  demoAnchorMs     = millis();
  demoAutoCycles   = 0;
}

void demoForceNextImmediate() {
  diagTestActive   = false;
  demoSceneIdx   = (uint8_t)((demoSceneIdx + 1) % 4);
  demoAnchorMs   = millis();
  demoAutoCycles   = 0;
}

void renderDemoMode() {
  unsigned long cur = millis();
  unsigned long elapsed = cur - demoAnchorMs;
  uint32_t fullC = (uint32_t)(elapsed / DEMO_CYCLE_MS);
  if (fullC > demoAutoCycles) {
    uint32_t d = fullC - demoAutoCycles;
    demoSceneIdx = (uint8_t)((demoSceneIdx + d) % 4);
    demoAutoCycles = fullC;
  }
  unsigned long cycleProgress = elapsed % DEMO_CYCLE_MS;

  /* Stan demoSceneIdx podbija się na początku *następnego* cyklu (granica pełnego CYCLE).
     W fade-in (trans >= 2750) masterFade rośnie — bez tego nadal rysowalibyśmy starą scenę,
     więc wizualnie: stara animacja wraca z czerni, dopiero potem skok indeksu w stanie. */
  uint8_t renderScene = demoSceneIdx;
  float masterFade = 1.0;
  if (cycleProgress > DEMO_ANIM_MS) {
    unsigned long trans = cycleProgress - DEMO_ANIM_MS;
    if (trans >= 2750)
      renderScene = (uint8_t)((demoSceneIdx + 1) % 4);
    if (trans < 2250)      masterFade = 1.0 - (trans / 2250.0);
    else if (trans < 2750) masterFade = 0.0;
    else                   masterFade = (trans - 2750) / 2250.0;
  }

  uint32_t activeCol = pgm_read_dword(&demoColors_PGM[renderScene]);
  byte rM = (activeCol >> 16) & 0xFF, gM = (activeCol >> 8) & 0xFF, bM = activeCol & 0xFF;

  for (int s = 0; s < 2; s++) {
    stripArray[s].setBrightness(255);
    stripArray[s].clear();

    if (renderScene == 0) {
      float speed = cur * 0.002;
      for (int i = 0; i < NUMPIXELS_TURN; i++) {
        int p = remapStripIndex(s, i);
        if (p >= NUMPIXELS_TURN) continue;
        float b = ((sin(speed + (i * 0.15)) * 127 + 128) / 255.0) * masterFade;
        stripArray[s].setPixelColor(p, turnLedColor(rM, gM, bM, constrain(b, 0.0f, 1.0f)));
      }

    } else if (renderScene == 1) {
      float pulse = sin(cur * 0.0015) * TURN_CENTER_Y + TURN_CENTER_Y;
      for (int col = 0; col < TURN_COLS; col++)
        for (int row = 0; row < TURN_ROWS; row++) {
          float heat = constrain(1.0 - (abs(row - pulse) * 0.4), 0.0, 1.0) * masterFade;
          stripArray[s].setPixelColor(getMappedPixel(s, col, row), turnLedColor(rM, gM, bM, heat));
        }

    } else if (renderScene == 2) {
      const float globalSpan = (float)(TURN_COLS * 2 - 1);
      float waveCenter = sin(cur * 0.001) * (globalSpan * 0.5f) + (globalSpan * 0.5f);
      for (int col = 0; col < TURN_COLS; col++) {
        float globalCol = (s == STRIP_TURN_LEFT)
                          ? (float)(TURN_COLS - 1 - col)
                          : (float)(TURN_COLS + col);
        float colIntensity = constrain(1.0 - (abs(globalCol - waveCenter) * 0.4), 0.0, 1.0) * masterFade;
        if (colIntensity > 0.05)
          for (int row = 0; row < TURN_ROWS; row++) {
            float rowH = constrain(1.0 - (abs(row - TURN_CENTER_Y) * 0.25), 0.0, 1.0) * colIntensity;
            stripArray[s].setPixelColor(getMappedPixel(s, col, row), turnLedColor(rM, gM, bM, rowH));
          }
      }

    } else {
      const float globalSpan = (float)(TURN_COLS * 2 - 1);
      float waveCenter = sin(cur * 0.001) * (globalSpan * 0.5f) + (globalSpan * 0.5f);
      for (int col = 0; col < TURN_COLS; col++) {
        float globalCol = (s == STRIP_TURN_LEFT)
                          ? (float)(TURN_COLS - 1 - col)
                          : (float)(TURN_COLS + col);
        float floorInt = constrain(1.0 - (abs(globalCol - waveCenter) * 0.3), 0.0, 1.0) * masterFade;
        int headH = (int)(floorInt * (float)(TURN_ROWS - 1));
        for (int h = 0; h <= headH; h++)
          stripArray[s].setPixelColor(getMappedPixel(s, col, (TURN_ROWS - 1) - h), turnLedColor(rM, gM, bM, floorInt));
      }
    }
  }
}

// =========================================================================
// --- LOGIKA ŚWIATEŁ ---
// =========================================================================

uint32_t getBrakeColor(int sIdx, bool b) {
  if (!b) return 0;

  if (f1StopEnabled) {
    unsigned long elapsed = millis() - brakeStartTime;
    unsigned long cycleTime = stopBlinkDuration + stopGap;
    if (elapsed < (unsigned long)stopBlinkNumber * cycleTime)

      return (elapsed % cycleTime < (unsigned long)stopBlinkDuration) ? colorConfig.stop : 0x000000;

  }

  return colorConfig.stop;
}

bool isTurnActive(bool currentInput, unsigned long &lastPulseTime) {

  if (currentInput) {
    lastPulseTime = millis();
    return true;
  }

  return (millis() - lastPulseTime < (unsigned long)turnTimeout);

}

// =========================================================================
// --- MENU: SETUP & OBSŁUGA ---
// =========================================================================

void setupMenu() {
  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT, INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENC_CLK), readEncoder, CHANGE);
}

void handleMenu() {
  if (isButtonPressed()) {
    if (!isInMenu) {
      isInMenu = true;
      currentMenuMode = NAVIGATE;
      menuNeedsUpdate = true;
      return;
    }

    if (pendingYesNo) {
      if (yesNoSelection == 0) {
        MenuItem src;
        menuItemLoad(src, yesNoSourceIdx);
        if (src.onSelect != NULL)        src.onSelect();

        else if (src.valuePointer != NULL) *(src.valuePointer) = !(*(src.valuePointer));
      }
      pendingYesNo = false; yesNoSourceIdx = -1;
      menuNeedsUpdate = true; oled.clear();
      return;
    }

    if (pendingDemoSub) {
      if (demoSubSelection == 0) {
        demoForceNextImmediate();
      } else {
        demoModeFlag = 0;
        pendingDemoSub = false;
        for (int i = 0; i < 2; i++) {
          stripArray[i].clear();
          stripArray[i].show();
        }
      }
      menuNeedsUpdate = true;
      oled.clear();
      return;
    }

    if (pendingDiagSub) {
      if (diagSubSelection == 0) {
        diagLedTestStart();
      } else {
        diagTestActive = false;
        for (int i = 0; i < 2; i++) {
          stripArray[i].clear();
          stripArray[i].show();
        }
      }
      pendingDiagSub = false;
      menuNeedsUpdate = true;
      oled.clear();
      return;
    }

    MenuItem item;
    menuItemLoad(item, menuIndex);

    if (item.type == MENU_DEMO_SUBMENU) {
      demoSubmenuEnter();
      pendingDemoSub     = true;
      demoSubSelection   = 0;
      menuNeedsUpdate = true;
      oled.clear();
      return;
    }

    if (item.type == MENU_DIAG_SUBMENU) {
      diagTestActive     = false;
      demoModeFlag       = 0;
      pendingDiagSub     = true;
      diagSubSelection   = 0;
      menuNeedsUpdate = true;
      oled.clear();
      return;
    }

    if (item.type == MENU_YESNO) {

      pendingYesNo = true; yesNoSelection = 1; yesNoSourceIdx = menuIndex;
      menuNeedsUpdate = true; oled.clear();

      return;
    }

    if (item.type == MENU_ACTION) {
      if (item.onSelect != NULL) item.onSelect();
      return;
    }

    // INT_RANGE / ENUM: NAVIGATE <-> EDIT
    if (currentMenuMode == NAVIGATE) {
      currentMenuMode = EDIT;
    } else {
      currentMenuMode = NAVIGATE;
      // Zgaś strips po wyjściu z edycji brightness lub animacji
      if (item.valuePointer == &brightStop || item.valuePointer == &brightTurn ||
          item.valuePointer == &brightLights || item.valuePointer == &turnAnimIdx ||
          item.valuePointer == &turnAnimSpeed || item.valuePointer == &turnPosUnderlay) {
        for (int i = 0; i < 2; i++) {
          stripArray[i].clear();
          stripArray[i].show();
        }
      }
      if (item.type == MENU_ENUM) syncColors();
    }
    menuNeedsUpdate = true;
  }

  if (!isInMenu) return;

  int step = encoderPos - lastEncoderPos;
  if (abs(step) >= 2) {
    int dir = (step > 0) ? 1 : -1;


    if (pendingYesNo) {
      yesNoSelection = (yesNoSelection == 0) ? 1 : 0;

      menuNeedsUpdate = true; lastEncoderPos = encoderPos;
      return;
    }

    if (pendingDemoSub) {
      demoSubSelection = (demoSubSelection == 0) ? 1 : 0;
      menuNeedsUpdate = true;
      lastEncoderPos = encoderPos;
      return;
    }

    if (pendingDiagSub) {
      diagSubSelection = (diagSubSelection == 0) ? 1 : 0;
      menuNeedsUpdate = true;
      lastEncoderPos = encoderPos;
      return;
    }

    if (currentMenuMode == NAVIGATE) {

      menuIndex = (menuIndex + dir + MENU_SIZE) % MENU_SIZE;


    } else {
      MenuItem item;
      menuItemLoad(item, menuIndex);

      if (item.type == MENU_ENUM) {
        int newIdx = (*(item.valuePointer) + dir + item.enumCount) % item.enumCount;


        *(item.valuePointer) = newIdx;

        // Live preview: kolor — fill; animacja — renderowanie w loop()


        if (item.valuePointer != &turnAnimIdx && item.valuePointer != &turnPosUnderlay) {


          syncColors();
          uint32_t previewColor = (item.valuePointer == &colorTurnIdx) ? colorConfig.turn
                                  : (item.valuePointer == &colorStopIdx) ? colorConfig.stop
                                  : colorConfig.lightsOn;
          for (int s = 0; s < 2; s++) {
            stripArray[s].setBrightness(TEST_MAX_BRIGHT);
            stripArray[s].fill(previewColor);
            stripArray[s].show();
          }
        }
        // turnAnimIdx: live preview dzieje się w loop()

      } else {
        // INT_RANGE: krok 5 (jasności co 5 % zgodnie z mapą) lub 10 dla turnAnimSpeed
        int step_size = (item.valuePointer == &turnAnimSpeed) ? 10 : 5;
        int newValue = *(item.valuePointer) + dir * step_size;
        if (newValue >= item.minVal && newValue <= item.maxVal)
          *(item.valuePointer) = newValue;

        // Live preview brightness
        if (item.valuePointer == &brightStop || item.valuePointer == &brightTurn || item.valuePointer == &brightLights) {

          uint32_t previewColor = (item.valuePointer == &brightTurn)  ? colorConfig.turn
                                  : (item.valuePointer == &brightStop)  ? colorConfig.stop
                                  : colorConfig.lightsOn;
          for (int s = 0; s < 2; s++) {
            int pct = (item.valuePointer == &brightTurn) ? brightTurn
                      : (item.valuePointer == &brightStop) ? brightStop : brightLights;
            stripArray[s].setBrightness(neoFromPercent(pct));
            stripArray[s].fill(previewColor);
            stripArray[s].show();
          }
        }
        // turnAnimSpeed: live preview dzieje się w loop() (animacja na żywo)
      }
    }

    menuNeedsUpdate = true;
    lastEncoderPos = encoderPos;
  }
}

// =========================================================================
// --- MENU: RYSOWANIE ---
// =========================================================================

static const int OLED_MENU_COLS = 21;
static const int MENU_ROW_SUB1  = 0;  // wiersz 1 podmenu
static const int MENU_ROW_SUB2  = 1;  // wiersz 2
static const int MENU_ROW_VALUE = 2;  // wiersz 3: wartość / wybór
static const int MENU_ROW_BAR   = 3;  // wiersz 4: pasek postępu

static void printOledMenuLineRam(int row, const char* text) {
  oled.setCursor(0, row);
  if (text) oled.print(text);
  oled.clearToEOL();
  oled.println("");
}

/** Tekst wskaźnikiem do PROGMEM (etykiety configMenu). */
static void printOledMenuLine(int row, const char* textP) {
  if (!textP) {
    printOledMenuLineRam(row, NULL);
    return;
  }
  char buf[OLED_MENU_COLS + 1];
  strncpy_P(buf, textP, OLED_MENU_COLS);
  buf[OLED_MENU_COLS] = '\0';
  printOledMenuLineRam(row, buf);
}

static void printOledMenuLineCentered(int row, const char* text) {
  oled.setCursor(0, row);
  if (!text) {
    oled.clearToEOL();
    oled.println("");
    return;
  }
  int len = (int)strlen(text);
  int pad = (OLED_MENU_COLS - len) / 2;
  if (pad < 0) pad = 0;
  for (int p = 0; p < pad; p++) oled.print(" ");
  oled.print(text);
  oled.clearToEOL();
  oled.println("");
}

static void drawSubmenuHeader(const char* lineSub1P, const char* lineSub2P) {
  printOledMenuLine(MENU_ROW_SUB1, lineSub1P);
  printOledMenuLine(MENU_ROW_SUB2, lineSub2P);
}

void drawProgressBar(int value, int minVal, int maxVal) {
  const int BAR_WIDTH = OLED_MENU_COLS - 2;
  int filled = map(value, minVal, maxVal, 0, BAR_WIDTH);
  oled.print("[");
  for (int i = 0; i < BAR_WIDTH; i++) oled.print(i < filled ? "#" : "-");
  oled.print("]");
}

// Wiersze 1–2: nagłówek; 3: row3 (wyśrodkowany); 4: opcjonalny pasek
static void drawSubmenuScreen(const char* lineSub1, const char* lineSub2,
                              const char* row3, bool showBar,
                              int barValue, int barMin, int barMax) {
  oled.setCursor(0, 0);
  oled.set1X();
  drawSubmenuHeader(lineSub1, lineSub2);
  printOledMenuLineCentered(MENU_ROW_VALUE, row3);
  if (showBar) {
    oled.setCursor(0, MENU_ROW_BAR);
    drawProgressBar(barValue, barMin, barMax);
    oled.clearToEOL();
  }
}

static void formatMenuIntValue(int* valuePointer, char* buf, size_t bufSize) {
  int val = *valuePointer;
  bool isMs = (valuePointer == &turnTimeout || valuePointer == &turnAnimSpeed);
  bool isBrightPct = (valuePointer == &brightStop || valuePointer == &brightTurn
                      || valuePointer == &brightLights);
  if (isMs) snprintf(buf, bufSize, "%dms", val);
  else if (isBrightPct) snprintf(buf, bufSize, "%d%%", val);
  else snprintf(buf, bufSize, "%d", val);
}

static void drawMenuEditScreen(int menuIdx) {
  MenuItem item;
  menuItemLoad(item, menuIdx);
  char valBuf[16];
  const char* row3 = NULL;
  bool showBar = false;
  int barValue = 0;
  int barMin = 0;
  int barMax = 0;

  if (item.type == MENU_INT_RANGE && item.valuePointer != NULL) {
    formatMenuIntValue(item.valuePointer, valBuf, sizeof(valBuf));
    row3 = valBuf;
    showBar = true;
    barValue = *(item.valuePointer);
    barMin = item.minVal;
    barMax = item.maxVal;
  } else if (item.type == MENU_ENUM && item.valuePointer != NULL) {
    row3 = item.enumLabels[*(item.valuePointer)];
    showBar = true;
    barValue = *(item.valuePointer);
    barMin = 0;
    barMax = item.enumCount - 1;
  }

  drawSubmenuScreen(item.lineSub1, item.lineSub2, row3, showBar, barValue, barMin, barMax);
}

void drawMenu() {

  if (menuSaveAckActive) {
    oled.clear();
    oled.set1X();
    oled.setCursor(0, 0);
    oled.println("");
    oled.println("  SAVED!");
    return;
  }

  if (pendingYesNo && yesNoSourceIdx >= 0) {
    MenuItem src;
    menuItemLoad(src, yesNoSourceIdx);
    const char* choices = (yesNoSelection == 0) ? "[YES] NO" : " YES [NO]";
    drawSubmenuScreen(src.lineSub1, src.lineSub2, choices, false, 0, 0, 0);
    return;
  }

  if (pendingDemoSub) {
    MenuItem src;
    menuItemLoad(src, menuIndex);
    const char* choices = (demoSubSelection == 0) ? "[NXT] BACK" : " NXT [BACK]";
    drawSubmenuScreen(src.lineSub1, src.lineSub2, choices, false, 0, 0, 0);
    return;
  }

  if (pendingDiagSub) {
    MenuItem src;
    menuItemLoad(src, menuIndex);
    const char* choices = (diagSubSelection == 0) ? "[TEST] BACK" : " TEST [BACK]";
    drawSubmenuScreen(src.lineSub1, src.lineSub2, choices, false, 0, 0, 0);
    return;
  }

  if (currentMenuMode != lastMenuMode) {
    oled.clear();
    lastMenuMode = currentMenuMode;
  }

  oled.setCursor(0, 0); oled.set1X();

  if (currentMenuMode == EDIT) {
    drawMenuEditScreen(menuIndex);
    return;
  }

  // NAVIGATE

  const int VISIBLE = 4;
  int scrollTop = menuIndex - 1;
  if (scrollTop < 0) scrollTop = 0;
  if (scrollTop > MENU_SIZE - VISIBLE) scrollTop = MENU_SIZE - VISIBLE;

  for (int v = 0; v < VISIBLE; v++) {
    int i = scrollTop + v;
    if (i >= MENU_SIZE) break;


    MenuItem navItem;
    menuItemLoad(navItem, i);
    oled.print(menuIndex == i ? ">" : " ");
    if (navItem.lineNav) {
      char navBuf[OLED_MENU_COLS + 1];
      strncpy_P(navBuf, navItem.lineNav, OLED_MENU_COLS);
      navBuf[OLED_MENU_COLS] = '\0';
      oled.print(navBuf);
    }
    oled.clearToEOL(); oled.println("");

  }
}

// =========================================================================
// --- SETUP & LOOP ---
// =========================================================================

void setup() {
  wdt_disable();

  /* Stripy przed Wire/OLED — bufor pikseli na heap (nie w global RAM). */
  for (int i = 0; i < 2; i++) {
    stripArray[i].begin();
    stripArray[i].setBrightness(TEST_MAX_BRIGHT);
    stripArray[i].clear();
    stripArray[i].show();
  }

  setupMenu();
  pinMode(BUTTON_PIN_RIGHT, INPUT_PULLUP);
  pinMode(BUTTON_PIN_LEFT,  INPUT_PULLUP);
  pinMode(BUTTON_PIN_STOP,  INPUT_PULLUP);
  pinMode(BUTTON_PIN_LIGHTS, INPUT_PULLUP);

  vehicleInputsInit();
  bootTime = millis();

  loadSettings();
  syncColors();

  Wire.begin();
  Wire.setClock(400000L);
  oled.begin(&Adafruit128x32, I2C_ADDRESS);
  oled.setFont(Adafruit5x7);
  oled.clear();
  oled.println(F("MUSTANG SYSTEM OK"));
  oled.println(F("Czekam na sygnal..."));

  /* Brak resetu WDT >4 s w jednej iteracji loop() → reset MCU (zawieszka / I2C). */
  wdt_enable(WATCHDOG_TIMEOUT);
}



void updateOLED(const __FlashStringHelper* status) {
  static const __FlashStringHelper* lastStatus = NULL;
  if (status != lastStatus) {
    oled.clear(); oled.set1X();

    oled.println(F(" STATUS:"));
    oled.println("");
    oled.println(status);
    lastStatus = status;
  }
}

void loop() {
  wdt_reset();

  const unsigned long now = millis();
  vehicleInputsUpdate(now);
  bool rawL = vinStable[VIN_LEFT];
  bool rawR = vinStable[VIN_RIGHT];
  bool b = vinStable[VIN_STOP];
  flagLightsOn = vinStable[VIN_LIGHTS];

  if (!(isInMenu && menuSaveAckActive))
    handleMenu();

  if (isInMenu) {
    if (menuSaveAckActive && (long)(now - menuSaveAckEndMs) >= 0) {
      menuSaveAckActive = false;
      oled.clear();
      menuNeedsUpdate = true;
    }
    if (menuNeedsUpdate) {
      drawMenu();
      menuNeedsUpdate = false;
    }

    // Diagnostyka LED ma pierwszeństwo przed podglądem edycji (spójny pełnoekranowy test).
    MenuItem cur;
    menuItemLoad(cur, menuIndex);
    if (diagTestActive) {
      renderLedCheckDiag(now);
    } else if (currentMenuMode == EDIT &&
        (cur.valuePointer == &turnAnimIdx || cur.valuePointer == &turnAnimSpeed ||
         cur.valuePointer == &turnPosUnderlay)) {
      for (int s = 0; s < 2; s++) {
        stripArray[s].clear();
        stripArray[s].setBrightness(255);
        if (flagLightsOn && turnPosUnderlay)
          stripArray[s].fill(scaleRgbBrightness(colorConfig.lightsOn, neoFromPercent(brightLights)));
        turnAnimations[turnAnimIdx].func(s, colorConfig.turn);
        stripArray[s].show();
      }
    } else if (demoModeFlag) {
      renderDemoMode();
      for (int i = 0; i < 2; i++) stripArray[i].show();
    }

    return;
  }

  bool l = isTurnActive(rawL, lastLeftPulseTime);
  bool r = isTurnActive(rawR, lastRightPulseTime);

  switch (currentState) {
    case IDLE:                 updateOLED(flagLightsOn ? F("POZYCYJNE") : F("OFF")); break;
    case TURN_LEFT:            updateOLED(F("LEWY <<"));    break;
    case TURN_RIGHT:           updateOLED(F("PRAWY >>"));   break;
    case HAZARD:               updateOLED(F("AWARIA!"));    break;
    case BRAKING:              updateOLED(F("STOP"));       break;
    case DEMO:                 updateOLED(F("TRYB DEMO"));  break;
    case ANIMATING_LIGHTS_ON:  updateOLED(F("WELCOME...")); break;
    case ANIMATING_LIGHTS_OFF: updateOLED(F("GOODBYE"));    break;
  }

  if (b && !prevBrakeInput) brakeStartTime = now;
  prevBrakeInput = b;


  if (!systemReady) {
    if (now - bootTime > STABILIZATION_DELAY) systemReady = true;


    else {
      currentState = IDLE;
      return;
    }


  }

  // --- LOGIKA STANU ---
  VehicleState ns = IDLE;

  if (b || l || r) {
    if (demoModeFlag) demoModeFlag = 0;
    if (l && r)      ns = HAZARD;
    else if (l)      ns = TURN_LEFT;
    else if (r)      ns = TURN_RIGHT;
    else             ns = BRAKING;
  } else if (demoModeFlag) {
    ns = DEMO;
  } else if (!flagLightsOn) {
    ns = (prevLightsOnFlag || currentState == ANIMATING_LIGHTS_OFF) ? ANIMATING_LIGHTS_OFF : IDLE;
  } else {
    if (!prevLightsOnFlag) ns = ANIMATING_LIGHTS_ON;
    else ns = (currentState == ANIMATING_LIGHTS_ON) ? currentState : IDLE;
  }

  if (ns != currentState) {


    for (int i = 0; i < 2; i++) {
      stripArray[i].clear();
      stripArray[i].setBrightness(TEST_MAX_BRIGHT);
    }

    currentState = ns;
    animationStartTime = now;
  }

  // --- RENDEROWANIE ---
  if (currentState == ANIMATING_LIGHTS_ON) {
    float p = constrain((float)(now - animationStartTime) / DURATION_TURN_ON, 0.0, 1.0);
    for (int s = 0; s < 2; s++) {
      stripArray[s].setBrightness(neoFromPercent(brightLights));
      stripArray[s].clear();
      renderOrganicTurn(s, p, colorConfig.lightsOn, 10.0);
    }
    if (p >= 1.0) currentState = IDLE;

  } else if (currentState == ANIMATING_LIGHTS_OFF) {
    float p = constrain((float)(now - animationStartTime) / DURATION_TURN_OFF, 0.0, 1.0);
    for (int s = 0; s < 2; s++) {
      stripArray[s].setBrightness(neoFromPercent(brightLights));
      stripArray[s].clear();
      renderOrganicTurn(s, 1.0 - p, colorConfig.lightsOn, 10.0);
    }

    if (p >= 1.0) {
      for (int s = 0; s < 2; s++) stripArray[s].clear();
      currentState = IDLE;
    }


  } else if (currentState == DEMO) {
    renderDemoMode();

  } else {
    for (int i = 0; i < 2; i++) {
      stripArray[i].clear();
      bool turn = (i == STRIP_TURN_RIGHT && r) || (i == STRIP_TURN_LEFT && l) || (currentState == HAZARD);
      if (turn) {

        stripArray[i].setBrightness(255);
        if (flagLightsOn && turnPosUnderlay)
          stripArray[i].fill(scaleRgbBrightness(colorConfig.lightsOn, neoFromPercent(brightLights)));
        turnAnimations[turnAnimIdx].func(i, colorConfig.turn);
      } else if (b) {
        stripArray[i].setBrightness(neoFromPercent(brightStop));
        stripArray[i].fill(getBrakeColor(i, b));
      } else if (flagLightsOn) {
        stripArray[i].setBrightness(neoFromPercent(brightLights));
        stripArray[i].fill(colorConfig.lightsOn);
      }
    }
  }

  for (int i = 0; i < 2; i++) stripArray[i].show();
  prevLightsOnFlag = flagLightsOn;
}
