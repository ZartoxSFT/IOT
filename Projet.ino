#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include "esp_timer.h"
#include "driver/gpio.h"
#include "FeatherShieldTFT.h"
#include "mario.h"

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

// Handles des tâches audio : déclarés ici, AVANT toute utilisation,
// car ils sont référencés dans step(), readJoystick(), setup() et loop().
TaskHandle_t audioTaskHandle;  // joue le son de mort (Mario)
TaskHandle_t bipTaskHandle;    // joue le petit bip de déplacement

// Vrai pendant que jouerSon() (le son de mort) est en cours.
// bipTask le consulte pour ne jamais bipper par-dessus.
volatile bool audioEnCours = false;


// ============================================================
// AUDIO
// ============================================================

constexpr int SPEAKER = A2;

constexpr uint32_t PWM_FREQ = 78125;

// Réglages du bip de déplacement.
constexpr uint32_t BIP_FREQ_HZ = 1000;
constexpr uint32_t BIP_DUREE_MS = 30;


// ============================================================
// RÉGLAGES DU SNAKE
// ============================================================

constexpr int NB_JOUEURS = 2;

// --- Broches des joysticks (2 axes analogiques par joueur) ---
// Joueur 1 : X -> A0, Y -> A1
// Joueur 2 : X -> A4, Y -> A5
constexpr int JOY_X_PIN[NB_JOUEURS] = { A0, A4 };
constexpr int JOY_Y_PIN[NB_JOUEURS] = { A1, A5 };

// Permet d'inverser indépendamment X et Y si le câblage du joystick
// donne une direction opposée à celle attendue.
constexpr bool JOY_INVERT_X[NB_JOUEURS] = { false, false };
constexpr bool JOY_INVERT_Y[NB_JOUEURS] = { false, false };

constexpr int JOY_RESOLUTION_BITS = 12;                        // résolution ADC ESP32
constexpr int JOY_MAX_VALUE = (1 << JOY_RESOLUTION_BITS) - 1;  // 4095

// Distance par rapport au centre à partir de laquelle on
// considère que le joystick est poussé à gauche/droite.
constexpr int JOY_THRESHOLD = JOY_MAX_VALUE / 5;  // ~800

// Distance par rapport au centre en dessous de laquelle on
// considère que le joystick est revenu au repos (hystérésis,
// doit être plus petit que JOY_THRESHOLD).
constexpr int JOY_DEADZONE = JOY_MAX_VALUE / 12;  // ~340

constexpr int CELL = 8;

constexpr int COLS = 30;
constexpr int ROWS = 16;

constexpr int OFFSET_Y =
  (135 - ROWS * CELL) / 2;

constexpr int MAX_LEN =
  COLS * ROWS;

constexpr unsigned long START_DELAY = 180;
constexpr unsigned long MIN_DELAY = 70;


// ============================================================
// DONNÉES DU JEU
// ============================================================

// IMPORTANT : Point doit être défini avant les prototypes
// pour éviter le problème du préprocesseur Arduino.

struct Point {
  int8_t x;
  int8_t y;
};

enum Direction {
  UP,
  RIGHT,
  DOWN,
  LEFT
};

enum JoyState {
  JOY_CENTER,
  JOY_USED
};

// Un joueur = un serpent + son état de jeu + son joystick.
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


// Prototypes explicites.
// Cela empêche Arduino de générer des prototypes
// avant la définition de Point / Player.

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


// ============================================================
// COULEURS DES JOUEURS
// ============================================================

// Couleur de chaque joueur. Ce sont des VARIABLES (pas des
// constantes) pour pouvoir être modifiées plus tard, par ex.
// depuis l'application mobile (Bluetooth/WiFi), via
// setPlayerColor(). Un changement prend effet dès le prochain
// segment dessiné.
uint16_t playerColor[NB_JOUEURS];

