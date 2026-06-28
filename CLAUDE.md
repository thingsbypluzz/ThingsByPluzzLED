# Mustang LED Visualizer — Kontekst Projektu

## Czym jest ten projekt

Aplikacja desktopowa do wizualizacji i testowania animacji LED dla tylnych lamp Forda Mustanga 1965.
Zastępuje fizyczny układ testowy podczas pisania i iterowania kodu animacji — bez potrzeby wgrywania firmware na Arduino za każdym razem.

## Kontekst sprzętowy (fizyczny układ docelowy)

### Architektura układu
- **Mikrokontroler:** Arduino Nano (ATmega328P)
- **Dwie lampy** (lewa / prawa), każda złożona z **2× matrycy 8×8 WS2812B (NeoPixel)**
- **128 diod na lampę, 256 diod łącznie**
- **Układ serpentyny:** fizyczny pasek biegnie wężem przez kolumny — mapowanie koryguje funkcja `generateSerpentineMaps()` w firmware
- **Klosze:** oryginalne czerwone klosze Mustanga — diody świecą przez czerwony filtr

### Sygnały wejściowe (z instalacji samochodowej, przez optoizolator PC847)
| Sygnał       | Opis                                                                 |
|--------------|----------------------------------------------------------------------|
| LIGHTS_ON    | Światła pozycyjne włączone                                           |
| STOP         | Sygnał z czujnika pedału hamulca (dedykowany przewód, niezależny)   |
| LEFT_TURN    | Kierunkowskaz lewy (sygnał przerywany przez flasher ~500ms on/off)  |
| RIGHT_TURN   | Kierunkowskaz prawy (j.w.)                                           |

### Specyfika Mustanga '65
W oryginalnej instalacji STOP i TURN idą tym samym kablem (LEFT/RIGHT świeci ciągłe przy STOP, migające przy kierunku). W tym projekcie problem rozwiązany przez **dedykowany przewód STOP** — Arduino otrzymuje 4 niezależne sygnały.

### Układ matrycy (ważne dla wizualizatora)
```
Lampa lewa i prawa mają identyczną strukturę:
- 2 matryce 8×8 = 128 diod
- Ułożone obok siebie: razem tworzą prostokąt 8 wierszy × 16 kolumn
- Dane lecą jako jeden ciągły pasek NeoPixel (serpentyna)
- Kolumny parzyste: diody idą od góry do dołu (indeks 0..7)
- Kolumny nieparzyste: diody idą od dołu do góry (indeks 7..0)
```

Widok z zewnątrz auta (to co widzi obserwator):
```
[ LEWA LAMPA          ] [ PRAWA LAMPA         ]
[ col0 col1 ... col15 ] [ col0 col1 ... col15 ]
```

---

## Stan aplikacji (czerwiec 2026)

### Zaimplementowane funkcje
1. **Panel podglądu LED** — dwie siatki 8×16 (lewa/prawa lampa), każda dioda to kolorowe kółko z efektem glow
2. **Symulator sygnałów** — przyciski: LIGHTS_ON, STOP, LEFT_TURN, RIGHT_TURN; kierunkowskazy symulują flasher (~500ms)
3. **Symulator state machine** — IDLE, TURN_L/R, HAZARD, BRAKING, ANIM_ON/OFF (identyczna logika co firmware)
4. **Edytor kodu animacji** — textarea z JS-owym kodem ciała funkcji; Apply (lub Ctrl+Enter) re-kompiluje przez `eval` i podmienia funkcję na żywo
5. **Panel parametrów per-animacja** — suwaki dla każdej animacji (np. Wave Scale, Head Width, Fill Frac); zmiany działają natychmiast bez recompile (parametry `p.xxx` przekazywane przy każdej klatce)
6. **Edytor kolorów i jasności** — osobne color pickery + suwaki dla Turn, Stop, Lights
7. **Parametry F1 Stop** — konfigurowalne: liczba błysków, czas on/off
8. **Podkład pozycyjnych** — toggle Underlay (pozycyjne pod animacją kierunkowskazów)
9. **Kontrola prędkości animacji** — suwak turnAnimSpeed (200–2000ms)

### Funkcje na później
- Zapis edytowanej animacji do osobnego pliku `.js` w `visualiser/` + ładowanie przy starcie
- Dodawanie nowych animacji z poziomu UI
- Połączenie przez Serial USB z fizycznym Arduino
- Eksport GIF/wideo animacji

---

## Stack technologiczny

**MVP: Pojedynczy plik `index.html` + vanilla JavaScript + Canvas**
- Zero zależności, zero build step, zero konfiguracji
- Hostowany na GitHub Pages — dostępny z iPhone pod stałym linkiem
- W przyszłości można opakować w Electron dla dostępu do Serial USB

