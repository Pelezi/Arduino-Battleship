#include <Adafruit_NeoPixel.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <avr/pgmspace.h>
#include <math.h>

#ifndef FPSTR
#define FPSTR(p) (reinterpret_cast<const __FlashStringHelper *>(p))
#endif

LiquidCrystal_I2C lcd1(0x26, 16, 2);
LiquidCrystal_I2C lcd2(0x27, 16, 2);

// -------- Macros para imprimir no LCD sem gastar RAM --------
#define LCD_MSG(L, L1) \
  do { \
    (L).clear(); \
    (L).setCursor(0, 0); \
    (L).print(L1); \
  } while (0)

#define LCD_MSG2(L, L1, L2) \
  do { \
    (L).clear(); \
    (L).setCursor(0, 0); \
    (L).print(L1); \
    (L).setCursor(0, 1); \
    (L).print(L2); \
  } while (0)

#define LCD_BOTH_MSG(L1, L2) \
  do { \
    LCD_MSG2(lcd1, (L1), (L2)); \
    LCD_MSG2(lcd2, (L1), (L2)); \
  } while (0)

// printf simples (2ª linha)
#define LCD_PRINTF(L, L1, FMT, VAL) \
  do { \
    char _buf[17]; \
    snprintf(_buf, sizeof(_buf), (FMT), (VAL)); \
    (L).clear(); \
    (L).setCursor(0, 0); \
    (L).print(L1); \
    (L).setCursor(0, 1); \
    (L).print(_buf); \
  } while (0)

#define LCD_BOTH_PRINTF(L1, FMT, VAL) \
  do { \
    LCD_PRINTF(lcd1, (L1), (FMT), (VAL)); \
    LCD_PRINTF(lcd2, (L1), (FMT), (VAL)); \
  } while (0)

// ===== Config =====
// Tabuleiros
#define W 8
#define H 8
#define BRIGHTNESS 40
#define SERPENTINE false

#define PIN_CHAIN_A 8
#define PIN_CHAIN_B 9

#define NPER (W * H)
#define PANELS_PER_CHAIN 2
#define NPIX_CHAIN (NPER * PANELS_PER_CHAIN)

#define FRAME_MS 80

// Tamanho da frota
const uint8_t FLEET_SIZES[] = { 5, 4, 3, 3, 2 };
const uint8_t FLEET_COUNT = sizeof(FLEET_SIZES);

