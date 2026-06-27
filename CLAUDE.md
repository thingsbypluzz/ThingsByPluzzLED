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

```cpp
// Konfiguracja kolorów
struct LampColors {
  uint32_t positionColor;   // kolor świateł pozycyjnych
  uint32_t stopColor;       // kolor świateł STOP
  uint32_t turnColor;       // kolor kierunkowskazów
  uint8_t  positionBrightness;
  uint8_t  stopBrightness;
  uint8_t  turnBrightness;
};

LampColors colorConfig[2]; // [0] = lewa, [1] = prawa

// Mapowanie serpentyny
// serpentineMap[strip][col][row] = indeks diody w pasku NeoPixel
int serpentineMap[2][16][8];

// Główne funkcje animacji (do symulacji)
void renderOrganicTurn(int strip, bool entering);
void renderContinuousOrganicTurn(int strip);
void renderDemoMode();
uint32_t getBrakeColor(uint32_t baseColor, bool isFlashing);
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

---

*Projekt: DIY tylne lampy LED, Ford Mustang 1965. Właściciel: pluzztezeusz@gmail.com*