Nie używaj frameworków (React, Vue itp.) na etapie MVP — niepotrzebny overhead.
Jeśli projekt urośnie, migracja jest prosta.

---

## Kluczowe struktury danych z firmware (referencja)

### Wersja firmware: v6.24 (mustang_working.ino)

### Układ fizyczny matrycy
```
Każda lampa = 2× matryca 8×8 WS2812B = 128 diod = jeden ciągły pasek NeoPixel
Logiczny widok: 16 kolumn × 8 wierszy

Blok 0 (kolumny 0–7):  indeksy NeoPixel 0–63
Blok 1 (kolumny 8–15): indeksy NeoPixel 64–127

Serpentyna (row-major, blok po bloku):
  - kolumny parzyste (0,2,4...):  wiersz 0→7 (góra→dół)
  - kolumny nieparzyste (1,3,5...): wiersz 7→0 (dół→góra)

UWAGA: lewa lampa ma lustrzane mapowanie względem prawej!
```

### Kolory i jasność
```cpp
struct LampColors {
  uint32_t turn;      // kolor kierunkowskazów (domyślnie 0xFF5500 = pomarańczowy)
  uint32_t stop;      // kolor STOP           (domyślnie 0xFFFFFF = biały)
  uint32_t lightsOn;  // kolor pozycyjnych    (domyślnie 0x400000 = ciemna czerwień)
};
LampColors colorConfig;  // jedna wspólna konfiguracja dla obu lamp

// Jasność: EEPROM przechowuje 0–100% (skala percepcyjna γ=2.2)
// Funkcja konwersji: neoFromPercent(int p) → uint8_t 0–255
// Domyślne jasności: stop=30%, turn=30%, lights=25%

// Paleta kolorów (enum):
// 0: RED    0xFF0000
// 1: ORANGE 0xFF5500  ← domyślny TURN
// 2: WHITE  0xFFFFFF  ← domyślny STOP
// 3: YELLOW 0xFFAA00
```

### Stany systemu (state machine)
```
IDLE              → świeci tylko pozycyjne (jeśli LIGHTS_ON)
TURN_LEFT         → animacja lewej lampy
TURN_RIGHT        → animacja prawej lampy
HAZARD            → animacja obu lamp jednocześnie
BRAKING           → STOP (F1 flash + stałe)
DEMO              → tryb demo (z menu)
ANIMATING_LIGHTS_ON  → animacja wejścia świateł (1500ms)
ANIMATING_LIGHTS_OFF → animacja wyjścia świateł (600ms)
```

### Zarejestrowane animacje kierunkowskazów (turnAnimIdx)
```
0: Diamond Pulse         renderDiamondPulse()
1: Star Fill Pulse       renderStarFillPulse()
2: Scanning Aura         renderScanningAura()
3: Continuous Organic    renderContinuousOrganicTurn(ghosting=10.0)
4: Continuous Organic 2  renderContinuousOrganicTurn2()
5: Radar                 renderRadarTurnCalculated()
6: Column Fill Pulse     renderColumnFillPulse()
7: Column Wipe           renderColumnWipe()
```
Wszystkie animacje mają sygnaturę: `void func(int stripIdx, uint32_t color)`
Globalny parametr czasu: `turnAnimSpeed` (domyślnie 900ms)

### Logika kierunkowskazu (ważne dla symulatora)
```
Sygnał LEFT/RIGHT z auta jest przerywany przez flasher (~500ms on/off).
Firmware filtruje ten sygnał przez isTurnActive() z timeoutem turnTimeout (domyślnie 600ms).
W wizualizatorze symuluj to jako przycisk który automatycznie pulsuje co ~500ms.
```

### Efekt F1 Stop
```
Po wciśnięciu hamulca: 3× błysk (50ms on / 50ms off) pełną jasnością (255),
następnie stałe świecenie z jasnością brightStop%.
Konfigurowalne: stopBlinkNumber=3, stopBlinkDuration=50ms, stopGap=50ms
```

### Podkład pozycyjnych pod TURN (turnPosUnderlay)
```
Gdy LIGHTS_ON=true i aktywny kierunkowskaz:
  strip.fill(lightsOn @ brightLights%)   ← najpierw pozycyjne
  turnAnim.func(strip, turn)             ← animacja na wierzch
```

### Piksele — format koloru
```
NeoPixel: 0xRRGGBB (32-bit uint32_t)
Skalowanie jasności: scaleRgbBrightness(color, neo_0_255)
```

---

## Deployment — GitHub Pages

### Struktura repozytorium
```
ThingsByPluzzLED/          ← publiczne repo na GitHubie
├── CLAUDE.md
├── firmware/
│   ├── mustang_working.ino       ← firmware v6.24
│   ├── mustang_settings.h
│   └── mustang_perceptual.h
└── visualiser/
    └── index.html                ← cała aplikacja (jeden plik HTML+JS)
```

