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
#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID    "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"

BLEServer* pServer = NULL;
BLECharacteristic* pCharacteristic = NULL;
bool deviceConnected = false;

volatile bool restartRequested = false;

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
// SÉLECTION DU JEU & RÉGLAGES MULTIJOUEURS (Jusqu'à 8)
// ============================================================
enum GameType { GAME_SNAKE, GAME_FLAPPY };
GameType currentGame = GAME_SNAKE; // Par défaut : Snake

constexpr int MAX_JOUEURS = 8;
constexpr int NB_JOYSTICKS_PHYSIQUES = 2; // Joysticks pour J1 et J2
int nbJoueursActifs = 2; // Par défaut à 2 joueurs

constexpr int JOY_X_PIN[NB_JOYSTICKS_PHYSIQUES] = { A0, A4 }; 
constexpr int JOY_Y_PIN[NB_JOYSTICKS_PHYSIQUES] = { A1, A5 };
constexpr bool JOY_INVERT_X[NB_JOYSTICKS_PHYSIQUES] = { false, false };
constexpr bool JOY_INVERT_Y[NB_JOYSTICKS_PHYSIQUES] = { false, false };

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

// États du programme et variables globales de jeu
enum GameState { STATE_MENU, STATE_PLAYING, STATE_GAMEOVER };
GameState currentState = STATE_MENU;

bool gameOver = false;
unsigned long stepDelay = START_DELAY;

// ============================================================
// STRUCTURES DE DONNÉES (SNAKE & FLAPPY)
// ============================================================
struct Point {
  int8_t x;
  int8_t y;
};

enum Direction { UP, RIGHT, DOWN, LEFT };
enum JoyState { JOY_CENTER, JOY_USED };

struct Player {
  // Pour le Snake
  Point snake[MAX_LEN];
  int snakeLen;
  Direction dir;
  Direction pendingDirection;
  
  // Pour le Flappy Bird
  float yPosFlappy;
  float yVelocity;
  
  bool alive;
  int score;
  int joyCenterX;
  int joyCenterY;
  JoyState joyState;
};

Player players[MAX_JOUEURS];
Point food; // Pour Snake

// Variables spécifiques Flappy Bird
int obstacleX;
int obstacleGapY;
int obstacleWidth = 4;
int obstacleGapHeight = 5; 
unsigned long flappyLastStep = 0;
unsigned long flappyStepDelay = 100;
unsigned long lastStep = 0;

uint16_t playerColor[MAX_JOUEURS];
constexpr uint16_t PALETTE_COULEURS[] = {
  ST77XX_GREEN, ST77XX_CYAN, ST77XX_YELLOW, ST77XX_MAGENTA, 0xFC00, ST77XX_BLUE, 0xF800, 0xFFFF
};
constexpr int NB_COULEURS_PALETTE = sizeof(PALETTE_COULEURS) / sizeof(PALETTE_COULEURS[0]);

// Prototypes
bool same(const Point &a, const Point &b);
void drawCell(const Point &p, uint16_t color);
void placeFood();
void showMenu();
void showGameOver();
void newGame();
void step();
void flappyStep();
void readJoystick(int idx);
void assignerCouleursAleatoires();
void audioTask(void *parameter);
void bipTask(void *parameter);
void jouerSon();
void envoyerScoresVersApp();

