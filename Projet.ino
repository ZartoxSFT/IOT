#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include "esp_timer.h"
#include "driver/gpio.h"
#include "FeatherShieldTFT.h"
#include "mario.h"

// --- INCLUSIONS BLE ---
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

TaskHandle_t audioTaskHandle;
TaskHandle_t bipTaskHandle;
volatile bool audioEnCours = false;

// --- CONFIGURATION BLE ---
// UUIDs générés pour le service et la caractéristique
#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID    "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"

BLEServer* pServer = NULL;
BLECharacteristic* pCharacteristic = NULL;
bool deviceConnected = false;

// Drapeau pour indiquer que l'application a demandé un redémarrage
volatile bool restartRequested = false;

// Gestion de la connexion/déconnexion BLE
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
      Serial.println("Appareil BLE connecté !");
    };

    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
      Serial.println("Appareil BLE déconnecté. Redémarrage de l'annonce...");
      BLEDevice::startAdvertising(); 
    }
};


// ============================================================
// AUDIO
// ============================================================
constexpr int SPEAKER = A2;
constexpr uint32_t PWM_FREQ = 78125;
constexpr uint32_t BIP_FREQ_HZ = 1000;
constexpr uint32_t BIP_DUREE_MS = 30;

// ============================================================
// RÉGLAGES DU SNAKE
// ============================================================
constexpr int NB_JOUEURS = 2;
constexpr int JOY_X_PIN[NB_JOUEURS] = { A0, A4 };
constexpr int JOY_Y_PIN[NB_JOUEURS] = { A1, A5 };
constexpr bool JOY_INVERT_X[NB_JOUEURS] = { false, false };
constexpr bool JOY_INVERT_Y[NB_JOUEURS] = { false, false };

constexpr int JOY_RESOLUTION_BITS = 12;
constexpr int JOY_MAX_VALUE = (1 << JOY_RESOLUTION_BITS) - 1;
constexpr int JOY_THRESHOLD = JOY_MAX_VALUE / 5;
constexpr int JOY_DEADZONE = JOY_MAX_VALUE / 12;

constexpr int CELL = 8;
constexpr int COLS = 30;
constexpr int ROWS = 16;
constexpr int OFFSET_Y = (135 - ROWS * CELL) / 2;
constexpr int MAX_LEN = COLS * ROWS;

constexpr unsigned long START_DELAY = 180;
constexpr unsigned long MIN_DELAY = 70;

// ============================================================
// DONNÉES DU JEU
// ============================================================
struct Point {
  int8_t x;
  int8_t y;
};

enum Direction { UP, RIGHT, DOWN, LEFT };
enum JoyState { JOY_CENTER, JOY_USED };

struct Player {
  Point snake[MAX_LEN];
  int snakeLen;
  Direction dir;
  Direction pendingDirection;
  bool alive;
  int score;
  int joyCenterX;
  int joyCenterY;
  JoyState joyState;
};

bool same(const Point &a, const Point &b);
void drawCell(const Point &p, uint16_t color);
void placeFood();
void showGameOver();
void newGame();
void step();
void readJoystick(int idx);
void assignerCouleursAleatoires();
void setPlayerColor(int idx, uint16_t couleur565);
void audioTask(void *parameter);
void bipTask(void *parameter);
void jouerSon();

// ============================================================
// VARIABLES DU JEU
// ============================================================
Player players[NB_JOUEURS];
Point food;
unsigned long stepDelay;
unsigned long lastStep;
bool gameOver;
uint16_t playerColor[NB_JOUEURS];

constexpr uint16_t PALETTE_COULEURS[] = {
  ST77XX_GREEN, ST77XX_CYAN, ST77XX_YELLOW, ST77XX_MAGENTA, 0xFC00, ST77XX_BLUE
};
constexpr int NB_COULEURS_PALETTE = sizeof(PALETTE_COULEURS) / sizeof(PALETTE_COULEURS[0]);

void assignerCouleursAleatoires() {
  for (int j = 0; j < NB_JOUEURS; j++) {
    uint16_t couleur;
    bool dejaPrise;
    do {
      couleur = PALETTE_COULEURS[random(NB_COULEURS_PALETTE)];
      dejaPrise = false;
      for (int k = 0; k < j; k++) {
        if (playerColor[k] == couleur) {
          dejaPrise = true;
          break;
        }
      }
    } while (dejaPrise);
    playerColor[j] = couleur;
  }
}

