#include <Adafruit_NeoPixel.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h> 

LiquidCrystal_I2C lcd1(0x20, 16, 2);
LiquidCrystal_I2C lcd2(0x21, 16, 2);

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

// printf simples (2ª linha)
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
#define COL_WATER1  C(0,0,12) // Azul
#define COL_WATER2  C(0,0,25) // Azul mais claro
#define COL_SHIP    C(82,59,53) // Amarelo escuro
#define COL_HIT     C(200,0,0)   // Vermelho
#define COL_MISS    C(80,80,80)  // Cinza
#define COL_CURSOR  C(0,180,0)   // Verde
#define COL_GHOST   C(200,200,0)  // Amarelo
#define COL_WIN     C(0,160,0)   // Verde
#define COL_LOSE    C(200,0,0)   // Vermelho

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
    if (x < 0 || y < 0 || x + len > W || y >= H) return false;
  } else {
    if (x < 0 || y < 0 || x >= W || y + len > H) return false;
  }

  int x0, y0, x1, y1;
  if (horiz) {
    x0 = x - 1; y0 = y - 1;
    x1 = x + len; y1 = y + 1;
  } else {
    x0 = x - 1; y0 = y - 1;
    x1 = x + 1; y1 = y + len;
  }

  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > W - 1) x1 = W - 1;
  if (y1 > H - 1) y1 = H - 1;

  for (int yy = y0; yy <= y1; yy++) {
    for (int xx = x0; xx <= x1; xx++) {
      if (B[yy][xx].ship) return false;
    }
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

// ===== Desenha tabuleiro =====
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
  // Verifica se pode posicionar o navio aqui
  bool pode = false;
  if (panel == 1 && state == PLACE_1) {
    pode = canPlaceOn(1, x, y, len, horiz);
  } else if (panel == 3 && state == PLACE_2) {
    pode = canPlaceOn(2, x, y, len, horiz);
  } else {
    pode = true; // fallback para outros usos
  }
  uint32_t cor = pode ? COL_CURSOR : COL_HIT;
  for (uint8_t k=0;k<len;k++){
    uint8_t gx = horiz? x+k : x;
    uint8_t gy = horiz? y   : y+k;
    if (gx<W && gy<H) setXY_on(panel, gx,gy, cor);
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
  if (state==PLACE_1) { uint8_t len=FLEET_SIZES[shipIndex]; drawGhost(1,curX,curY,len,horizontal); /* drawCursor(1,curX,curY); */ }
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
  if (state==PLACE_2) { uint8_t len=FLEET_SIZES[shipIndex]; drawGhost(3,curX,curY,len,horizontal); /* drawCursor(3,curX,curY); */ }
    show_on(3); dirtyP3=false;
  }
  if (dirtyP4) {
    clear_on(4); drawChecker(4);
    drawShots(4, 1);
    if (state==TURN_2) { drawCursor(4,curX,curY); }
    show_on(4); dirtyP4=false;
  }
}

// ===== Tela final =====
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


// ===== Nomes (máx 9 + '\0') =====
char name1[10] = "JOGADOR 1";
char name2[10] = "JOGADOR 2";

// Helper p/ pegar nome por id (1 ou 2)
const char* PNAME(int id){ return (id==1)? name1 : name2; }

// ===== Estatísticas p/ ranking =====
uint16_t shotsBy[3]      = {0,0,0};
uint16_t hitsBy[3]       = {0,0,0};
uint16_t sunkShipsBy[3]  = {0,0,0};
uint16_t sunkCellsBy[3]  = {0,0,0}; // soma dos comprimentos dos navios afundados

// UI de nomes: conjunto de caracteres aceitos
const char CHARSET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_ ";
const int  NCH = sizeof(CHARSET)-1; // sem o '\0'