// Palette dans laquelle les couleurs sont tirées au hasard au
// démarrage. Le rouge est évité (réservé à la nourriture), le
// noir et le blanc aussi (fond et bordures/texte).
constexpr uint16_t PALETTE_COULEURS[] = {
  ST77XX_GREEN,
  ST77XX_CYAN,
  ST77XX_YELLOW,
  ST77XX_MAGENTA,
  0xFC00,  // orange
  ST77XX_BLUE,
};

constexpr int NB_COULEURS_PALETTE =
  sizeof(PALETTE_COULEURS) / sizeof(PALETTE_COULEURS[0]);

// Tire les couleurs des joueurs au hasard dans la palette, en
// s'assurant qu'elles soient toutes différentes entre elles.
// Appelé une seule fois au démarrage : les couleurs restent
// ensuite les mêmes d'une partie à l'autre, sauf si l'app
// mobile les change via setPlayerColor().
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

// Point d'entrée prévu pour l'app mobile : change la couleur
// d'un joueur à la volée (idx = 0 pour J1, 1 pour J2).
void setPlayerColor(int idx, uint16_t couleur565) {

  if (idx < 0 || idx >= NB_JOUEURS) {
    return;
  }

  playerColor[idx] = couleur565;
}


// ============================================================
// OUTILS
// ============================================================

bool same(const Point &a, const Point &b) {

  return a.x == b.x && a.y == b.y;
}


// ============================================================
// DESSIN
// ============================================================

void drawCell(const Point &p, uint16_t color) {

  tft.fillRect(
    p.x * CELL,
    OFFSET_Y + p.y * CELL,
    CELL - 1,
    CELL - 1,
    color);
}


// ============================================================
// NOURRITURE
// ============================================================

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

  drawCell(
    food,
    ST77XX_RED);
}


// ============================================================
// GAME OVER
// ============================================================

