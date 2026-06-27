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

## MVP — Co ma robić aplikacja

### Cel główny
Wklejasz fragment kodu C++ (funkcję animacji z firmware Arduino) → aplikacja renderuje jak ta animacja wygląda w czasie rzeczywistym na wirtualnych matrycach LED.

### Funkcje MVP
1. **Panel podglądu LED** — dwie siatki 8×16 (lewa/prawa lampa), każda dioda to kolorowe kółko
2. **Symulator sygnałów** — przyciski/przełączniki do wyzwalania: LIGHTS_ON, STOP, LEFT_TURN, RIGHT_TURN
3. **Edytor kodu animacji** — pole tekstowe, wklejasz funkcję C++ animacji
4. **Transpilacja/interpretacja** — aplikacja parsuje lub transpiluje logikę animacji do JavaScript i uruchamia ją w pętli renderowania
5. **Filtr czerwony** — opcja nałożenia czerwonego filtra (symulacja kloszów Mustanga)
6. **Kontrola czasu** — suwak prędkości animacji, pauza, reset

### Funkcje na później (poza MVP)
- Połączenie przez Serial USB z fizycznym Arduino (odbiór danych z symulatora)
- Zapis/odtwarzanie sekwencji animacji
- Eksport GIF/wideo animacji
- Edytor kolorów i jasności z podglądem na żywo

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
├── index.html             ← cała aplikacja MVP (jeden plik)
└── README.md              ← krótki opis projektu
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
git add index.html
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

### Priorytet prac
1. Zacznij od działającego renderera matrycy LED w przeglądarce (HTML/Canvas lub React)
2. Dodaj symulator sygnałów wejściowych
3. Zaimplementuj najprostszą animację kierunkowskazu ręcznie w JS (jako punkt odniesienia)
4. Dopiero potem buduj mechanizm wklejania/parsowania kodu C++

### Zasady kodowania
- Kod animacji w firmware używa **nieblokującej pętli loop()** — żadnych `delay()`, tylko liczniki czasu (`millis()`)
- Wizualizator musi odwzorowywać tę samą logikę — używaj `requestAnimationFrame` lub setInterval z deltaTime
- Serpentyna MUSI być zaimplementowana identycznie jak w firmware — to krytyczne dla poprawności wizualizacji
- Kolory NeoPixel są w formacie 0xRRGGBB (32-bit uint)

### Czego unikać
- Nie uprościć mapowania serpentyny — błędy tam spowodują że animacje będą wyglądać inaczej niż w aucie
- Nie blokować głównego wątku renderowania przy parsowaniu kodu użytkownika
- Sandbox kod użytkownika (eval w Web Worker lub iframe sandbox) dla bezpieczeństwa

---

## Pliki projektu na Google Drive

Folder: `LED_TailLights` (Google Drive, właściciel: pluzztezeusz@gmail.com)
- `Mustang LED - podsumowanie` — główny raport projektu z firmware i historią decyzji
- `Opis schematu` — schemat elektryczny i lista połączeń
- `Mustang LED - Stan Faktyczny Projektu` — aktualny stan projektu (czerwiec 2026)
- Arkusz kalkulacyjny — lista zakupów komponentów

## Pliki firmware w repozytorium

```
ThingsByPluzzLED/
├── firmware/
│   ├── mustang_working.ino       ← główny plik firmware v6.24
│   ├── mustang_settings.h        ← struktura Settings + EEPROM defaults
│   └── mustang_perceptual.h      ← konwersja jasności % → NeoPixel (γ=2.2)
├── visualizer/
│   └── index.html                ← wizualizator (cel budowy)
└── CLAUDE.md
```

Przy budowie wizualizatora czytaj firmware jako źródło prawdy — szczególnie funkcje animacji i mapowanie serpentyny.

---

*Projekt: DIY tylne lampy LED, Ford Mustang 1965. Właściciel: pluzztezeusz@gmail.com*
