#include <Adafruit_NeoPixel.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>   // instale a biblioteca LiquidCrystal_I2C

LiquidCrystal_I2C lcd1(0x20, 16, 2); // troque 0x27 por 0x3F se necessário
LiquidCrystal_I2C lcd2(0x21, 16, 2); // troque 0x27 por 0x3F se necessário

// -------- Macros para imprimir no LCD sem gastar RAM --------
#define LCD_MSG(L, L1) do { \
  (L).clear(); (L).setCursor(0,0); (L).print(L1); \
} while(0)

#define LCD_MSG2(L, L1, L2) do { \
  (L).clear(); \
  (L).setCursor(0,0); (L).print(L1); \
  (L).setCursor(0,1); (L).print(L2); \
} while(0)

#define LCD_BOTH_MSG(L1, L2) do { \
  LCD_MSG2(lcd1, (L1), (L2)); \
  LCD_MSG2(lcd2, (L1), (L2)); \
} while(0)

// printf simples (2ª linha). OBS: FMT deve ser string normal (em RAM).
#define LCD_PRINTF(L, L1, FMT, VAL) do { \
  char _buf[17]; snprintf(_buf, sizeof(_buf), (FMT), (long)(VAL)); \
  (L).clear(); \
  (L).setCursor(0,0); (L).print(L1); \
  (L).setCursor(0,1); (L).print(_buf); \
} while(0)

#define LCD_BOTH_PRINTF(L1, FMT, VAL) do { \
  LCD_PRINTF(lcd1, (L1), (FMT), (VAL)); \
  LCD_PRINTF(lcd2, (L1), (FMT), (VAL)); \
} while(0)

// ===== Config =====
// Tabuleiros
#define PIN_P1 6   // J1 - próprio
#define PIN_P2 7   // J1 - visão inimigo
#define PIN_P3 8   // J2 - próprio
#define PIN_P4 9   // J2 - visão inimigo
#define W 8
#define H 8
#define BRIGHTNESS 40
#define SERPENTINE false

// Limite de FPS no Tinkercad (ms por frame)
#define FRAME_MS 80  // ~12.5 fps


// Tamanho da frota
const uint8_t FLEET_SIZES[] = {2,1};
const uint8_t FLEET_COUNT = sizeof(FLEET_SIZES);