void enterNameForPlayer(uint8_t player, char* dest, uint8_t maxLen){
  Btn &bUp    = (player==1)? bUp1   : bUp2;
  Btn &bDown  = (player==1)? bDown1 : bDown2;
  Btn &bLeft  = (player==1)? bLeft1 : bLeft2;
  Btn &bRight = (player==1)? bRight1: bRight2;
  Btn &bRot   = (player==1)? bRot1  : bRot2;
  Btn &bOk    = (player==1)? bOk1   : bOk2;
  LiquidCrystal_I2C &lcd = (player==1)? lcd1 : lcd2;

  uint8_t len = 0; dest[0]='\0';
  int idx = 0; // índice no CHARSET

  // Mensagens fixas
  char line1[17], preview[17];
  snprintf(line1, sizeof(line1), "Nome P%d (9):", player);

  while(true){
    // linha 2: nome já digitado + '>' + char atual
    snprintf(preview, sizeof(preview), "%s>%c", dest, CHARSET[idx]);
    // garante que cabe em 16 colunas
    preview[16] = '\0';

    LCD_MSG2(lcd, line1, preview);

    // Navega caractere
    if (bUp.fellOrRepeat())   { idx = (idx+1)%NCH; }
    if (bDown.fellOrRepeat()) { idx = (idx-1+NCH)%NCH; }

    // Adiciona caractere (RIGHT) – respeita limite
    if (bRight.fellOrRepeat()){
      if (len < maxLen){
        dest[len++] = CHARSET[idx];
        dest[len]   = '\0';
      }
    }

    // Backspace (LEFT ou ROTATE)
    if (bLeft.fellOrRepeat() || bRot.fellRaw()){
      if (len > 0){ dest[--len] = '\0'; }
    }

    // OK finaliza (se vazio, cai num default curto)
    if (bOk.fellRaw()){
      // trim espaço à direita
      while(len>0 && dest[len-1]==' '){ dest[--len]='\0'; }
      if (len==0){
        snprintf(dest, maxLen+1, "P%d", player);
      }
      // upper já está, mas se quiser minúsculas, adapte aqui
      break;
    }
  }
}

// ---------- TELEMETRIA ----------
unsigned long matchStartMs = 0;
int gid = 0; // id da partida atual (incrementa a cada jogo)

// imprime vetor de tamanhos de frota
void printFleetArray(){
  Serial.print('[');
  for (int i=0;i<FLEET_COUNT;i++){
    if (i) Serial.print(',');
    Serial.print(FLEET_SIZES[i]);
  }
  Serial.print(']');
}

void telem_game_start(){
  Serial.print(F("{\"type\":\"game_start\",\"gid\":")); Serial.print(gid);
  Serial.print(F(",\"w\":")); Serial.print(W);
  Serial.print(F(",\"h\":")); Serial.print(H);
  Serial.print(F(",\"p1_name\":\"")); Serial.print(name1); Serial.print(F("\""));
  Serial.print(F(",\"p2_name\":\"")); Serial.print(name2); Serial.print(F("\""));
  Serial.print(F(",\"fleet\":")); printFleetArray();
  Serial.print(F(",\"arduino_ms\":")); Serial.print(matchStartMs);
  Serial.println('}');
}

void telem_place_ship(int player, int x, int y, int len, bool horiz){
  Serial.print(F("{\"type\":\"place_ship\",\"gid\":")); Serial.print(gid);
  Serial.print(F(",\"player\":")); Serial.print(player);
  Serial.print(F(",\"x\":")); Serial.print(x);
  Serial.print(F(",\"y\":")); Serial.print(y);
  Serial.print(F(",\"len\":")); Serial.print(len);
  Serial.print(F(",\"horiz\":")); Serial.print(horiz ? 1 : 0);
  Serial.print(F(",\"cells\":["));
  for (int k=0;k<len;k++){
    int cx = horiz ? x+k : x;
    int cy = horiz ? y   : y+k;
    if (k) Serial.print(',');
    Serial.print('['); Serial.print(cx); Serial.print(','); Serial.print(cy); Serial.print(']');
  }
  Serial.print(']');
  Serial.print(F(",\"arduino_ms\":")); Serial.print(millis());
  Serial.println('}');
}

void telem_shot(int attacker, int defender, int x, int y, bool hit, bool sunk, int remaining_def){
  Serial.print(F("{\"type\":\"shot\",\"gid\":")); Serial.print(gid);
  Serial.print(F(",\"attacker\":")); Serial.print(attacker);
  Serial.print(F(",\"defender\":")); Serial.print(defender);
  Serial.print(F(",\"x\":")); Serial.print(x);
  Serial.print(F(",\"y\":")); Serial.print(y);
  Serial.print(F(",\"hit\":")); Serial.print(hit ? 1 : 0);
  Serial.print(F(",\"sunk\":")); Serial.print(sunk ? 1 : 0);
  Serial.print(F(",\"remaining_defender\":")); Serial.print(remaining_def);
  Serial.print(F(",\"arduino_ms\":")); Serial.print(millis());
  Serial.println('}');
}