void setPlayerColor(int idx, uint16_t couleur565) {
  if (idx < 0 || idx >= NB_JOUEURS) return;
  playerColor[idx] = couleur565;
}

bool same(const Point &a, const Point &b) {
  return a.x == b.x && a.y == b.y;
}

void drawCell(const Point &p, uint16_t color) {
  tft.fillRect(p.x * CELL, OFFSET_Y + p.y * CELL, CELL - 1, CELL - 1, color);
}

void placeFood() {
  bool onSnake;
  do {
    food.x = random(COLS);
    food.y = random(ROWS);
    onSnake = false;
    for (int j = 0; j < NB_JOUEURS && !onSnake; j++) {
      for (int i = 0; i < players[j].snakeLen; i++) {
        if (same(players[j].snake[i], food)) {
          onSnake = true;
          break;
        }
      }
    }
  } while (onSnake);
  drawCell(food, ST77XX_RED);
}

void showGameOver() {
  tft.fillRect(20, 20, 200, 95, ST77XX_BLACK);
  tft.drawRect(20, 20, 200, 95, ST77XX_WHITE);
  tft.setTextColor(ST77XX_RED);
  tft.setTextSize(2);
  tft.setCursor(38, 28);
  tft.print("GAME OVER");
  tft.setTextSize(1);
  tft.setTextColor(playerColor[0]);
  tft.setCursor(38, 55);
  tft.print("J1 : ");
  tft.print(players[0].score);
  tft.setTextColor(playerColor[1]);
  tft.setCursor(38, 68);
  tft.print("J2 : ");
  tft.print(players[1].score);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(38, 90);
  tft.print("Bougez un joystick");
  tft.setCursor(38, 100);
  tft.print("pour rejouer");
}

void newGame() {
  tft.fillScreen(ST77XX_BLACK);
  tft.drawRect(0, OFFSET_Y - 1, COLS * CELL, ROWS * CELL + 1, 0x2104);

  players[0].dir = RIGHT;
  players[0].snakeLen = 3;
  for (int i = 0; i < players[0].snakeLen; i++) {
    players[0].snake[i] = { (int8_t)(COLS / 4 - i), (int8_t)(ROWS / 2) };
  }

  players[1].dir = LEFT;
  players[1].snakeLen = 3;
  for (int i = 0; i < players[1].snakeLen; i++) {
    players[1].snake[i] = { (int8_t)(3 * COLS / 4 + i), (int8_t)(ROWS / 2) };
  }

  for (int j = 0; j < NB_JOUEURS; j++) {
    players[j].pendingDirection = players[j].dir;
    players[j].alive = true;
    players[j].score = 0;
    players[j].joyState = JOY_CENTER;
    for (int i = 0; i < players[j].snakeLen; i++) {
      drawCell(players[j].snake[i], playerColor[j]);
    }
  }

  stepDelay = START_DELAY;
  gameOver = false;
  lastStep = millis();
  placeFood();
}

