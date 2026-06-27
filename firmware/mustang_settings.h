// Typ i domyślne EEPROM — w osobnym pliku, #include na końcu bloku include w .ino,
// żeby Arduino wstrzyknęło prototypy funkcji *po* znanym już typie Settings.
// Jasności STOP/TURN/POS: 0–100 % (EEPROM), mapowanie liniowe → NeoPixel w mustang_perceptual.h

#ifndef MUSTANG_SETTINGS_H
#define MUSTANG_SETTINGS_H

#include <Arduino.h>
#include <avr/pgmspace.h>

// 0xAB: jasności STOP/TURN/POS w EEPROM jako 0–100 % (perceptualna skala), v6.5
#define SETTINGS_MAGIC 0xAB

struct Settings {
  uint8_t magic;
  int brightStop;
  int brightTurn;
  int brightLights;
  int turnTimeout;
  int colorStopIdx;
  int colorTurnIdx;
  int colorLightsIdx;
  int f1StopEnabled;
  int turnAnimIdx;
  int turnAnimSpeed;
  int turnPosUnderlay;  // 0/1: podkład pozycyjnych pod animację TURN (gdy światła z auta ON)
};

const Settings DEFAULT_SETTINGS PROGMEM = {
  SETTINGS_MAGIC,
  30,   // brightStop   [% perceptual]
  30,   // brightTurn   [%]
  25,   // brightLights [%]
  600,  // turnTimeout [ms]
  2,    // colorStopIdx  (WHITE)
  1,    // colorTurnIdx  (ORANGE)
  0,    // colorLightsIdx (RED)
  1,    // f1StopEnabled
  0,    // turnAnimIdx (DIAMOND)
  900,  // turnAnimSpeed [ms]
  1     // turnPosUnderlay (ON)
};

#endif