void telem_game_end(int winner){
  unsigned long dur = millis() - matchStartMs;
  int s1 = calcScore(1, winner, dur);
  int s2 = calcScore(2, winner, dur);

  Serial.print(F("{\"type\":\"game_end\",\"gid\":")); Serial.print(gid);
  Serial.print(F(",\"winner\":")); Serial.print(winner);
  Serial.print(F(",\"duration_ms\":")); Serial.print(dur);
  Serial.print(F(",\"p1\":{\"name\":\"")); Serial.print(name1); Serial.print(F("\","));
  Serial.print(F("\"shots\":")); Serial.print(shotsBy[1]); Serial.print(F(","));
  Serial.print(F("\"hits\":")); Serial.print(hitsBy[1]); Serial.print(F(","));
  Serial.print(F("\"sunk_cells\":")); Serial.print(sunkCellsBy[1]); Serial.print(F(","));
  Serial.print(F("\"score\":")); Serial.print(s1); Serial.print(F("},"));
  Serial.print(F("\"p2\":{\"name\":\"")); Serial.print(name2); Serial.print(F("\","));
  Serial.print(F("\"shots\":")); Serial.print(shotsBy[2]); Serial.print(F(","));
  Serial.print(F("\"hits\":")); Serial.print(hitsBy[2]); Serial.print(F(","));
  Serial.print(F("\"sunk_cells\":")); Serial.print(sunkCellsBy[2]); Serial.print(F(","));
  Serial.print(F("\"score\":")); Serial.print(s2); Serial.print(F("}"));
  Serial.print(F(",\"arduino_ms\":")); Serial.print(millis());
  Serial.println('}');
}

// ---------- /TELEMETRIA ----------

int calcScore(int pid, int winnerId, unsigned long durationMs){
  int W = (pid==winnerId)? 1 : 0;
  int hits = hitsBy[pid];
  int shots = shotsBy[pid];
  int sunkCells = sunkCellsBy[pid];
  int sunkShips = sunkShipsBy[pid];

  float acc = (shots>0)? (float)hits / (float)shots : 0.0f;
  int base = 200*W + 10*hits + 5*sunkCells + 15*sunkShips + (int)(50.0f*acc + 0.5f);

  unsigned long secs = durationMs/1000UL;
  int timeBonus = 0;
  if (secs < 180UL) timeBonus = (int)((180UL - secs)/6UL); // 0..30

  return base + timeBonus;
}


void resetStats(){
  shotsBy[1]=shotsBy[2]=0;
  hitsBy[1]=hitsBy[2]=0;
  sunkShipsBy[1]=sunkShipsBy[2]=0;
  sunkCellsBy[1]=sunkCellsBy[2]=0;
}

void showTurnPrompt(){
  char buf[17];
  if (state==TURN_1){
    LCD_MSG2(lcd1, F("Sua vez: atirar"), F("Boa sorte!"));
    snprintf(buf,sizeof(buf),"Vez: %s", PNAME(1));  // "Vez: " + 9 = <=14
    LCD_MSG2(lcd2, F("Aguarde"), buf);
  } else if (state==TURN_2){
    LCD_MSG2(lcd2, F("Sua vez: atirar"), F("Boa sorte!"));
    snprintf(buf,sizeof(buf),"Vez: %s", PNAME(2));
    LCD_MSG2(lcd1, F("Aguarde"), buf);
  }
}

// ===== Jogo =====
void nextStateAfterPlacement(){
  shipIndex = 0; curX=0; curY=0; horizontal=true;
  if (state == PLACE_1) {
    state = PLACE_2;
    char buf[17];
    snprintf(buf,sizeof(buf),"Vez: %s", name2);
    LCD_MSG2(lcd1, F("Aguarde"), buf);
    LCD_MSG2(lcd2, name2, F("posicione navios"));
  } else {
    state = TURN_1;
    resetStats();
    matchStartMs = millis();
    telem_game_start();
    showTurnPrompt();
  }
  markAllDirty();
}