void assignerCouleursAleatoires() {
  for (int j = 0; j < MAX_JOUEURS; j++) {
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

bool same(const Point &a, const Point &b) {
  return a.x == b.x && a.y == b.y;
}

void drawCell(const Point &p, uint16_t color) {
  if (p.x >= 0 && p.x < COLS && p.y >= 0 && p.y < ROWS) {
    tft.fillRect(p.x * CELL, OFFSET_Y + p.y * CELL, CELL - 1, CELL - 1, color);
  }
}

void placeFood() {
  bool onSnake;
  do {
    food.x = random(COLS);
    food.y = random(ROWS);
    onSnake = false;
    for (int j = 0; j < nbJoueursActifs && !onSnake; j++) {
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

void envoyerScoresVersApp() {
  if (deviceConnected) {
    String gameName = (currentGame == GAME_SNAKE) ? "SNAKE" : "FLAPPY";
    String scoreMessage = "SCORE:" + gameName;
    for (int j = 0; j < nbJoueursActifs; j++) {
      scoreMessage += "," + String(players[j].score);
    }
    pCharacteristic->setValue(scoreMessage.c_str());
    pCharacteristic->notify(); 
    Serial.println("Scores envoyés à l'application via BLE !");
  }
}

void showMenu() {
  tft.fillScreen(ST77XX_BLACK);
  tft.drawRect(10, 10, 220, 115, ST77XX_WHITE);
  
  tft.setTextColor(ST77XX_GREEN);
  tft.setTextSize(2);
  tft.setCursor(45, 25);
  tft.print(currentGame == GAME_SNAKE ? "SNAKE" : "FLAPPY BIRD");

  tft.setTextSize(1);
  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(35, 65);
  tft.print("Mode : ");
  tft.print(nbJoueursActifs);
  tft.print(" Joueurs (Max 8)");

  tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(35, 82);
  tft.print("Jeu actif : ");
  tft.print(currentGame == GAME_SNAKE ? "Snake" : "Flappy");

  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(25, 105);
  tft.print("Joystick pour lancer");
}

void showGameOver() {
  tft.fillRect(10, 10, 220, 115, ST77XX_BLACK);
  tft.drawRect(10, 10, 220, 115, ST77XX_WHITE);
  tft.setTextColor(ST77XX_RED);
  tft.setTextSize(2);
  tft.setCursor(60, 15);
  tft.print("GAME OVER");
  
  tft.setTextSize(1);
  for (int j = 0; j < nbJoueursActifs; j++) {
    int xPos = (j < 4) ? 25 : 130;
    int yPos = 45 + ((j % 4) * 15);
    tft.setTextColor(playerColor[j]);
    tft.setCursor(xPos, yPos);
    tft.print("J"); tft.print(j + 1); tft.print(": ");
    tft.print(players[j].score);
  }
  
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(55, 110);
  tft.print("Joystick pour menu");

  envoyerScoresVersApp();
}

void newGame() {
  tft.fillScreen(ST77XX_BLACK);
  tft.drawRect(0, OFFSET_Y - 1, COLS * CELL, ROWS * CELL + 1, 0x2104);

  for (int j = 0; j < nbJoueursActifs; j++) {
    players[j].alive = true;
    players[j].score = 0;
    players[j].joyState = JOY_CENTER;

    if (currentGame == GAME_SNAKE) {
      players[j].snakeLen = (nbJoueursActifs >= 7) ? 2 : 3;
      int yPos = 1 + (j * (ROWS - 2) / max(1, nbJoueursActifs - 1));
      
      if (j % 2 == 0) {
        players[j].dir = RIGHT;
        for (int i = 0; i < players[j].snakeLen; i++) {
          players[j].snake[i] = { (int8_t)(5 - i), (int8_t)yPos };
        }
      } else {
        players[j].dir = LEFT;
        for (int i = 0; i < players[j].snakeLen; i++) {
          players[j].snake[i] = { (int8_t)(COLS - 6 + i), (int8_t)yPos };
        }
      }
      players[j].pendingDirection = players[j].dir;
      for (int i = 0; i < players[j].snakeLen; i++) {
        drawCell(players[j].snake[i], playerColor[j]);
      }
    } else {
      players[j].yPosFlappy = ROWS / 2;
      players[j].yVelocity = 0;
      Point p = { (int8_t)(4 + (j / 2)), (int8_t)players[j].yPosFlappy };
      drawCell(p, playerColor[j]);
    }
  }

  if (currentGame == GAME_FLAPPY) {
    obstacleX = COLS - 5;
    obstacleGapY = ROWS / 2 - obstacleGapHeight / 2;
    flappyStepDelay = 120;
  }

  stepDelay = START_DELAY;
  gameOver = false;
  lastStep = millis();
  flappyLastStep = millis();
  if (currentGame == GAME_SNAKE) placeFood();
}

void step() {
  Point newHead[8];
  bool eating[8] = { false };
  bool died[8] = { false };

  for (int j = 0; j < nbJoueursActifs; j++) {
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

  for (int j = 0; j < nbJoueursActifs; j++) {
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

    for (int autre = 0; autre < nbJoueursActifs; autre++) {
      if (players[autre].alive) {
        for (int i = 0; i < players[autre].snakeLen; i++) {
          if (j == autre && i == 0) continue; 
          if (same(players[autre].snake[i], head)) {
            died[j] = true;
            break;
          }
        }
      }
    }
  }

  bool unMort = false;
  for (int j = 0; j < nbJoueursActifs; j++) {
    if (died[j]) unMort = true;
  }
  if (unMort) xTaskNotifyGive(audioTaskHandle);

  for (int j = 0; j < nbJoueursActifs; j++) {
    if (died[j] && players[j].alive) {
      for (int i = 0; i < players[j].snakeLen; i++) {
        drawCell(players[j].snake[i], ST77XX_BLACK);
      }
      players[j].alive = false;
      players[j].snakeLen = 0;
    }
  }

  for (int j = 0; j < nbJoueursActifs; j++) {
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
      if (stepDelay > MIN_DELAY) stepDelay -= 3;
      placeFood();
    }
  }

  bool quelquUnVivant = false;
  for (int j = 0; j < nbJoueursActifs; j++) {
    if (players[j].alive) quelquUnVivant = true;
  }
  if (!quelquUnVivant) gameOver = true;
}

void flappyStep() {
  for (int x = obstacleX; x < obstacleX + obstacleWidth; x++) {
    for (int y = 0; y < ROWS; y++) {
      if (y < obstacleGapY || y >= obstacleGapY + obstacleGapHeight) {
        Point p = { (int8_t)x, (int8_t)y };
        drawCell(p, ST77XX_BLACK);
      }
    }
  }

  obstacleX--;
  if (obstacleX < -obstacleWidth) {
    obstacleX = COLS - 1;
    obstacleGapY = random(1, ROWS - obstacleGapHeight - 1);
    for (int j = 0; j < nbJoueursActifs; j++) {
      if (players[j].alive) players[j].score++;
    }
  }

  for (int j = 0; j < nbJoueursActifs; j++) {
    if (!players[j].alive) continue;

    Point oldPos = { (int8_t)(4 + (j / 2)), (int8_t)players[j].yPosFlappy };
    drawCell(oldPos, ST77XX_BLACK);

    players[j].yPosFlappy += players[j].yVelocity;
    players[j].yVelocity += 0.4f;

    int birdX = 4 + (j / 2);
    int birdY = (int)players[j].yPosFlappy;

    if (birdY < 0 || birdY >= ROWS) {
      players[j].alive = false;
    } else if (birdX >= obstacleX && birdX < obstacleX + obstacleWidth) {
      if (birdY < obstacleGapY || birdY >= obstacleGapY + obstacleGapHeight) {
        players[j].alive = false;
      }
    }

    if (players[j].alive) {
      Point newPos = { (int8_t)birdX, (int8_t)birdY };
      drawCell(newPos, playerColor[j]);
    }
  }

  for (int x = obstacleX; x < obstacleX + obstacleWidth; x++) {
    if (x >= 0 && x < COLS) {
      for (int y = 0; y < ROWS; y++) {
        if (y < obstacleGapY || y >= obstacleGapY + obstacleGapHeight) {
          Point p = { (int8_t)x, (int8_t)y };
          drawCell(p, ST77XX_BLUE);
        }
      }
    }
  }

  bool quelquUnVivant = false;
  for (int j = 0; j < nbJoueursActifs; j++) {
    if (players[j].alive) quelquUnVivant = true;
  }
  if (!quelquUnVivant) {
    xTaskNotifyGive(audioTaskHandle);
    gameOver = true;
  }
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
    bool actionDemandee = false;

    if (currentGame == GAME_SNAKE) {
      Direction nouvelleDirection = p.dir;
      if (abs(diffX) >= JOY_THRESHOLD && abs(diffX) >= abs(diffY)) {
        actionDemandee = true;
        if (diffX < 0) nouvelleDirection = LEFT; else nouvelleDirection = RIGHT;
        xTaskNotifyGive(bipTaskHandle);
      } else if (abs(diffY) >= JOY_THRESHOLD) {
        actionDemandee = true;
        if (diffY < 0) nouvelleDirection = UP; else nouvelleDirection = DOWN;
        xTaskNotifyGive(bipTaskHandle);
      }

      if (actionDemandee) {
        if (currentState == STATE_MENU) {
          currentState = STATE_PLAYING;
          newGame();
        } else if (currentState == STATE_GAMEOVER) {
          currentState = STATE_MENU;
          showMenu();
        } else if (currentState == STATE_PLAYING && p.alive) {
          bool oppose = (p.dir == UP && nouvelleDirection == DOWN) || 
                        (p.dir == DOWN && nouvelleDirection == UP) || 
                        (p.dir == LEFT && nouvelleDirection == RIGHT) || 
                        (p.dir == RIGHT && nouvelleDirection == LEFT);
          if (!oppose) p.pendingDirection = nouvelleDirection;
        }
        p.joyState = JOY_USED;
      }
    } else {
      if (abs(diffY) >= JOY_THRESHOLD || abs(diffX) >= JOY_THRESHOLD) {
        actionDemandee = true;
        xTaskNotifyGive(bipTaskHandle);
      }

      if (actionDemandee) {
        if (currentState == STATE_MENU) {
          currentState = STATE_PLAYING;
          newGame();
        } else if (currentState == STATE_GAMEOVER) {
          currentState = STATE_MENU;
          showMenu();
        } else if (currentState == STATE_PLAYING && p.alive) {
          p.yVelocity = -1.5f;
        }
        p.joyState = JOY_USED;
      }
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

class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pChar) {
      String rxValue = pChar->getValue();

      if (rxValue.length() > 0) {
        if (rxValue.startsWith("G") && rxValue.length() <= 2) {
          int gameChoice = rxValue.substring(1).toInt();
          if (gameChoice == 1) currentGame = GAME_SNAKE;
          else if (gameChoice == 2) currentGame = GAME_FLAPPY;
          Serial.printf("Jeu sélectionné : %s\n", currentGame == GAME_SNAKE ? "Snake" : "Flappy Bird");
          if (currentState == STATE_MENU) showMenu();
        }
        else if (rxValue.startsWith("P") && rxValue.length() <= 3) {
          int mode = rxValue.substring(1).toInt();
          if (mode >= 1 && mode <= MAX_JOUEURS) {
            nbJoueursActifs = mode;
            Serial.printf("Mode de jeu mis à jour : %d joueurs\n", nbJoueursActifs);
            if (currentState == STATE_MENU) showMenu(); 
          }
        }
        else if (rxValue.indexOf(':') != -1 && currentState == STATE_PLAYING) {
          int playerIdx = rxValue.substring(0, rxValue.indexOf(':')).toInt() - 1; 
          char cmdChar = rxValue.charAt(rxValue.indexOf(':') + 1);

          if (playerIdx >= NB_JOYSTICKS_PHYSIQUES && playerIdx < nbJoueursActifs && players[playerIdx].alive) {
            if (currentGame == GAME_SNAKE) {
              Direction nouvelleDir = players[playerIdx].dir;
              if (cmdChar == 'U') nouvelleDir = UP;
              else if (cmdChar == 'D') nouvelleDir = DOWN;
              else if (cmdChar == 'L') nouvelleDir = LEFT;
              else if (cmdChar == 'R') nouvelleDir = RIGHT;

              bool oppose = (players[playerIdx].dir == UP && nouvelleDir == DOWN) || 
                            (players[playerIdx].dir == DOWN && nouvelleDir == UP) || 
                            (players[playerIdx].dir == LEFT && nouvelleDir == RIGHT) || 
                            (players[playerIdx].dir == RIGHT && nouvelleDir == LEFT);
              if (!oppose) players[playerIdx].pendingDirection = nouvelleDir;
            } else {
              players[playerIdx].yVelocity = -1.5f;
            }
          }
        }
        if (rxValue[0] == 'R') {
          currentState = STATE_MENU;
          showMenu();
        }
      }
    }
};

void setup() {
  Serial.begin(115200);

  BLEDevice::init("GameApp_ESP32");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  
  BLEService *pService = pServer->createService(SERVICE_UUID);
  
  pCharacteristic = pService->createCharacteristic(
                      CHARACTERISTIC_UUID,
                      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY
                    );
  pCharacteristic->setCallbacks(new MyCallbacks());
  pCharacteristic->addDescriptor(new BLE2902());
  
  pService->start();
  
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);  
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();

  pinMode(TFT_BACKLITE, OUTPUT);
  digitalWrite(TFT_BACKLITE, HIGH);
  pinMode(TFT_I2C_POWER, OUTPUT);
  digitalWrite(TFT_I2C_POWER, HIGH);
  delay(10);

  xTaskCreatePinnedToCore(audioTask, "AudioTask", 4096, NULL, 1, &audioTaskHandle, 1);
  xTaskCreatePinnedToCore(bipTask, "BipTask", 2048, NULL, 1, &bipTaskHandle, 1);

  analogReadResolution(JOY_RESOLUTION_BITS);
  for (int j = 0; j < NB_JOYSTICKS_PHYSIQUES; j++) {
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
  
  showMenu();
}

void loop() {
  if (restartRequested) {
    restartRequested = false; 
    currentState = STATE_MENU;
    showMenu();
  }

  for (int j = 0; j < min(nbJoueursActifs, NB_JOYSTICKS_PHYSIQUES); j++) {
    readJoystick(j);
  }

  if (currentState == STATE_PLAYING) {
    if (currentGame == GAME_SNAKE) {
      if (!gameOver && millis() - lastStep >= stepDelay) {
        lastStep = millis();
        step();
        if (gameOver) {
          jouerSon();
          showGameOver();
          currentState = STATE_GAMEOVER;
        }
      }
    } else {
      if (!gameOver && millis() - flappyLastStep >= flappyStepDelay) {
        flappyLastStep = millis();
        flappyStep();
        if (gameOver) {
          jouerSon();
          showGameOver();
          currentState = STATE_GAMEOVER;
        }
      }
    }
  }
}