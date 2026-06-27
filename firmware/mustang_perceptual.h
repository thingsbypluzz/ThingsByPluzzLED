// Jasność „ludzka”: menu/EEPROM = 0, 5, …, 100 % (krok 5) → dokładnie 21 wartości neo 0–255 (γ=2.2, monotonicznie).
// Migracja EEPROM 0xAA: neo 0–255 → % (LUT 256 B w PROGMEM).

#ifndef MUSTANG_PERCEPTUAL_H
#define MUSTANG_PERCEPTUAL_H

#include <Arduino.h>
#include <avr/pgmspace.h>

/** neo dla p = 0, 5, 10, …, 100 % — krańce 0 i 255, brak „martwych” powtórzeń na dole. */
static const uint8_t kPct5ToNeo[21] PROGMEM = {
  0, 1, 2, 4, 7, 12, 18, 25, 34, 44, 55, 68, 83, 99, 116, 135, 156, 178, 202, 228, 255
};

/** Zsynchronizuj % do siatki 0, 5, …, 100 (najbliższe 5 %). */
static inline int snapPercentStep5(int p) {
  p = constrain(p, 0, 100);
  int s = ((p + 2) / 5) * 5;
  if (s > 100) s = 100;
  return s;
}

static inline uint8_t neoFromPercent(int p) {
  uint8_t idx = (uint8_t)(snapPercentStep5(p) / 5);
  if (idx > 20) idx = 20;
  return pgm_read_byte(&kPct5ToNeo[idx]);
}

/** Stary driver liniowy 0–255 → % (γ=2.2), tylko przy migracji magic 0xAA. */
static const uint8_t kLinearNeo255ToPct[256] PROGMEM = {
  0, 8, 11, 13, 15, 17, 18, 20, 21, 22, 23, 24, 25, 26, 27, 28,
  28, 29, 30, 31, 31, 32, 33, 34, 34, 35, 35, 36, 37, 37, 38, 38,
  39, 39, 40, 41, 41, 42, 42, 43, 43, 44, 44, 45, 45, 45, 46, 46,
  47, 47, 48, 48, 49, 49, 49, 50, 50, 51, 51, 51, 52, 52, 53, 53,
  53, 54, 54, 54, 55, 55, 56, 56, 56, 57, 57, 57, 58, 58, 58, 59,
  59, 59, 60, 60, 60, 61, 61, 61, 62, 62, 62, 63, 63, 63, 64, 64,
  64, 64, 65, 65, 65, 66, 66, 66, 67, 67, 67, 67, 68, 68, 68, 69,
  69, 69, 69, 70, 70, 70, 70, 71, 71, 71, 72, 72, 72, 72, 73, 73,
  73, 73, 74, 74, 74, 74, 75, 75, 75, 75, 76, 76, 76, 76, 77, 77,
  77, 77, 78, 78, 78, 78, 79, 79, 79, 79, 80, 80, 80, 80, 80, 81,
  81, 81, 81, 82, 82, 82, 82, 82, 83, 83, 83, 83, 84, 84, 84, 84,
  84, 85, 85, 85, 85, 86, 86, 86, 86, 86, 87, 87, 87, 87, 87, 88,
  88, 88, 88, 89, 89, 89, 89, 89, 90, 90, 90, 90, 90, 91, 91, 91,
  91, 91, 92, 92, 92, 92, 92, 93, 93, 93, 93, 93, 94, 94, 94, 94,
  94, 94, 95, 95, 95, 95, 95, 96, 96, 96, 96, 96, 97, 97, 97, 97,
  97, 97, 98, 98, 98, 98, 98, 99, 99, 99, 99, 99, 99, 100, 100, 100
};

static inline uint8_t percentFromStoredLinearNeo(uint8_t neo) {
  return pgm_read_byte(&kLinearNeo255ToPct[neo]);
}

#endif