void step() {
  Point newHead[NB_JOUEURS];
  bool eating[NB_JOUEURS] = { false, false };
  bool died[NB_JOUEURS] = { false, false };

  for (int j = 0; j < NB_JOUEURS; j++) {
    if (!players[j].alive) continue;
    if (players[j].pendingDirection != players[j].dir) {
      players[j].dir = players[j].pendingDirection;
    }
    Point head = players[j].snake[0];
    switch (players[j].dir) {
      case UP: head.y++; break;
      case RIGHT: head.x++; break;
      case DOWN: head.y--; break;
      case LEFT: head.x--; break;
    }
    newHead[j] = head;
  }

  for (int j = 0; j < NB_JOUEURS; j++) {
    if (!players[j].alive) continue;
    Point &head = newHead[j];

    if (head.x < 0 || head.x >= COLS || head.y < 0 || head.y >= ROWS) {
      died[j] = true;
      continue;
    }

    eating[j] = same(head, food);
    int checkLen = eating[j] ? players[j].snakeLen : players[j].snakeLen - 1;

    for (int i = 0; i < checkLen; i++) {
      if (same(players[j].snake[i], head)) {
        died[j] = true;
        break;
      }
    }
    if (died[j]) continue;

    int autre = 1 - j;
    if (players[autre].alive) {
      for (int i = 0; i < players[autre].snakeLen; i++) {
        if (same(players[autre].snake[i], head)) {
          died[j] = true;
          break;
        }
      }
    }
  }

  if (NB_JOUEURS == 2 && players[0].alive && players[1].alive && !died[0] && !died[1] && same(newHead[0], newHead[1])) {
    died[0] = true;
    died[1] = true;
  }

  if (died[0] || died[1]) {
    xTaskNotifyGive(audioTaskHandle);
  }

  for (int j = 0; j < NB_JOUEURS; j++) {
    if (died[j] && players[j].alive) {
      for (int i = 0; i < players[j].snakeLen; i++) {
        drawCell(players[j].snake[i], ST77XX_BLACK);
      }
      players[j].alive = false;
      players[j].snakeLen = 0;
      Serial.printf("Joueur %d elimine - score final : %d\n", j + 1, players[j].score);
    }
  }

  for (int j = 0; j < NB_JOUEURS; j++) {
    if (!players[j].alive) continue;

    if (!eating[j]) {
      drawCell(players[j].snake[players[j].snakeLen - 1], ST77XX_BLACK);
    } else {
      players[j].snakeLen++;
    }

    for (int i = players[j].snakeLen - 1; i > 0; i--) {
      players[j].snake[i] = players[j].snake[i - 1];
    }
    players[j].snake[0] = newHead[j];
    drawCell(players[j].snake[0], playerColor[j]);

    if (eating[j]) {
      players[j].score++;
      if (stepDelay > MIN_DELAY) stepDelay -= 5;
      Serial.printf("Joueur %d - score : %d\n", j + 1, players[j].score);
      placeFood();
    }
  }

  bool quelquUnVivant = false;
  for (int j = 0; j < NB_JOUEURS; j++) {
    if (players[j].alive) quelquUnVivant = true;
  }
  if (!quelquUnVivant) gameOver = true;
}

void readJoystick(int idx) {
  Player &p = players[idx];
  int valeurX = analogRead(JOY_X_PIN[idx]);
  int valeurY = analogRead(JOY_Y_PIN[idx]);
  int diffX = valeurX - p.joyCenterX;
  int diffY = valeurY - p.joyCenterY;
  if (JOY_INVERT_X[idx]) diffX = -diffX;
  if (JOY_INVERT_Y[idx]) diffY = -diffY;

  if (p.joyState == JOY_CENTER) {
    Direction nouvelleDirection = p.dir;
    bool directionDemandee = false;

    if (abs(diffX) >= JOY_THRESHOLD && abs(diffX) >= abs(diffY)) {
      directionDemandee = true;
      if (diffX < 0) { nouvelleDirection = LEFT; xTaskNotifyGive(bipTaskHandle); } 
      else { nouvelleDirection = RIGHT; xTaskNotifyGive(bipTaskHandle); }
    } else if (abs(diffY) >= JOY_THRESHOLD) {
      directionDemandee = true;
      if (diffY < 0) { nouvelleDirection = UP; xTaskNotifyGive(bipTaskHandle); } 
      else { nouvelleDirection = DOWN; xTaskNotifyGive(bipTaskHandle); }
    }

    if (directionDemandee) {
      bool directionOpposee = (p.dir == UP && nouvelleDirection == DOWN) || 
                              (p.dir == DOWN && nouvelleDirection == UP) || 
                              (p.dir == LEFT && nouvelleDirection == RIGHT) || 
                              (p.dir == RIGHT && nouvelleDirection == LEFT);
      bool memeDirection = (nouvelleDirection == p.dir);

      if (!directionOpposee && !memeDirection) {
        if (gameOver) newGame();
        else if (p.alive) p.pendingDirection = nouvelleDirection;
      } else if (gameOver) {
        newGame();
      }
      p.joyState = JOY_USED;
    }
  } else {
    if (abs(diffX) <= JOY_DEADZONE && abs(diffY) <= JOY_DEADZONE) {
      p.joyState = JOY_CENTER;
    }
  }
}