void tryConfirmPlacement(int myId){
  uint8_t len = FLEET_SIZES[shipIndex];
  if (canPlaceOn(myId, curX, curY, len, horizontal)) {
    placeShipOn(myId, curX, curY, len, horizontal);
    telem_place_ship(myId, curX, curY, len, horizontal);
    remaining[myId] += len;
    shipIndex++;
    if (myId==1) dirtyP1=true; else dirtyP3=true;
    if (shipIndex >= FLEET_COUNT){
      nextStateAfterPlacement();
    } else {
      if (myId==1) LCD_PRINTF(lcd1, F("Tam. navio:"), "%d", FLEET_SIZES[shipIndex]);
      else        LCD_PRINTF(lcd2, F("Tam. navio:"), "%d", FLEET_SIZES[shipIndex]);
    }
  } else {
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
  int attacker = (enemyId==2)? 1 : 2;
  shotsBy[attacker]++;
  bool afundou = false;
  if (c.ship) { 
    c.hit = 1; 
    remaining[enemyId]--; 
    hitsBy[attacker]++;
    // Descobre o tamanho e orientação do navio atingido
    int x0 = curX, y0 = curY, x1 = curX, y1 = curY;
    // Checa horizontalmente
    while (x0 > 0 && E[y0][x0-1].ship) x0--;
    while (x1 < W-1 && E[y0][x1+1].ship) x1++;
    int navio_tam_h = x1 - x0 + 1;
    // Checa verticalmente
    int y0v = curY, y1v = curY;
    while (y0v > 0 && E[y0v-1][curX].ship) y0v--;
    while (y1v < H-1 && E[y1v+1][curX].ship) y1v++;
    int navio_tam_v = y1v - y0v + 1;
    // Decide se é horizontal, vertical ou tamanho 1
    if (navio_tam_h > 1) {
      // Checa se todas as partes do navio horizontal estão com hit=1
      afundou = true;
      for (int k = x0; k <= x1; k++) {
        if (E[y0][k].ship && !E[y0][k].hit) { afundou = false; break; }
      }
    } else if (navio_tam_v > 1) {
      // Checa se todas as partes do navio vertical estão com hit=1
      afundou = true;
      for (int k = y0v; k <= y1v; k++) {
        if (E[k][curX].ship && !E[k][curX].hit) { afundou = false; break; }
      }
    } else {
      // Navio de tamanho 1
      afundou = true;
    }
    int ship_len = 1;
    if (navio_tam_h > 1) ship_len = navio_tam_h;
    else if (navio_tam_v > 1) ship_len = navio_tam_v;

    if (afundou) {
      sunkShipsBy[attacker]++;
      sunkCellsBy[attacker] += ship_len;
    }
    
    telem_shot(attacker, enemyId, curX, curY, /*hit=*/true, afundou, remaining[enemyId]);
    if (afundou) {
      LCD_BOTH_MSG(F("Acertou!"), F("Afundou o navio!"));
    } else {
      LCD_BOTH_MSG(F("Acertou!"), F("Parte do navio."));
    }
  }
  else {
    telem_shot(attacker, enemyId, curX, curY, /*hit=*/false, /*sunk=*/false, remaining[enemyId]);
    LCD_BOTH_MSG(F("Errou..."), F(""));
  }

  // marcar paineis relevantes como sujos:
  if (enemyId==2){ dirtyP2 = true; dirtyP3 = true; }  // J1 atirou no J2
  else            { dirtyP4 = true; dirtyP1 = true; }  // J2 atirou no J1

  // Mensagem de vez do outro jogador
  if (remaining[enemyId] == 0) {
    state = GAME_OVER;
    winnerId = (enemyId==2)? 1 : 2;
    gameOverDrawn = false;
    telem_game_end(winnerId);
    return;
  }

  // Aguarda um pouco para mostrar o resultado antes de trocar a vez
  delay(1200);
  if (state == TURN_1) {
    state = TURN_2;
    dirtyP4 = true;
  } else if (state == TURN_2) {
    state = TURN_1;
    dirtyP2 = true;
  }
  showTurnPrompt();
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
  while(!Serial){}

  lcd1.init(); lcd1.backlight();
  lcd2.init(); lcd2.backlight();

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

  p1.begin(); p1.setBrightness(BRIGHTNESS); p1.show();
  p2.begin(); p2.setBrightness(BRIGHTNESS); p2.show();
  p3.begin(); p3.setBrightness(BRIGHTNESS); p3.show();
  p4.begin(); p4.setBrightness(BRIGHTNESS); p4.show();

  LCD_BOTH_MSG(F("Preenchendo"), F("tabuleiros..."));
  
  clearBoards();
  delay(300);
  renderAllNow();
  delay(120);
  renderAllNow();
  delay(120);

  LCD_BOTH_MSG(F("Pronto!"), F("")); 
  delay(300);

  // 1) Nome do Jogador 1
  LCD_MSG2(lcd1, F("Escolha seu"), F("nome (OK p/ fim)"));
  LCD_MSG2(lcd2, F("Aguarde..."),  F(""));
  enterNameForPlayer(1, name1, 9);

  // 2) Nome do Jogador 2
  LCD_MSG2(lcd2, F("Escolha seu"), F("nome (OK p/ fim)"));
  LCD_MSG2(lcd1, F("Aguarde..."),  F(""));
  enterNameForPlayer(2, name2, 9);

  gid++;

  // Mensagens iniciais já com nomes
  LCD_MSG2(lcd1, name1, F("posicione navios"));
  char turnbuf[17];
  snprintf(turnbuf,sizeof(turnbuf),"Vez: %s", name1);
  LCD_MSG2(lcd2, F("Aguarde"), turnbuf);

  delay(1500);

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
      char buf[17];
      snprintf(buf, sizeof(buf), "%s venceu", PNAME(winnerId));
      LCD_BOTH_MSG(F("GAME OVER!"), buf);
    }
    return;
  }

  renderIfDirty();
}