// ===== LEDs =====
Adafruit_NeoPixel p1(W*H, PIN_P1, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel p2(W*H, PIN_P2, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel p3(W*H, PIN_P3, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel p4(W*H, PIN_P4, NEO_GRB + NEO_KHZ800);

// Paleta
#define C(r,g,b) p1.Color((r),(g),(b))
#define COL_WATER1  C(0,0,12)
#define COL_WATER2  C(0,0,25)
#define COL_SHIP    C(40,40,40)
#define COL_HIT     C(200,0,0)    // vermelho (acerto)
#define COL_MISS    C(80,80,80)   // cinza (erro)  << mudou aqui
#define COL_CURSOR  C(0,180,0)
#define COL_GHOST   C(200,200,0)
#define COL_WIN     C(0,160,0)    // verde vencedor
#define COL_LOSE    C(200,0,0)    // vermelho perdedor

// ===== Tabuleiros =====
struct Cell { uint8_t ship:1; uint8_t shot:1; uint8_t hit:1; };
Cell b1[H][W];   // Jogador 1 (próprio)
Cell b2[H][W];   // Jogador 2 (próprio)
uint16_t remaining[3] = {0,0,0};

// ===== Estado =====
enum State { PLACE_1, PLACE_2, TURN_1, TURN_2, GAME_OVER };
State state = PLACE_1;

uint8_t curX=0, curY=0;
bool horizontal = true;
uint8_t shipIndex = 0;

// winner/GO
int winnerId = 0;
bool gameOverDrawn = false;

// ===== Dirty flags por painel =====
bool dirtyP1=true, dirtyP2=true, dirtyP3=true, dirtyP4=true;
unsigned long nextFrameAt=0;

// ===== Utils =====
uint16_t idxXY(uint8_t x, uint8_t y) {
  if (SERPENTINE) return (y%2==0)? y*W + x : y*W + (W-1-x);
  return y*W + x;
}
void setXY_on(uint8_t panel, uint8_t x, uint8_t y, uint32_t col) {
  uint16_t i = idxXY(x,y);
  if (panel==1) p1.setPixelColor(i, col);
  else if (panel==2) p2.setPixelColor(i, col);
  else if (panel==3) p3.setPixelColor(i, col);
  else if (panel==4) p4.setPixelColor(i, col);
}
void clear_on(uint8_t panel){
  if (panel==1) p1.clear();
  else if (panel==2) p2.clear();
  else if (panel==3) p3.clear();
  else if (panel==4) p4.clear();
}
void show_on(uint8_t panel){
  if (panel==1) p1.show();
  else if (panel==2) p2.show();
  else if (panel==3) p3.show();
  else if (panel==4) p4.show();
}
void markAllDirty(){ dirtyP1=dirtyP2=dirtyP3=dirtyP4=true; }

Cell (*boardPtr(int id))[W] { return (id==1 ? b1 : b2); }

bool canPlaceOn(int boardId, int x, int y, int len, bool horiz) {
  Cell (*B)[W] = boardPtr(boardId);
  if (horiz) {
    if (x + len > W) return false;
    for (int i=0; i<len; i++) if (B[y][x+i].ship) return false;
  } else {
    if (y + len > H) return false;
    for (int i=0; i<len; i++) if (B[y+i][x].ship) return false;
  }
  return true;
}
void placeShipOn(int boardId, int x, int y, int len, bool horiz) {
  Cell (*B)[W] = boardPtr(boardId);
  if (horiz) for (int i=0;i<len;i++) B[y][x+i].ship = 1;
  else       for (int i=0;i<len;i++) B[y+i][x].ship = 1;
}
void clearBoards() {
  for (uint8_t y=0;y<H;y++) for (uint8_t x=0;x<W;x++) {
    b1[y][x] = {0,0,0};
    b2[y][x] = {0,0,0};
  }
}

// ===== Render de um painel =====
void drawChecker(uint8_t panel){
  for (uint8_t y=0;y<H;y++)
  for (uint8_t x=0;x<W;x++)
  setXY_on(panel, x,y, ((x+y)&1)? COL_WATER2 : COL_WATER1);
}
void drawShips(uint8_t panel, int myId, bool revealShips){
  if (!revealShips) return;
  Cell (*B)[W] = boardPtr(myId);
  for (uint8_t y=0;y<H;y++)
  for (uint8_t x=0;x<W;x++)
  if (B[y][x].ship && !B[y][x].shot) setXY_on(panel, x,y, COL_SHIP);
}
void drawShots(uint8_t panel, int id){
  Cell (*B)[W] = boardPtr(id);
  for (uint8_t y=0;y<H;y++)
  for (uint8_t x=0;x<W;x++)
  if (B[y][x].shot) setXY_on(panel, x,y, B[y][x].hit? COL_HIT : COL_MISS);
}
void drawGhost(uint8_t panel, uint8_t x,uint8_t y,uint8_t len,bool horiz){
  for (uint8_t k=0;k<len;k++){
    uint8_t gx = horiz? x+k : x;
    uint8_t gy = horiz? y   : y+k;
    if (gx<W && gy<H) setXY_on(panel, gx,gy, COL_GHOST);
  }
}
void drawCursor(uint8_t panel, uint8_t x,uint8_t y){ setXY_on(panel, x,y, COL_CURSOR); }

void fillPanel(uint8_t panel, uint32_t color){
  if (panel==1) p1.fill(color);
  else if (panel==2) p2.fill(color);
  else if (panel==3) p3.fill(color);
  else if (panel==4) p4.fill(color);
  show_on(panel);
}

// ===== Render condicional (só se “dirty” e respeitando FRAME_MS) =====
void renderIfDirty(){
  if (state==GAME_OVER) return; // não redesenhar tela normal após fim de jogo
  
  unsigned long now = millis();
  if (now < nextFrameAt) return;     // throttling global
  nextFrameAt = now + FRAME_MS;
  
  if (dirtyP1) {
    clear_on(1); drawChecker(1);
    drawShips(1, 1, true);
    drawShots(1, 1);
    if (state==PLACE_1) { uint8_t len=FLEET_SIZES[shipIndex]; drawGhost(1,curX,curY,len,horizontal); drawCursor(1,curX,curY); }
    show_on(1); dirtyP1=false;
  }
  if (dirtyP2) {
    clear_on(2); drawChecker(2);
    drawShots(2, 2);
    if (state==TURN_1) { drawCursor(2,curX,curY); }
    show_on(2); dirtyP2=false;
  }
  if (dirtyP3) {
    clear_on(3); drawChecker(3);
    drawShips(3, 2, true);
    drawShots(3, 2);
    if (state==PLACE_2) { uint8_t len=FLEET_SIZES[shipIndex]; drawGhost(3,curX,curY,len,horizontal); drawCursor(3,curX,curY); }
    show_on(3); dirtyP3=false;
  }
  if (dirtyP4) {
    clear_on(4); drawChecker(4);
    drawShots(4, 1);
    if (state==TURN_2) { drawCursor(4,curX,curY); }
    show_on(4); dirtyP4=false;
  }
}

// ===== NOVO: tela final =====
void renderGameOverNow(){
  int loserId = (winnerId==1)? 2 : 1;
  
  // painéis de tabuleiro próprio
  uint8_t panelWinnerOwn = (winnerId==1)? 1 : 3;
  uint8_t panelLoserOwn  = (winnerId==1)? 3 : 1;
  
  // painéis de visão do inimigo
  uint8_t panelWinnerAtk = (winnerId==1)? 2 : 4;
  uint8_t panelLoserAtk  = (winnerId==1)? 4 : 2;
  
  fillPanel(panelWinnerOwn, COL_WIN);
  fillPanel(panelLoserOwn,  COL_LOSE);
  
  // opcional: pintar visões também
  fillPanel(panelWinnerAtk, COL_WIN);
  fillPanel(panelLoserAtk,  COL_LOSE);
}

// Botões
#define BTN_1_UP     2
#define BTN_1_LEFT   3
#define BTN_1_DOWN   4
#define BTN_1_RIGHT  5

#define BTN_2_UP     10
#define BTN_2_LEFT   11
#define BTN_2_DOWN   12
#define BTN_2_RIGHT  13

#define BTN_1_ROTATE A0
#define BTN_1_OK     A1

#define BTN_2_ROTATE A2
#define BTN_2_OK     A3

struct Btn {
  uint8_t pin;
  bool last = HIGH;
  unsigned long lastChange = 0;
  const unsigned long debounceMs = 25;

  // repeat
  bool isDown = false;
  unsigned long pressedAt = 0, lastRepeat = 0;
  const unsigned long repeatDelay = 300;
  const unsigned long repeatRate  = 80;

  void begin(){ pinMode(pin, INPUT_PULLUP); last = digitalRead(pin); }

  bool fellRaw() {
    bool now = digitalRead(pin);
    auto t = millis();
    if (now != last && (t - lastChange) > debounceMs) {
      lastChange = t; last = now;
      if (now == LOW) { isDown = true; pressedAt = t; lastRepeat = t; return true; }
      isDown = false;
    }
    return false;
  }

  bool fellOrRepeat() {
    if (fellRaw()) return true;
    if (isDown) {
      auto t = millis();
      if (t - pressedAt >= repeatDelay && t - lastRepeat >= repeatRate) {
        lastRepeat = t; return true;
      }
    }
    return false;
  }
};


Btn bUp1{BTN_1_UP}, bDown1{BTN_1_DOWN}, bLeft1{BTN_1_LEFT}, bRight1{BTN_1_RIGHT}, bRot1{BTN_1_ROTATE}, bOk1{BTN_1_OK};
Btn bUp2{BTN_2_UP}, bDown2{BTN_2_DOWN}, bLeft2{BTN_2_LEFT}, bRight2{BTN_2_RIGHT}, bRot2{BTN_2_ROTATE}, bOk2{BTN_2_OK};

void moveCursor(int dx,int dy){
  int nx = (int)curX + dx, ny = (int)curY + dy;
  if (nx<0) nx=0; if (ny<0) ny=0;
  if (nx>=W) nx=W-1; if (ny>=H) ny=H-1;
  curX = (uint8_t)nx; curY = (uint8_t)ny;
  if (state==PLACE_1) dirtyP1=true;
  else if (state==PLACE_2) dirtyP3=true;
  else if (state==TURN_1) dirtyP2=true;
  else if (state==TURN_2) dirtyP4=true;
}

// Lida com o conjunto de botões de um jogador (1 ou 2)
void handleButtonsForPlayer(uint8_t player){
  Btn &bUp    = (player==1)? bUp1   : bUp2;
  Btn &bDown  = (player==1)? bDown1 : bDown2;
  Btn &bLeft  = (player==1)? bLeft1 : bLeft2;
  Btn &bRight = (player==1)? bRight1: bRight2;
  Btn &bRot   = (player==1)? bRot1  : bRot2;
  Btn &bOk    = (player==1)? bOk1   : bOk2;

  if (bUp.fellOrRepeat())   { 
    moveCursor(0,-1);
  }
  if (bDown.fellOrRepeat())  { 
    moveCursor(0, 1);
  }
  if (bLeft.fellOrRepeat())  { 
    moveCursor(-1,0);
  }
  if (bRight.fellOrRepeat()) { 
    moveCursor(1, 0);
  }

  if (bRot.fellRaw()) {
    horizontal = !horizontal;
    if (state==PLACE_1) dirtyP1 = true;
    else if (state==PLACE_2) dirtyP3 = true;
  }

  if (bOk.fellRaw()) {
    if (state==PLACE_1 && player==1)      tryConfirmPlacement(1);
    else if (state==PLACE_2 && player==2) tryConfirmPlacement(2);
    else if (state==TURN_1  && player==1) tryShootAt(2);
    else if (state==TURN_2  && player==2) tryShootAt(1);
  }
}

// Aceita apenas C-strings (sem F())
// ...funções antigas de LCD removidas...

// ===== Jogo =====
void nextStateAfterPlacement(){
  shipIndex = 0; curX=0; curY=0; horizontal=true;
  if (state == PLACE_1) {
    state = PLACE_2;
    LCD_MSG2(lcd1, F("Aguarde"), F("Vez do Jogador 2"));
    LCD_MSG2(lcd2, F("Jogador 2"), F("posicione navios"));
  } else {
    state = TURN_1;
    LCD_MSG2(lcd1, F("Sua vez de atirar"), F("Boa sorte!"));
    LCD_MSG2(lcd2, F("Aguarde"), F("Jogador 1 Atirar"));
  }
  markAllDirty();
}

void tryConfirmPlacement(int myId){
  uint8_t len = FLEET_SIZES[shipIndex];
  if (canPlaceOn(myId, curX, curY, len, horizontal)) {
    placeShipOn(myId, curX, curY, len, horizontal);
    remaining[myId] += len;
    shipIndex++;
    if (myId==1) dirtyP1=true; else dirtyP3=true;
    if (shipIndex >= FLEET_COUNT){
      nextStateAfterPlacement();
    } else {
      // Serial.print(F("Navio colocado. Proximo tamanho: "));
      // Serial.println(FLEET_SIZES[shipIndex]);
      if (myId==1) LCD_PRINTF(lcd1, F("Tam. navio:"), "%d", FLEET_SIZES[shipIndex]);
      else        LCD_PRINTF(lcd2, F("Tam. navio:"), "%d", FLEET_SIZES[shipIndex]);
    }
  } else {
    // Serial.println(F("Posicao invalida."));
    if (myId==1) LCD_MSG(lcd1, F("Posicao invalida."));
    else        LCD_MSG(lcd2, F("Posicao invalida."));
  }
}

void tryShootAt(int enemyId){
  Cell (*E)[W] = boardPtr(enemyId);
  Cell &c = E[curY][curX];
  if (c.shot) { 
    if (enemyId==2) LCD_MSG(lcd1, F("Ja atirou aqui."));
    else            LCD_MSG(lcd2, F("Ja atirou aqui."));
    return; 
  }
  c.shot = 1;
  if (c.ship) { 
    c.hit = 1; remaining[enemyId]--; 
    LCD_BOTH_MSG(F("Acertou!"));
  }
  else        { 
    LCD_BOTH_MSG(F("Errou..."));
  }

  // marcar paineis relevantes como sujos:
  if (enemyId==2){ dirtyP2 = true; dirtyP3 = true; }  // J1 atirou no J2
  else            { dirtyP4 = true; dirtyP1 = true; }  // J2 atirou no J1

  if (remaining[enemyId] == 0) {
    state = GAME_OVER;
    winnerId = (enemyId==2)? 1 : 2;   // quem atirou agora venceu
    gameOverDrawn = false;            // vamos desenhar na próxima iteração
    return;
  }
  if (state == TURN_1) state = TURN_2; else if (state == TURN_2) state = TURN_1;
  if (state==TURN_1) { dirtyP2=true; } else { dirtyP4=true; }
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
  Serial.begin(9600);
  while(!Serial){} // Tinkercad

  lcd1.init(); lcd1.backlight();
  lcd2.init(); lcd2.backlight();

  LCD_BOTH_MSG(F("BATALHA NAVAL"), F("Aguarde..."));
  delay(1000);
  LCD_BOTH_MSG(F("Iniciando LEDS"), F(""));

  p1.begin(); p1.setBrightness(BRIGHTNESS); p1.show();
  p2.begin(); p2.setBrightness(BRIGHTNESS); p2.show();
  p3.begin(); p3.setBrightness(BRIGHTNESS); p3.show();
  p4.begin(); p4.setBrightness(BRIGHTNESS); p4.show();

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

  LCD_BOTH_MSG(F("Preenchendo"), F("tabuleiros..."));
  
  clearBoards();
  delay(300);
  renderAllNow();
  delay(120);
  renderAllNow();
  delay(120);

  LCD_BOTH_MSG(F("Pronto!"), F("")); 
  delay(300);
  LCD_MSG2(lcd1, F("Jogador 1"), F("posicione navios"));
  LCD_MSG2(lcd2, F("Aguarde"),   F("Vez do Jogador 1"));

  LCD_PRINTF(lcd1, F("Tam. navio:"), "%d", FLEET_SIZES[0]);

  markAllDirty();
}

void loop() {
  if      (state==PLACE_1 || state==TURN_1) handleButtonsForPlayer(1);
  else if (state==PLACE_2 || state==TURN_2) handleButtonsForPlayer(2);

  if (state==GAME_OVER) {
    if (!gameOverDrawn) {
      renderGameOverNow();
      gameOverDrawn = true;
      LCD_BOTH_PRINTF(F("GAME OVER!"), "Jogador %d venceu", winnerId);
    }
    return;
  }

  renderIfDirty();
}