// ===== LEDs =====
Adafruit_NeoPixel chainA(NPIX_CHAIN, PIN_CHAIN_A, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel chainB(NPIX_CHAIN, PIN_CHAIN_B, NEO_GRB + NEO_KHZ800);

struct VPanel {
  Adafruit_NeoPixel *strip;
  uint16_t base;
};
VPanel VP[5];

static inline void ledsBegin() {
  chainA.begin();
  chainB.begin();
  chainA.setBrightness(BRIGHTNESS);
  chainB.setBrightness(BRIGHTNESS);
  chainA.show();  // clear
  chainB.show();  // clear

  // Panel 1 and 2 live on chain A
  VP[1] = { &chainA, 0 };     // panel 1 -> pixels [0..63]
  VP[2] = { &chainA, NPER };  // panel 2 -> pixels [64..127]

  // Panel 3 and 4 live on chain B
  VP[3] = { &chainB, 0 };     // panel 3 -> pixels [0..63]
  VP[4] = { &chainB, NPER };  // panel 4 -> pixels [64..127]
}

// Color helper: use any strip's Color() helper
#define C(r, g, b) chainA.Color((r), (g), (b))
#define COL_WATER1 C(0,25,60)
#define COL_SHIP C(200, 200, 0)
#define COL_HIT C(255,30,10)
#define COL_MISS C(0, 5, 20)
#define COL_CURSOR C(0, 180, 0)
#define COL_WIN C(0, 160, 0)
#define COL_LOSE C(200, 0, 0)

// ===== Dicionario =====
const char MSG_WAIT[] PROGMEM = "Aguarde";
const char MSG_FIRE[] PROGMEM = "Atire!";
const char MSG_GOODLUCK[] PROGMEM = "Boa Sorte!";
const char MSG_P1_TURN[] PROGMEM = "Vez: Jogador 1";
const char MSG_P2_TURN[] PROGMEM = "Vez: Jogador 2";
const char MSG_P1_SHOOT[] PROGMEM = "Jogador 1";
const char MSG_P2_SHOOT[] PROGMEM = "Jogador 2";

static inline void showTurn(uint8_t pid) {
  if (pid == 1) {
    LCD_MSG2(lcd1, FPSTR(MSG_FIRE), FPSTR(MSG_GOODLUCK));
    LCD_MSG2(lcd2, FPSTR(MSG_WAIT), FPSTR(MSG_P1_TURN));
  } else {
    LCD_MSG2(lcd2, FPSTR(MSG_FIRE), FPSTR(MSG_GOODLUCK));
    LCD_MSG2(lcd1, FPSTR(MSG_WAIT), FPSTR(MSG_P2_TURN));
  }
}

// ===== Tabuleiros =====
struct Cell {
  uint8_t ship : 1;
  uint8_t shot : 1;
  uint8_t hit : 1;
};
Cell b1[H][W];  // Jogador 1 (próprio)
Cell b2[H][W];  // Jogador 2 (próprio)
uint16_t remaining[3] = { 0, 0, 0 };

// ===== Estado =====
enum State {
  PLACE_1,
  PLACE_2,
  TURN_1,
  TURN_2,
  GAME_OVER
};
State state = PLACE_1;

uint8_t curX = 0, curY = 0;
bool horizontal = true;
uint8_t shipIndex = 0;

// winner/GO
int winnerId = 0;
bool gameOverDrawn = false;

// ===== Dirty flags por painel =====
bool dirtyP1 = true, dirtyP2 = true, dirtyP3 = true, dirtyP4 = true;
unsigned long nextFrameAt = 0;

// ===== Utils =====
static inline uint16_t idxXY(uint8_t x, uint8_t y) {
  if (SERPENTINE)
    return (y & 1) ? y * W + (W - 1 - x) : y * W + x;
  return (uint16_t)y * W + x;
}
static inline void setXY_on(uint8_t panel, uint8_t x, uint8_t y, uint32_t col) {
  VPanel &vp = VP[panel];
  vp.strip->setPixelColor(vp.base + idxXY(x, y), col);
}

static inline void clear_on(uint8_t panel) {
  VPanel &vp = VP[panel];
  for (uint16_t i = 0; i < NPER; ++i)
    vp.strip->setPixelColor(vp.base + i, 0);
}

const uint8_t BUZZER_PIN = 6;

volatile uint32_t lastShowAt = 0;

inline void ledSafeShow(Adafruit_NeoPixel *s) {
  noTone(BUZZER_PIN);
  delayMicroseconds(60);
  s->show();
  lastShowAt = millis();
}

// === replace your two show calls ===
static inline void show_on(uint8_t panel) {
  VPanel &vp = VP[panel];
  ledSafeShow(vp.strip);
}
static inline void fillPanel(uint8_t panel, uint32_t color) {
  VPanel &vp = VP[panel];
  for (uint16_t i = 0; i < NPER; ++i)
    vp.strip->setPixelColor(vp.base + i, color);
  ledSafeShow(vp.strip);
}

void markAllDirty() {
  dirtyP1 = dirtyP2 = dirtyP3 = dirtyP4 = true;
}

Cell (*boardPtr(int id))[W] {
  return (id == 1 ? b1 : b2);
}

bool canPlaceOn(int boardId, int x, int y, int len, bool horiz) {
  Cell(*B)[W] = boardPtr(boardId);

  if (horiz) {
    if (x < 0 || y < 0 || x + len > W || y >= H)
      return false;
  } else {
    if (x < 0 || y < 0 || x >= W || y + len > H)
      return false;
  }

  int x0, y0, x1, y1;
  if (horiz) {
    x0 = x - 1;
    y0 = y - 1;
    x1 = x + len;
    y1 = y + 1;
  } else {
    x0 = x - 1;
    y0 = y - 1;
    x1 = x + 1;
    y1 = y + len;
  }

  if (x0 < 0)
    x0 = 0;
  if (y0 < 0)
    y0 = 0;
  if (x1 > W - 1)
    x1 = W - 1;
  if (y1 > H - 1)
    y1 = H - 1;

  for (int yy = y0; yy <= y1; yy++) {
    for (int xx = x0; xx <= x1; xx++) {
      if (B[yy][xx].ship)
        return false;
    }
  }

  return true;
}

void placeShipOn(int boardId, int x, int y, int len, bool horiz) {
  Cell(*B)[W] = boardPtr(boardId);
  if (horiz)
    for (int i = 0; i < len; i++)
      B[y][x + i].ship = 1;
  else
    for (int i = 0; i < len; i++)
      B[y + i][x].ship = 1;
}
void clearBoards() {
  for (uint8_t y = 0; y < H; y++)
    for (uint8_t x = 0; x < W; x++) {
      b1[y][x] = { 0, 0, 0 };
      b2[y][x] = { 0, 0, 0 };
    }
}

// ===== Desenha tabuleiro =====
void drawChecker(uint8_t panel) {
  fillPanel(panel, COL_WATER1);
}
void drawShips(uint8_t panel, int myId, bool revealShips) {
  if (!revealShips)
    return;
  Cell(*B)[W] = boardPtr(myId);
  for (uint8_t y = 0; y < H; y++)
    for (uint8_t x = 0; x < W; x++)
      if (B[y][x].ship && !B[y][x].shot)
        setXY_on(panel, x, y, COL_SHIP);
}
void drawShots(uint8_t panel, int id) {
  Cell(*B)[W] = boardPtr(id);
  for (uint8_t y = 0; y < H; y++)
    for (uint8_t x = 0; x < W; x++)
      if (B[y][x].shot)
        setXY_on(panel, x, y, B[y][x].hit ? COL_HIT : COL_MISS);
}
void drawGhost(uint8_t panel, uint8_t x, uint8_t y, uint8_t len, bool horiz) {
  // Verifica se pode posicionar o navio aqui
  bool pode = false;
  if (panel == 1 && state == PLACE_1) {
    pode = canPlaceOn(1, x, y, len, horiz);
  } else if (panel == 3 && state == PLACE_2) {
    pode = canPlaceOn(2, x, y, len, horiz);
  } else {
    pode = true;  // fallback para outros usos
  }
  uint32_t cor = pode ? COL_CURSOR : COL_HIT;
  for (uint8_t k = 0; k < len; k++) {
    uint8_t gx = horiz ? x + k : x;
    uint8_t gy = horiz ? y : y + k;
    if (gx < W && gy < H)
      setXY_on(panel, gx, gy, cor);
  }
}
void drawCursor(uint8_t panel, uint8_t x, uint8_t y) {
  setXY_on(panel, x, y, COL_CURSOR);
}

// ===== Render condicional (só se “dirty” e respeitando FRAME_MS) =====
void renderIfDirty() {
  if (state == GAME_OVER)
    return;

  unsigned long now = millis();
  if (now < nextFrameAt)
    return;
  nextFrameAt = now + FRAME_MS;

  // ===== CHAIN A (panels 1 & 2) =====
  if (dirtyP1 || dirtyP2) {
    // Rebuild panel 1
    clear_on(1);
    drawChecker(1);
    drawShips(1, 1, true);
    drawShots(1, 1);
    if (state == PLACE_1) {
      uint8_t len = FLEET_SIZES[shipIndex];
      drawGhost(1, curX, curY, len, horizontal);
    }

    // Rebuild panel 2
    clear_on(2);
    drawChecker(2);
    drawShots(2, 2);
    if (state == TURN_1) {
      drawCursor(2, curX, curY);
    }

    // One atomic frame for the whole chain A
    ledSafeShow(&chainA);
    dirtyP1 = dirtyP2 = false;
  }

  // ===== CHAIN B (panels 3 & 4) =====
  if (dirtyP3 || dirtyP4) {
    // Rebuild panel 3
    clear_on(3);
    drawChecker(3);
    drawShips(3, 2, true);
    drawShots(3, 2);
    if (state == PLACE_2) {
      uint8_t len = FLEET_SIZES[shipIndex];
      drawGhost(3, curX, curY, len, horizontal);
    }

    // Rebuild panel 4
    clear_on(4);
    drawChecker(4);
    drawShots(4, 1);
    if (state == TURN_2) {
      drawCursor(4, curX, curY);
    }

    // One atomic frame for the whole chain B
    ledSafeShow(&chainB);
    dirtyP3 = dirtyP4 = false;
  }
}

// ===== Tela final =====
void renderGameOverNow() {
  int loserId = (winnerId == 1) ? 2 : 1;

  // painéis de tabuleiro próprio
  uint8_t panelWinnerOwn = (winnerId == 1) ? 1 : 3;
  uint8_t panelLoserOwn = (winnerId == 1) ? 3 : 1;

  // painéis de visão do inimigo
  uint8_t panelWinnerAtk = (winnerId == 1) ? 2 : 4;
  uint8_t panelLoserAtk = (winnerId == 1) ? 4 : 2;

  fillPanel(panelWinnerOwn, COL_WIN);
  fillPanel(panelLoserOwn, COL_LOSE);

  // opcional: pintar visões também
  fillPanel(panelWinnerAtk, COL_WIN);
  fillPanel(panelLoserAtk, COL_LOSE);
}

// ================== SFX ==================

// Notas (Hz)
const uint16_t C4 = 262;
const uint16_t D4 = 294;
const uint16_t E4 = 330;
const uint16_t F4 = 349;
const uint16_t G4 = 392;
const uint16_t LA4 = 440;
const uint16_t B4 = 494;

const uint16_t C5 = 523;
const uint16_t D5 = 587;
const uint16_t E5 = 659;
const uint16_t F5 = 698;
const uint16_t G5 = 784;
const uint16_t LA5 = 880;
const uint16_t B5 = 988;

const uint16_t C6 = 1047;

// ================== Helpers ==================
inline void beep(uint16_t freq, uint16_t durMs, uint16_t gapMs = 25) {
  if (durMs <= 40 && (millis() - lastShowAt) < 2) {
    if (gapMs)
      delay(gapMs);
    return;
  }

  if (freq > 0 && durMs > 0) {
    tone(BUZZER_PIN, freq, durMs);
    delay(durMs);
    noTone(BUZZER_PIN);
  }
  if (gapMs)
    delay(gapMs);
}

// "Spray" com jitter (para o MISS)
void splashSpray(uint16_t startHz, uint16_t endHz, uint16_t totalMs, uint8_t stepMs = 8) {
  uint32_t t0 = millis();
  while (true) {
    uint32_t t = millis() - t0;
    if (t >= totalMs)
      break;
    float prog = (float)t / (float)totalMs;  // 0..1
    float target = startHz + (endHz - startHz) * prog;
    int jitterRange = (int)(60 * (1.0f - prog)) + 10;  // 70->10
    int jitter = random(-jitterRange, jitterRange);
    int f = (int)target + jitter;
    if (f < 80)
      f = 80;
    tone(BUZZER_PIN, (unsigned int)f, stepMs);
    delay(stepMs);
  }
  noTone(BUZZER_PIN);
}

// ——— Helper de ritmo pontilhado (longa-curta)
inline void dotted(uint16_t noteLong, uint16_t noteShort,
                   uint16_t longMs = 100, uint16_t shortMs = 40, uint16_t gapMs = 10) {
  beep(noteLong, longMs, 6);
  beep(noteShort, shortMs, gapMs);
}

// Clique / Confirmação
void playClick() {
  beep(C5, 35, 15);
}

// Miss (água): spray/glitch + bloop curto
void playMiss() {
  splashSpray(1600, 420, 180, 8);
  beep(240, 70, 15);
  beep(190, 90, 25);
}

// Hit (acertou)
void playHit() {
  beep(C4, 70, 10);
  beep(E4, 80, 10);
  beep(G4, 100, 15);
}

// Sink (afundou) – FANFARE v3 (pontilhada + salto final)
void playSink() {
  // (sua Frase A pontilhada estava comentada no trecho enviado; mantive assim)
  // dotted(C5, E5, 100, 40, 12);
  // dotted(G5, C5, 100, 40, 12);
  // dotted(E5, G5, 100, 40, 16);

  // Frase B — cadência + SALTO final
  beep(B4, 55, 6);    // leading tone
  beep(C5, 100, 10);  // resolução
  dotted(E5, D5, 95, 38, 10);
  beep(G5, 180, 0);  // salto D5 → G5 (stinger)
}

// Game Over – Vitória (estendido)
void playGameOverWin() {
  beep(G4, 90, 10);
  beep(C5, 110, 10);
  beep(E5, 120, 12);
  beep(G5, 140, 40);
  beep(F5, 120, 15);
  beep(G5, 150, 20);
  beep(C6, 260, 40);
  beep(E5, 110, 10);
  beep(G5, 130, 10);
  beep(C6, 300, 60);
}

// Botões
#define BTN_1_UP 2
#define BTN_1_LEFT 3
#define BTN_1_DOWN 4
#define BTN_1_RIGHT 5

#define BTN_2_UP 12
#define BTN_2_LEFT 11
#define BTN_2_DOWN 10
#define BTN_2_RIGHT 13

#define BTN_1_ROTATE A0
#define BTN_1_OK A1

#define BTN_2_OK A2
#define BTN_2_ROTATE A3

struct Btn {
  uint8_t pin;
  bool last = HIGH;
  uint32_t lastChange = 0;

  bool isDown = false;
  uint32_t pressedAt = 0, lastRepeat = 0;

  static constexpr uint16_t debounceMs = 25;
  static constexpr uint16_t repeatDelay = 300;
  static constexpr uint16_t repeatRate = 80;

  Btn(uint8_t p)
    : pin(p) {}

  void begin() {
    pinMode(pin, INPUT_PULLUP);
    last = digitalRead(pin);
  }

  bool fellRaw() {
    uint32_t t = millis();
    bool now = digitalRead(pin);
    if (now != last && (t - lastChange) > debounceMs) {
      lastChange = t;
      last = now;
      if (now == LOW) {
        isDown = true;
        pressedAt = lastRepeat = t;
        return true;
      }
      isDown = false;
    }
    return false;
  }
  bool fellOrRepeat() {
    if (fellRaw())
      return true;
    if (isDown) {
      uint32_t t = millis();
      if (t - pressedAt >= repeatDelay && t - lastRepeat >= repeatRate) {
        lastRepeat = t;
        return true;
      }
    }
    return false;
  }
};

Btn bUp1{ BTN_1_UP }, bDown1{ BTN_1_DOWN }, bLeft1{ BTN_1_LEFT }, bRight1{ BTN_1_RIGHT }, bRot1{ BTN_1_ROTATE }, bOk1{ BTN_1_OK };
Btn bUp2{ BTN_2_UP }, bDown2{ BTN_2_DOWN }, bLeft2{ BTN_2_LEFT }, bRight2{ BTN_2_RIGHT }, bRot2{ BTN_2_ROTATE }, bOk2{ BTN_2_OK };

void moveCursor(int dx, int dy) {
  int nx = (int)curX + dx, ny = (int)curY + dy;
  if (nx < 0)
    nx = 0;
  if (ny < 0)
    ny = 0;
  if (nx >= W)
    nx = W - 1;
  if (ny >= H)
    ny = H - 1;
  curX = (uint8_t)nx;
  curY = (uint8_t)ny;
  if (state == PLACE_1)
    dirtyP1 = true;
  else if (state == PLACE_2)
    dirtyP3 = true;
  else if (state == TURN_1)
    dirtyP2 = true;
  else if (state == TURN_2)
    dirtyP4 = true;
}

// Lida com o conjunto de botões de um jogador (1 ou 2)
void handleButtonsForPlayer(uint8_t player) {
  Btn &bUp = (player == 1) ? bUp1 : bUp2;
  Btn &bDown = (player == 1) ? bDown1 : bDown2;
  Btn &bLeft = (player == 1) ? bLeft1 : bLeft2;
  Btn &bRight = (player == 1) ? bRight1 : bRight2;
  Btn &bRot = (player == 1) ? bRot1 : bRot2;
  Btn &bOk = (player == 1) ? bOk1 : bOk2;

  if (bUp.fellOrRepeat()) {
    playClick();
    moveCursor(0, -1);
  }
  if (bDown.fellOrRepeat()) {
    playClick();
    moveCursor(0, 1);
  }
  if (bLeft.fellOrRepeat()) {
    playClick();
    moveCursor(-1, 0);
  }
  if (bRight.fellOrRepeat()) {
    playClick();
    moveCursor(1, 0);
  }

  if (bRot.fellRaw()) {
    playClick();
    horizontal = !horizontal;
    if (state == PLACE_1)
      dirtyP1 = true;
    else if (state == PLACE_2)
      dirtyP3 = true;
  }

  if (bOk.fellRaw()) {
    playClick();
    if (state == PLACE_1 && player == 1)
      tryConfirmPlacement(1);
    else if (state == PLACE_2 && player == 2)
      tryConfirmPlacement(2);
    else if (state == TURN_1 && player == 1)
      tryShootAt(2);
    else if (state == TURN_2 && player == 2)
      tryShootAt(1);
  }
}

// ===== Nomes (máx 9 + '\0') =====
char name1[10] = "JOGADOR 1";
char name2[10] = "JOGADOR 2";

// Helper p/ pegar nome por id (1 ou 2)
const char *PNAME(int id) {
  return (id == 1) ? name1 : name2;
}

// ===== Estatísticas p/ ranking =====
uint16_t shotsBy[3] = { 0, 0, 0 };
uint16_t hitsBy[3] = { 0, 0, 0 };
uint16_t sunkShipsBy[3] = { 0, 0, 0 };
uint16_t sunkCellsBy[3] = { 0, 0, 0 };  // soma dos comprimentos dos navios afundados

// UI de nomes: conjunto de caracteres aceitos
const char CHARSET[] PROGMEM =
  "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_ ";
static inline char charsetAt(int i) {
  return (char)pgm_read_byte(&CHARSET[i]);
}
const int NCH = sizeof(CHARSET) - 1;

void enterNameForPlayer(uint8_t player, char *dest, uint8_t maxLen) {
  Btn &bUp = (player == 1) ? bUp1 : bUp2;
  Btn &bDown = (player == 1) ? bDown1 : bDown2;
  Btn &bLeft = (player == 1) ? bLeft1 : bLeft2;
  Btn &bRight = (player == 1) ? bRight1 : bRight2;
  Btn &bRot = (player == 1) ? bRot1 : bRot2;
  Btn &bOk = (player == 1) ? bOk1 : bOk2;
  LiquidCrystal_I2C &lcd = (player == 1) ? lcd1 : lcd2;

  uint8_t len = 0;
  dest[0] = '\0';
  int idx = 0;

  bool first = true;

  auto drawName = [&]() {
    if (first) {
      first = false;
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print(F("Nome P"));
      lcd.print((int)player);
      lcd.print(F(" (9):"));
    }

    lcd.setCursor(0, 1);
    lcd.print(dest);
    lcd.print('>');
    lcd.print(charsetAt(idx));

    int used = (int)strlen(dest) + 2;
    for (int i = used; i < 16; ++i)
      lcd.print(' ');
  };

  drawName();

  while (true) {
    bool changed = false;

    if (bUp.fellOrRepeat()) {
      playClick();
      idx = (idx + 1) % NCH;
      changed = true;
    }
    if (bDown.fellOrRepeat()) {
      playClick();
      idx = (idx - 1 + NCH) % NCH;
      changed = true;
    }

    if (bRight.fellOrRepeat() && len < maxLen) {
      playClick();
      dest[len++] = charsetAt(idx);
      dest[len] = '\0';
      changed = true;
    }

    if (bLeft.fellOrRepeat() || bRot.fellRaw()) {
      playClick();
      if (len > 0) {
        dest[--len] = '\0';
        changed = true;
      }
    }

    if (bOk.fellRaw()) {
      playHit();
      while (len > 0 && dest[len - 1] == ' ')
        dest[--len] = '\0';

      if (len == 0) {
        char c = charsetAt(idx);
        if (c == ' ') {
          dest[0] = 'P';
          dest[1] = (player == 1) ? '1' : '2';
          dest[2] = '\0';
        } else {
          dest[0] = c;
          dest[1] = '\0';
        }
      }
      break;
    }

    if (changed)
      drawName();
    delay(20);
  }
}

// ---------- TELEMETRIA ----------
unsigned long matchStartMs = 0;

static void telem_player_names() {
  Serial.print(F("PN,"));
  Serial.print(name1);
  Serial.print(',');
  Serial.println(name2);
}

static void telem_game_start() {
  Serial.print(F("GS,"));
  Serial.println(matchStartMs);
}

static void telem_place_ship(int player, int x, int y, int len, bool horiz) {
  Serial.print(F("PS,"));
  Serial.print(player);
  Serial.print(',');
  Serial.print(x);
  Serial.print(',');
  Serial.print(y);
  Serial.print(',');
  Serial.print(len);
  Serial.print(',');
  Serial.print(horiz ? 1 : 0);
  Serial.print(',');
  Serial.println(millis());
}

static int calcScore(int pid, int winnerId, unsigned long durationMs) {
  int win = (pid == winnerId) ? 1 : 0;
  uint16_t hits = hitsBy[pid];
  uint16_t shots = shotsBy[pid];
  uint16_t sunkCells = sunkCellsBy[pid];
  uint16_t sunkShips = sunkShipsBy[pid];

  uint16_t accBonus = 0;
  if (shots > 0) {
    uint16_t num = (uint16_t)(hits * 50u);
    accBonus = (uint16_t)((num + (shots >> 1)) / shots);
  }

  int base = 200 * win + 10 * hits + 5 * sunkCells + 15 * sunkShips + accBonus;

  uint16_t secs = 0;
  unsigned long ms = durationMs;
  while (ms >= 1000UL && secs < 180u) {
    ms -= 1000UL;
    secs++;
  }

  int timeBonus = 0;
  if (secs < 180u) {
    uint16_t t = secs;
    while ((uint16_t)(t + 6u) <= 180u) {
      t += 6u;
      timeBonus++;
    }
  }

  return base + timeBonus;
}

static void telem_shot(int attacker, int defender, int x, int y, bool hit, bool sunk, int remaining_def) {
  Serial.print(F("SH,"));
  Serial.print(attacker);
  Serial.print(',');
  Serial.print(defender);
  Serial.print(',');
  Serial.print(x);
  Serial.print(',');
  Serial.print(y);
  Serial.print(',');
  Serial.print(hit ? 1 : 0);
  Serial.print(',');
  Serial.print(sunk ? 1 : 0);
  Serial.print(',');
  Serial.print(remaining_def);
  Serial.print(',');
  Serial.println(millis());
}

static void telem_game_end(int winner) {
  unsigned long dur = millis() - matchStartMs;
  int s1 = calcScore(1, winner, dur);
  int s2 = calcScore(2, winner, dur);

  Serial.print(F("GE,"));
  Serial.print(winner);
  Serial.print(',');
  Serial.print(dur);
  Serial.print(',');

  // P1: name,shots,hits,sunkCells,score
  Serial.print(name1);
  Serial.print(',');
  Serial.print(shotsBy[1]);
  Serial.print(',');
  Serial.print(hitsBy[1]);
  Serial.print(',');
  Serial.print(sunkCellsBy[1]);
  Serial.print(',');
  Serial.print(s1);
  Serial.print(',');

  // P2
  Serial.print(name2);
  Serial.print(',');
  Serial.print(shotsBy[2]);
  Serial.print(',');
  Serial.print(hitsBy[2]);
  Serial.print(',');
  Serial.print(sunkCellsBy[2]);
  Serial.print(',');
  Serial.print(s2);
  Serial.print(',');

  Serial.println(millis());
}

// ---------- /TELEMETRIA ----------

void resetStats() {
  shotsBy[1] = shotsBy[2] = 0;
  hitsBy[1] = hitsBy[2] = 0;
  sunkShipsBy[1] = sunkShipsBy[2] = 0;
  sunkCellsBy[1] = sunkCellsBy[2] = 0;
}

// ===== Jogo =====
void nextStateAfterPlacement() {
  shipIndex = 0;
  curX = 0;
  curY = 0;
  horizontal = true;
  if (state == PLACE_1) {
    state = PLACE_2;
    LCD_MSG2(lcd1, F("Aguarde"), F("Vez: Jogador 2"));
    LCD_MSG2(lcd2, F("Jogador 2:"), F("posicione navios"));
  } else {
    state = TURN_1;
    resetStats();
    matchStartMs = millis();
    telem_game_start();
    showTurn(1);
  }
  markAllDirty();
}

void tryConfirmPlacement(int myId) {
  uint8_t len = FLEET_SIZES[shipIndex];
  if (canPlaceOn(myId, curX, curY, len, horizontal)) {
    placeShipOn(myId, curX, curY, len, horizontal);
    telem_place_ship(myId, curX, curY, len, horizontal);
    remaining[myId] += len;
    shipIndex++;
    if (myId == 1)
      dirtyP1 = true;
    else
      dirtyP3 = true;
    if (shipIndex >= FLEET_COUNT) {
      nextStateAfterPlacement();
    }
  } else {
    if (myId == 1) {
      playMiss();
      LCD_MSG(lcd1, F("Posicao invalida."));
    } else {
      playMiss();
      LCD_MSG(lcd2, F("Posicao invalida."));
    }
  }
}

void tryShootAt(int enemyId) {
  Cell(*E)[W] = boardPtr(enemyId);
  Cell &c = E[curY][curX];
  if (c.shot) {
    if (enemyId == 2) {
      playMiss();
      LCD_MSG(lcd1, F("Ja atirou aqui."));
    } else {
      playMiss();
      LCD_MSG(lcd2, F("Ja atirou aqui."));
    }
    return;
  }
  c.shot = 1;
  int attacker = (enemyId == 2) ? 1 : 2;
  shotsBy[attacker]++;
  bool afundou = false;
  if (c.ship) {
    c.hit = 1;
    remaining[enemyId]--;
    hitsBy[attacker]++;
    // Descobre o tamanho e orientação do navio atingido
    int x0 = curX, y0 = curY, x1 = curX, y1 = curY;
    // Checa horizontalmente
    while (x0 > 0 && E[y0][x0 - 1].ship)
      x0--;
    while (x1 < W - 1 && E[y0][x1 + 1].ship)
      x1++;
    int navio_tam_h = x1 - x0 + 1;
    // Checa verticalmente
    int y0v = curY, y1v = curY;
    while (y0v > 0 && E[y0v - 1][curX].ship)
      y0v--;
    while (y1v < H - 1 && E[y1v + 1][curX].ship)
      y1v++;
    int navio_tam_v = y1v - y0v + 1;
    // Decide se é horizontal, vertical ou tamanho 1
    if (navio_tam_h > 1) {
      // Checa se todas as partes do navio horizontal estão com hit=1
      afundou = true;
      for (int k = x0; k <= x1; k++) {
        if (E[y0][k].ship && !E[y0][k].hit) {
          afundou = false;
          break;
        }
      }
    } else if (navio_tam_v > 1) {
      // Checa se todas as partes do navio vertical estão com hit=1
      afundou = true;
      for (int k = y0v; k <= y1v; k++) {
        if (E[k][curX].ship && !E[k][curX].hit) {
          afundou = false;
          break;
        }
      }
    } else {
      // Navio de tamanho 1
      afundou = true;
    }
    int ship_len = 1;
    if (navio_tam_h > 1)
      ship_len = navio_tam_h;
    else if (navio_tam_v > 1)
      ship_len = navio_tam_v;

    if (afundou) {
      sunkShipsBy[attacker]++;
      sunkCellsBy[attacker] += ship_len;
    }
    telem_shot(attacker, enemyId, curX, curY, true, afundou, remaining[enemyId]);
    if (afundou) {
      playSink();
      LCD_BOTH_MSG(F("Acertou!"), F("Afundou o navio!"));
    } else {
      playHit();
      LCD_BOTH_MSG(F("Acertou!"), F("Parte do navio."));
    }
  } else {
    playMiss();
    telem_shot(attacker, enemyId, curX, curY, /*hit=*/false, /*sunk=*/false, remaining[enemyId]);
    LCD_BOTH_MSG(F("Errou..."), F(""));
  }

  // marcar paineis relevantes como sujos:
  if (enemyId == 2) {
    dirtyP2 = true;
    dirtyP3 = true;
  }  // J1 atirou no J2
  else {
    dirtyP4 = true;
    dirtyP1 = true;
  }  // J2 atirou no J1

  // Mensagem de vez do outro jogador
  if (remaining[enemyId] == 0) {
    state = GAME_OVER;
    winnerId = (enemyId == 2) ? 1 : 2;
    gameOverDrawn = false;
    telem_game_end(winnerId);
    return;
  }

  // Aguarda um pouco para mostrar o resultado antes de trocar a vez
  delay(1200);
  if (state == TURN_1) {
    showTurn(2);
    state = TURN_2;
    dirtyP4 = true;
  } else if (state == TURN_2) {
    showTurn(1);
    state = TURN_1;
    dirtyP2 = true;
  }
}

void renderAllNow() {
  clear_on(1);
  drawChecker(1);
  drawShips(1, 1, true);
  drawShots(1, 1);
  show_on(1);
  clear_on(2);
  drawChecker(2);
  drawShots(2, 2);
  show_on(2);
  clear_on(3);
  drawChecker(3);
  drawShips(3, 2, true);
  drawShots(3, 2);
  show_on(3);
  clear_on(4);
  drawChecker(4);
  drawShots(4, 1);
  show_on(4);
}

void setup() {
  Serial.begin(115200);
  while (!Serial) {
  }

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  lcd1.init();
  lcd1.backlight();
  lcd2.init();
  lcd2.backlight();

  LCD_BOTH_MSG(F("BATALHA NAVAL"), F("Aguarde..."));
  delay(1000);

  LCD_BOTH_MSG(F("Iniciando Botoes"), F(""));

  bUp1.begin();
  bDown1.begin();
  bLeft1.begin();
  bRight1.begin();
  bRot1.begin();
  bOk1.begin();

  bUp2.begin();
  bDown2.begin();
  bLeft2.begin();
  bRight2.begin();
  bRot2.begin();
  bOk2.begin();

  LCD_BOTH_MSG(F("Iniciando LEDS"), F(""));

  ledsBegin();

  LCD_BOTH_MSG(F("Preenchendo"), F("tabuleiros..."));

  clearBoards();
  delay(300);
  renderAllNow();
  delay(120);
  renderAllNow();
  delay(120);

  // 1) Nome do Jogador 1
  LCD_MSG2(lcd1, F("Escolha seu"), F("nome (OK p/ fim)"));
  delay(500);
  LCD_MSG2(lcd2, F("Aguarde..."), F(""));
  enterNameForPlayer(1, name1, 9);

  // 2) Nome do Jogador 2
  LCD_MSG2(lcd2, F("Escolha seu"), F("nome (OK p/ fim)"));
  delay(500);
  LCD_MSG2(lcd1, F("Aguarde..."), F(""));
  enterNameForPlayer(2, name2, 9);

  telem_player_names();

  LCD_MSG2(lcd1, F("Jogador 1:"), F("posicione navios"));
  LCD_MSG2(lcd2, F("Aguarde"), F("Vez: Jogador 1"));
  delay(1500);

  markAllDirty();
}

void loop() {
  if (state == PLACE_1 || state == TURN_1)
    handleButtonsForPlayer(1);
  else if (state == PLACE_2 || state == TURN_2)
    handleButtonsForPlayer(2);

  if (state == GAME_OVER) {
    if (!gameOverDrawn) {
      playGameOverWin();
      renderGameOverNow();
      gameOverDrawn = true;
      if (winnerId == 1) {
        LCD_BOTH_MSG(F("GAME OVER!"), F("Jogador 1 venceu"));
      } else {
        LCD_BOTH_MSG(F("GAME OVER!"), F("Jogador 2 venceu"));
      }
    }
    return;
  }

  renderIfDirty();
}