void jouerSon() {
  audioEnCours = true;
  noTone(SPEAKER);
  ledcAttach(SPEAKER, PWM_FREQ, 8);
  gpio_set_drive_capability((gpio_num_t)SPEAKER, GPIO_DRIVE_CAP_3);

  int64_t debut = esp_timer_get_time();
  for (uint32_t i = 0; i < SON_LEN; i++) {
    ledcWrite(SPEAKER, pgm_read_byte(&SON_DATA[i]));
    int64_t prochain = debut + (int64_t)(i + 1) * 1000000LL / SON_FREQ;
    while (esp_timer_get_time() < prochain) {}
  }
  ledcWrite(SPEAKER, 0);
  ledcDetach(SPEAKER);
  pinMode(SPEAKER, OUTPUT);
  digitalWrite(SPEAKER, LOW);
  audioEnCours = false;
}

void audioTask(void *parameter) {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    jouerSon();
  }
}

void bipTask(void *parameter) {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (audioEnCours) continue;
    ledcAttach(SPEAKER, BIP_FREQ_HZ, 8);
    ledcWriteTone(SPEAKER, BIP_FREQ_HZ);
    delay(BIP_DUREE_MS);
    ledcWriteTone(SPEAKER, 0);
    ledcDetach(SPEAKER);
  }
}

// Gestion de la réception des messages BLE
class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pChar) {
      // Remplacement de std::string par String
      String rxValue = pChar->getValue();

      if (rxValue.length() > 0) {
        if (rxValue[0] == 'R') {
          Serial.println("Commande de redémarrage (R) reçue via BLE !");
          restartRequested = true;
        }
        if (rxValue[0] == 'C') {
           Serial.println("Commande de redémarrage (C) reçue via BLE !");
           setPlayerColor(0, 0x5);
        }
      }
    }
};

void setup() {
  Serial.begin(115200);

  // --- INITIALISATION BLE ---
  BLEDevice::init("Snake_ESP32");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  
  BLEService *pService = pServer->createService(SERVICE_UUID);
  
  // Création de la caractéristique en mode écriture (Write)
  pCharacteristic = pService->createCharacteristic(
                      CHARACTERISTIC_UUID,
                      BLECharacteristic::PROPERTY_WRITE
                    );
  pCharacteristic->setCallbacks(new MyCallbacks());
  
  pService->start();
  
  // Démarrage de l'annonce BLE
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);  
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
  Serial.println("Service BLE démarré. Prêt à être connecté !");

  pinMode(TFT_BACKLITE, OUTPUT);
  digitalWrite(TFT_BACKLITE, HIGH);
  pinMode(TFT_I2C_POWER, OUTPUT);
  digitalWrite(TFT_I2C_POWER, HIGH);
  delay(10);

  xTaskCreatePinnedToCore(audioTask, "AudioTask", 4096, NULL, 1, &audioTaskHandle, 1);
  xTaskCreatePinnedToCore(bipTask, "BipTask", 2048, NULL, 1, &bipTaskHandle, 1);

  analogReadResolution(JOY_RESOLUTION_BITS);
  for (int j = 0; j < NB_JOUEURS; j++) {
    long sommeX = 0;
    long sommeY = 0;
    constexpr int NB_ECHANTILLONS = 20;
    for (int i = 0; i < NB_ECHANTILLONS; i++) {
      sommeX += analogRead(JOY_X_PIN[j]);
      sommeY += analogRead(JOY_Y_PIN[j]);
      delay(2);
    }
    players[j].joyCenterX = sommeX / NB_ECHANTILLONS;
    players[j].joyCenterY = sommeY / NB_ECHANTILLONS;
    players[j].joyState = JOY_CENTER;
  }

  pinMode(SPEAKER, OUTPUT);
  digitalWrite(SPEAKER, LOW);

  tft.init(135, 240);
  tft.setRotation(3);
  randomSeed(micros());
  assignerCouleursAleatoires();
  jouerSon();
  newGame();
}

void loop() {
  // --- GESTION DU REDÉMARRAGE BLE ---
  if (restartRequested) {
    restartRequested = false; // On réinitialise le drapeau
    newGame();
  }

  for (int j = 0; j < NB_JOUEURS; j++) {
    readJoystick(j);
  }

  if (!gameOver && millis() - lastStep >= stepDelay) {
    lastStep = millis();
    step();
    if (gameOver) {
      jouerSon();
      showGameOver();
    }
  }
}