### Repo
- GitHub: https://github.com/thingsbypluzz/ThingsByPluzzLED
- GitHub Pages URL (po włączeniu): https://thingsbypluzz.github.io/ThingsByPluzzLED/

### Konfiguracja GitHub Pages (jednorazowo)
1. Wejdź na github.com/thingsbypluzz/ThingsByPluzzLED → Settings → Pages
2. Source: `Deploy from branch` → branch `main`, folder `/ (root)`
3. Zapisz — strona będzie dostępna pod adresem powyżej w ciągu ~1 minuty

### Workflow przy każdej zmianie
```bash
git add visualiser/index.html
git commit -m "opis zmiany"
git push
# GitHub Pages automatycznie aktualizuje stronę w ciągu ~1 minuty
```

### Pierwsze uruchomienie (jednorazowa konfiguracja)
```bash
git init
git remote add origin https://github.com/thingsbypluzz/ThingsByPluzzLED.git
git branch -M main
git push -u origin main
```
Następnie włącz GitHub Pages w ustawieniach repo (jak wyżej).

---

## Instrukcje dla Claude Code

### Architektura edytora kodu (ważne przy dalszej pracy)

Animacje w `visualiser/index.html` są przechowywane jako stringi z ciałem funkcji JS (`ANIM_SOURCES_DEFAULT[]`).
Sygnatura każdej funkcji: `(buf, sIdx, color, now, p)` gdzie:
- `buf` — Uint8Array pikseli lampy
- `sIdx` — indeks paska (0=prawa, 1=lewa)
- `color` — 0xRRGGBB kolor (np. `colorConfig.turn`)
- `now` — timestamp w ms (jak `millis()` w firmware)
- `p` — obiekt parametrów animacji (`animParamVals[idx]`)

**Dostępne globale w kodzie animacji** (eval działa w bieżącym scope):
```
state.turnAnimSpeed, state.brightTurn, colorConfig.turn/stop/lightsOn
TURN_COLS, TURN_ROWS, TURN_CENTER_X, TURN_CENTER_Y, TURN_MAX_DIST
turnLedRGB(rM,gM,bM,heat), lightsLedRGB(), setLed(buf,sIdx,c,r,rv,gv,bv)
neoFromPercent(pct), scaleRgb(color,bright)
```

**Uwaga:** Używamy `eval(...)` bezpośrednio (nie Web Worker) — aplikacja hostowana na zaufanym GitHub Pages, edytor jest narzędziem dewelopera. Upraszcza architekturę i daje dostęp do lokalnego scope.

### Zasady kodowania
- Kod animacji w firmware używa **nieblokującej pętli loop()** — żadnych `delay()`, tylko liczniki czasu (`millis()`)
- Wizualizator odwzorowuje tę samą logikę — `requestAnimationFrame` z `now` (timestamp) jako odpowiednik `millis()`
- Mapowanie `getMappedPixel(stripIdx, col, row)` MUSI być identyczne jak w firmware — lewa lampa lustrzana
- Kolory NeoPixel są w formacie 0xRRGGBB (32-bit uint)

### Czego unikać
- Nie upraszczać mapowania `getMappedPixel` — błędy spowodują że animacje będą wyglądać inaczej niż w aucie
- Nie blokować głównego wątku renderowania
- Parametry animacji przekazywać przez obiekt `p` (nie hardkodować) — to pozwala na suwaki bez recompile

---

## Pliki projektu na Google Drive

Folder: `LED_TailLights` (Google Drive, właściciel: pluzztezeusz@gmail.com)
- `Mustang LED - podsumowanie` — główny raport projektu z firmware i historią decyzji
- `Opis schematu` — schemat elektryczny i lista połączeń
- `Mustang LED - Stan Faktyczny Projektu` — aktualny stan projektu (czerwiec 2026)
- Arkusz kalkulacyjny — lista zakupów komponentów

## Pliki projektu

```
ThingsByPluzzLED/
├── firmware/
│   ├── mustang_working.ino       ← główny plik firmware v6.24
│   ├── mustang_settings.h        ← struktura Settings + EEPROM defaults
│   └── mustang_perceptual.h      ← konwersja jasności % → NeoPixel (γ=2.2)
├── visualiser/
│   └── index.html                ← wizualizator (gotowy, single-file)
└── CLAUDE.md
```

Przy zmianach w wizualizatorze czytaj firmware jako źródło prawdy — szczególnie funkcje animacji i `getMappedPixel`.

---

*Projekt: DIY tylne lampy LED, Ford Mustang 1965. Właściciel: pluzztezeusz@gmail.com*