void showGameOver() {

  tft.fillRect(
    20,
    20,
    200,
    95,
    ST77XX_BLACK);

  tft.drawRect(
    20,
    20,
    200,
    95,
    ST77XX_WHITE);

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


// ============================================================
// NOUVELLE PARTIE
// ============================================================

void newGame() {

  tft.fillScreen(ST77XX_BLACK);

  tft.drawRect(
    0,
    OFFSET_Y - 1,
    COLS * CELL,
    ROWS * CELL + 1,
    0x2104);


  // --- Joueur 1 : part à gauche de l'écran, avance vers la droite ---
  players[0].dir = RIGHT;
  players[0].snakeLen = 3;

  for (int i = 0; i < players[0].snakeLen; i++) {

    players[0].snake[i] = {
      (int8_t)(COLS / 4 - i),
      (int8_t)(ROWS / 2)
    };
  }


  // --- Joueur 2 : part à droite de l'écran, avance vers la gauche ---
  players[1].dir = LEFT;
  players[1].snakeLen = 3;

  for (int i = 0; i < players[1].snakeLen; i++) {

    players[1].snake[i] = {
      (int8_t)(3 * COLS / 4 + i),
      (int8_t)(ROWS / 2)
    };
  }


  for (int j = 0; j < NB_JOUEURS; j++) {

    players[j].pendingDirection = players[j].dir;
    players[j].alive = true;
    players[j].score = 0;
    players[j].joyState = JOY_CENTER;

    for (int i = 0; i < players[j].snakeLen; i++) {

      drawCell(
        players[j].snake[i],
        playerColor[j]);
    }
  }

  stepDelay = START_DELAY;

  gameOver = false;

  lastStep = millis();

  placeFood();
}


// ============================================================
// DÉPLACEMENT DES SERPENTS
// ============================================================

void step() {

  Point newHead[NB_JOUEURS];
  bool eating[NB_JOUEURS] = { false, false };
  bool died[NB_JOUEURS] = { false, false };


  // ----------------------------------------------------------
  // 1) Applique le virage et calcule la nouvelle tête de
  //    chaque joueur encore vivant
  // ----------------------------------------------------------

  for (int j = 0; j < NB_JOUEURS; j++) {

    if (!players[j].alive) {
      continue;
    }

    // Applique le changement de direction demandé par le joystick.
    // Le contrôle du joystick empêche déjà les demi-tours et les
    // demandes identiques à la direction actuelle.
    if (players[j].pendingDirection != players[j].dir) {
      players[j].dir = players[j].pendingDirection;
    }

    Point head = players[j].snake[0];

    switch (players[j].dir) {

      case UP:
        head.y++;
        break;

      case RIGHT:
        head.x++;
        break;

      case DOWN:
        head.y--;
        break;

      case LEFT:
        head.x--;
        break;
    }

    newHead[j] = head;
  }


  // ----------------------------------------------------------
  // 2) Détecte les collisions : mur, son propre corps, le
  //    corps de l'autre joueur
  // ----------------------------------------------------------

  for (int j = 0; j < NB_JOUEURS; j++) {

    if (!players[j].alive) {
      continue;
    }

    Point &head = newHead[j];

    // Mur
    if (
      head.x < 0 || head.x >= COLS || head.y < 0 || head.y >= ROWS) {

      died[j] = true;
      continue;
    }

    eating[j] = same(head, food);

    // Son propre corps
    int checkLen =
      eating[j] ? players[j].snakeLen : players[j].snakeLen - 1;

    for (int i = 0; i < checkLen; i++) {

      if (same(players[j].snake[i], head)) {

        died[j] = true;
        break;
      }
    }

    if (died[j]) {
      continue;
    }

    // Le corps de l'autre joueur (sa position avant qu'il ne
    // bouge lui-même ce tour-ci)
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

  // Collision frontale : les deux têtes arrivent sur la même case
  if (
    NB_JOUEURS == 2 && players[0].alive && players[1].alive && !died[0] && !died[1] && same(newHead[0], newHead[1])) {

    died[0] = true;
    died[1] = true;
  }

  // Un seul son de mort par tour, même si les deux joueurs meurent
  // en même temps : on prévient la tâche audio ici, une bonne fois.
  if (died[0] || died[1]) {
    xTaskNotifyGive(audioTaskHandle);
  }


  // ----------------------------------------------------------
  // 3) Élimine les joueurs qui viennent de mourir : leur
  //    serpent disparaît de l'écran
  // ----------------------------------------------------------

  for (int j = 0; j < NB_JOUEURS; j++) {

    if (died[j] && players[j].alive) {

      for (int i = 0; i < players[j].snakeLen; i++) {

        drawCell(
          players[j].snake[i],
          ST77XX_BLACK);
      }

      players[j].alive = false;
      players[j].snakeLen = 0;

      Serial.printf(
        "Joueur %d elimine - score final : %d\n",
        j + 1,
        players[j].score);
    }
  }


  // ----------------------------------------------------------
  // 4) Fait avancer les joueurs encore vivants
  // ----------------------------------------------------------

  for (int j = 0; j < NB_JOUEURS; j++) {

    if (!players[j].alive) {
      continue;
    }

    if (!eating[j]) {

      drawCell(
        players[j].snake[players[j].snakeLen - 1],
        ST77XX_BLACK);

    } else {

      players[j].snakeLen++;
    }

    for (
      int i = players[j].snakeLen - 1;
      i > 0;
      i--) {

      players[j].snake[i] = players[j].snake[i - 1];
    }

    players[j].snake[0] = newHead[j];

    drawCell(
      players[j].snake[0],
      playerColor[j]);

    if (eating[j]) {

      players[j].score++;

      if (stepDelay > MIN_DELAY) {
        stepDelay -= 5;
      }

      Serial.printf(
        "Joueur %d - score : %d\n",
        j + 1,
        players[j].score);

      placeFood();
    }
  }


  // ----------------------------------------------------------
  // 5) La partie se termine quand plus personne n'est vivant
  // ----------------------------------------------------------

  bool quelquUnVivant = false;

  for (int j = 0; j < NB_JOUEURS; j++) {

    if (players[j].alive) {
      quelquUnVivant = true;
    }
  }

  if (!quelquUnVivant) {
    gameOver = true;
  }
}


// ============================================================
// JOYSTICKS (un axe analogique par joueur)
// ============================================================

void readJoystick(int idx) {

  Player &p = players[idx];

  int valeurX = analogRead(JOY_X_PIN[idx]);
  int valeurY = analogRead(JOY_Y_PIN[idx]);

  int diffX = valeurX - p.joyCenterX;
  int diffY = valeurY - p.joyCenterY;

  if (JOY_INVERT_X[idx]) {
    diffX = -diffX;
  }

  if (JOY_INVERT_Y[idx]) {
    diffY = -diffY;
  }

  // Tant que le joystick n'est pas revenu au centre, on ignore
  // les nouvelles poussées. Cela évite plusieurs changements
  // de direction avec une seule poussée.
  if (p.joyState == JOY_CENTER) {

    Direction nouvelleDirection = p.dir;
    bool directionDemandee = false;

    // On choisit l'axe qui est le plus éloigné du centre.
    // Cela évite qu'un petit bruit sur l'autre axe fasse
    // changer de direction par erreur.
    if (abs(diffX) >= JOY_THRESHOLD && abs(diffX) >= abs(diffY)) {

      directionDemandee = true;

      if (diffX < 0) {
        nouvelleDirection = LEFT;
      } else {
        nouvelleDirection = RIGHT;
      }

    } else if (abs(diffY) >= JOY_THRESHOLD) {

      directionDemandee = true;

      if (diffY < 0) {
        nouvelleDirection = UP;
      } else {
        nouvelleDirection = DOWN;
      }
    }

    if (directionDemandee) {

      // On autorise uniquement :
      // - la direction actuelle -> ignorée
      // - une direction perpendiculaire -> acceptée
      // - la direction opposée -> interdite
      bool directionOpposee =
        (p.dir == UP && nouvelleDirection == DOWN) || (p.dir == DOWN && nouvelleDirection == UP) || (p.dir == LEFT && nouvelleDirection == RIGHT) || (p.dir == RIGHT && nouvelleDirection == LEFT);

      bool memeDirection = (nouvelleDirection == p.dir);

      // Bip sur tout mouvement pris en compte. Si le son de mort
      // est en cours, bipTask ignorera cette notification (voir
      // plus bas) : le son de mort passe toujours devant le bip.
      xTaskNotifyGive(bipTaskHandle);

      if (!directionOpposee && !memeDirection) {

        if (gameOver) {
          newGame();
        } else if (p.alive) {
          p.pendingDirection = nouvelleDirection;
        }

      } else if (gameOver) {
        newGame();
      }

      p.joyState = JOY_USED;
    }

  } else {

    // Réarmement du joystick une fois revenu proche du centre.
    if (abs(diffX) <= JOY_DEADZONE && abs(diffY) <= JOY_DEADZONE) {

      p.joyState = JOY_CENTER;
    }
  }
}


// ============================================================
// AUDIO (tâches + lecture des sons)
// ============================================================

// Joue l'échantillon Mario (son de mort). Bloquant le temps de
// l'échantillon, mais tourne dans sa propre tâche sur le coeur 1,
// donc ne bloque jamais le jeu (loop/step) qui tourne sur le coeur 0.
void jouerSon() {

  audioEnCours = true;

  noTone(SPEAKER);

  ledcAttach(SPEAKER, PWM_FREQ, 8);

  gpio_set_drive_capability(
    (gpio_num_t)SPEAKER,
    GPIO_DRIVE_CAP_3);

  int64_t debut = esp_timer_get_time();

  for (uint32_t i = 0; i < SON_LEN; i++) {

    ledcWrite(
      SPEAKER,
      pgm_read_byte(&SON_DATA[i]));

    int64_t prochain =
      debut + (int64_t)(i + 1) * 1000000LL / SON_FREQ;

    while (esp_timer_get_time() < prochain) {
    }
  }

  ledcWrite(SPEAKER, 0);

  ledcDetach(SPEAKER);

  pinMode(SPEAKER, OUTPUT);
  digitalWrite(SPEAKER, LOW);

  audioEnCours = false;
}

// Tâche qui attend une notification puis joue le son de mort.
void audioTask(void *parameter) {

  while (true) {

    // Attend qu'on lui demande de jouer le son
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    jouerSon();
  }
}

// Tâche qui attend une notification puis joue un petit bip bref,
// utilisée pour signaler un déplacement de joystick. Tourne sur
// le coeur 1, comme audioTask, pour ne jamais bloquer le jeu.
//
// Tant que le son de mort (jouerSon) n'est pas terminé, on ignore
// simplement la notification de bip : le son Mario garde toujours
// la priorité sur le haut-parleur.
void bipTask(void *parameter) {

  while (true) {

    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    if (audioEnCours) {
      continue;
    }

    ledcAttach(SPEAKER, BIP_FREQ_HZ, 8);
    ledcWriteTone(SPEAKER, BIP_FREQ_HZ);

    delay(BIP_DUREE_MS);  // bloquant, mais bref — acceptable pour un bip

    ledcWriteTone(SPEAKER, 0);
    ledcDetach(SPEAKER);
  }
}


// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(115200);


  // ----------------------------------------------------------
  // Alimentation TFT
  // ----------------------------------------------------------

  pinMode(TFT_BACKLITE, OUTPUT);
  digitalWrite(TFT_BACKLITE, HIGH);

  pinMode(TFT_I2C_POWER, OUTPUT);
  digitalWrite(TFT_I2C_POWER, HIGH);

  delay(10);

  // Les deux tâches audio tournent sur le coeur 1, pour ne jamais
  // ralentir la boucle de jeu (coeur 0).
  xTaskCreatePinnedToCore(
    audioTask,
    "AudioTask",
    4096,
    NULL,
    1,
    &audioTaskHandle,
    1);

  xTaskCreatePinnedToCore(
    bipTask,
    "BipTask",
    2048,
    NULL,
    1,
    &bipTaskHandle,
    1);


  // ----------------------------------------------------------
  // Joysticks
  // ----------------------------------------------------------

  analogReadResolution(JOY_RESOLUTION_BITS);

  for (int j = 0; j < NB_JOUEURS; j++) {

    // Calibre les deux axes au centre en moyennant plusieurs lectures.
    // Les joysticks doivent être au repos au démarrage.
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

    Serial.printf(
      "Centre joystick joueur %d : X=%d Y=%d\n",
      j + 1,
      players[j].joyCenterX,
      players[j].joyCenterY);
  }


  // ----------------------------------------------------------
  // Speaker
  // ----------------------------------------------------------

  pinMode(
    SPEAKER,
    OUTPUT);

  digitalWrite(
    SPEAKER,
    LOW);


  // ----------------------------------------------------------
  // Écran
  // ----------------------------------------------------------

  tft.init(
    135,
    240);

  tft.setRotation(3);


  // ----------------------------------------------------------
  // Random
  // ----------------------------------------------------------

  randomSeed(micros());

  assignerCouleursAleatoires();


  // ----------------------------------------------------------
  // Joue le son Mario au démarrage
  // ----------------------------------------------------------

  jouerSon();


  // ----------------------------------------------------------
  // Lance le Snake
  // ----------------------------------------------------------

  newGame();
}


// ============================================================
// LOOP
// ============================================================

void loop() {

  for (int j = 0; j < NB_JOUEURS; j++) {
    readJoystick(j);
  }


  if (
    !gameOver && millis() - lastStep >= stepDelay) {

    lastStep = millis();

    step();


    if (gameOver) {

      showGameOver();
    }
  }
}
