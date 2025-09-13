//https://wokwi.com/projects/441378105660326913//
#include <LiquidCrystal.h>
#include <EEPROM.h>

// ===================== Hardware =====================
// LCD (RS, E, D4, D5, D6, D7)
LiquidCrystal lcd(12, 11, 5, 4, 3, 2);

// A4988
const int stepPin   = 6;
const int dirPin    = 7;
const int enablePin = 8;
const int ms1Pin = 9;
const int ms2Pin = 10;
const int ms3Pin = A3;

// Botones
const int BTN_BACK   = 13;  // Escape/Atrás/Cancelar
const int BTN_OK     = A4;  // Confirmar/Entrar
const int BTN_UP     = A5;  // Subir
const int BTN_DOWN   = A1;  // Bajar

// Otros
const int buzzer = A2;      // Para alarmas
const int voltagePin = A0;  // Batería simulada (potenciómetro)

// ===================== Timing de botones / Antirrebote =====================
struct Btn {
  uint8_t pin;
  bool last;
  uint32_t lastChange;
};

Btn btns[4] = {
  {BTN_BACK, true, 0},
  {BTN_OK,   true, 0},
  {BTN_UP,   true, 0},
  {BTN_DOWN, true, 0}
};

const uint16_t DEBOUNCE_MS = 100;
const uint16_t REPEAT_START_MS = 500;
const uint16_t REPEAT_RATE_MS  = 200;

bool pressedEdge(uint8_t index) {
  bool now = digitalRead(btns[index].pin);
  uint32_t t = millis();
  if (now != btns[index].last && (t - btns[index].lastChange) > DEBOUNCE_MS) {
    btns[index].last = now;
    btns[index].lastChange = t;
    return (now == LOW);    // Botones con PULLUP: LOW = presionado
  }
  return false;
}

bool heldRepeat(uint8_t index) {
  // auto-repetición cuando se mantiene presionado
  static uint32_t startHeld[4] = {0,0,0,0};
  static uint32_t lastRpt[4]   = {0,0,0,0};
  bool now = (digitalRead(btns[index].pin) == LOW);
  uint32_t t = millis();
  if (pressedEdge(index)) { startHeld[index] = t; lastRpt[index]=t; return true; }
  if (now) {
    if (t - startHeld[index] > REPEAT_START_MS && t - lastRpt[index] > REPEAT_RATE_MS) {
      lastRpt[index] = t;
      return true;
    }
  }
  return false;
}

// ===================== Alarmas audibles =====================
void beepOK()   { tone(buzzer, 1400, 80); }
void beepWarn() { tone(buzzer, 900,  120); }
void beepErr()  { tone(buzzer, 500,  200); }

// ===================== Menú / Estados =====================
enum State {
  ST_HOME,                  // Pantalla muestra fecha, iconos de que esta sucediendo.
  ST_MENU,                  // Acceso a menú de opciones
  ST_PRIME,                 // Acceso a menú de cebado
  ST_PRIMING,               // — Acceso a sub-menú de cebado
  ST_PRIMING_START,         // — — Acceso a sub-menú de cebado iniciar ( rebobina el motor completamente )
  ST_PRIMING_END,           // — — Acceso a sub-sub-menú de terminación de cebado ( una vez rebobinado, te permite cebar para llenar la línea )
  ST_BOLUS,                 // Acceso a menú de bolos
  ST_BOLUS_MANUAL,          // — Acceso a sub-menú de bolo manual
  ST_BOLUS_MANUAL_EXTDUR,   // — Acceso a sub-menú de bolo cuadrado
  ST_BOLUS_CANCEL,          // — Acceso a sub-menú de cancelación de bolo cuadrado
  ST_BASAL,                 // Acceso a sub-menú de configuración de perfiles basal
  ST_BASAL_CONF_SELECT,     // — Acceso a sub-sub-menú de configuración de basal de seleccionar basales.
  ST_BASAL_TEMP,            // — Acceso a sub-sub-menú de configuración de basal temporal ( porcentaje arriba / abajo de basal por X tiempo )
  ST_BASAL_CONF,            // — Acceso a sub-sub-menú de configuración de basal ( de hora A a hora B que cantidad de insulina U/hr )
  ST_BASAL_CONF_ADD,        // — — Acceso a sub-sub-menú de configuración de basal de agregar basales.
  ST_BASAL_CONF_EDIT,       // — — Acceso a sub-sub-menú de configuración de basal de editar basales.
  ST_CONFIG,                // Acceso a menú de configuraciones
  ST_TIME,                  // — Acceso a sub-menú de configuración de fecha
  ST_LIMITS,                // — Acceso a sub-menú de configuración de máximos de insulina ( bolos, etc )
  ST_COMMS,                 // — Acceso a sub-menú de configuración de comunicación bluetooth / otros.
  ST_SUSPEND_SELECT,        // Acceso a menú de suspender todo.
  ST_SUSPEND,               // — Acceso a sub-menú de suspender.
  ST_CONTINUE               // — Acceso a sub-menú de reanudar.
};
State state = ST_HOME;

int menuIndex = 0;
const int MENU_COUNT = 5;

const char* items[MENU_COUNT] = {
  "CEBADO",
  "BOLO",
  "BASAL",
  "AJUSTES",
  "SUSPENDER"
};

const State base_states[MENU_COUNT] = {
  ST_PRIME,
  ST_BOLUS,
  ST_BASAL,
  ST_CONFIG,
  ST_SUSPEND
};

void drawHome() {
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("BAT ");
  lcd.print("100%");
  lcd.print(" ");
  lcd.print(300.0);
  lcd.print("U");

  // TODO: AGREGAR FECHA & HORA
}

void drawMenu() {
  lcd.clear();
  int next = (menuIndex+1) % MENU_COUNT;
  lcd.setCursor(0,0); lcd.print(">");
  lcd.print(items[menuIndex]);
  lcd.setCursor(0,1); lcd.print(" ");
  lcd.print(items[next]);
}

void drawPrime() {
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("CEBADO");
}

void drawBolus() {
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("BOLO");
}

void drawBasal() {
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("BASAL");
}

void drawConfig() {
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("AJUSTES");
}

void drawSuspend() {
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("SUSPENDER");
}

// ===================== Setup =====================
void setup() {
  pinMode(stepPin, OUTPUT);
  pinMode(dirPin, OUTPUT);
  pinMode(enablePin, OUTPUT);
  pinMode(ms1Pin, OUTPUT);
  pinMode(ms2Pin, OUTPUT);
  pinMode(ms3Pin, OUTPUT);

  pinMode(BTN_BACK, INPUT_PULLUP);
  pinMode(BTN_OK,   INPUT_PULLUP);
  pinMode(BTN_UP,   INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);

  pinMode(buzzer, OUTPUT);

  // Microstepping 1/16
  digitalWrite(ms1Pin, HIGH);
  digitalWrite(ms2Pin, HIGH);
  digitalWrite(ms3Pin, HIGH);

  lcd.begin(16,2);
  lcd.print("MICROINFUSORA");
  lcd.setCursor(0,1); lcd.print("INICIANDO...");
  delay(900);
  drawHome();
}

// ===================== Loop =====================
void loop() {
    switch (state) {
        case ST_HOME: {
          // Entrar a menú
          if (pressedEdge(1)) { // OK
            state = ST_MENU;
            menuIndex = 0;
            drawMenu();
          }
          // Cancelación rápida
          if (pressedEdge(0)) {
            state = ST_HOME;
            drawHome();
          }
          // Scroll para refrescar pantalla
          if (heldRepeat(2) || heldRepeat(3)) {
            drawHome();
          }
        } break;

        case ST_MENU: {
          if (heldRepeat(2)) { // UP
            menuIndex = (menuIndex - 1 + MENU_COUNT) % MENU_COUNT;
            drawMenu();
          }
          
          if (heldRepeat(3)) { // DOWN
            menuIndex = (menuIndex + 1) % MENU_COUNT;
            drawMenu();
          }

          if (pressedEdge(1)) { // OK
            state = static_cast<State>(base_states[menuIndex]);

            if ( state == ST_PRIME ) {
              drawPrime();
            }
            if ( state == ST_BOLUS ) {
              drawBolus();
            }
            if ( state == ST_BASAL ) {
              drawBasal();
            }
            if ( state == ST_CONFIG ) {
              drawConfig();
            }
            if ( state == ST_SUSPEND ) {
              drawSuspend();
            }
          }

          if (pressedEdge(0)) { // BACK
            state = ST_HOME; 
            drawHome();
          }

        } break;

        case ST_PRIME: {          
          if (pressedEdge(0)) { // BACK
            state = ST_MENU; 
            drawMenu();
          }
        } break;

        case ST_BOLUS: {
          if (pressedEdge(0)) { // BACK
            state = ST_MENU; 
            drawMenu();
          }
        } break;

        case ST_BASAL: {
          if (pressedEdge(0)) { // BACK
            state = ST_MENU; 
            drawMenu();
          }
        } break;

        case ST_CONFIG: {
          if (pressedEdge(0)) { // BACK
            state = ST_MENU; 
            drawMenu();
          }
        } break;

        case ST_SUSPEND: {
          if (pressedEdge(0)) { // BACK
            state = ST_MENU; 
            drawMenu();
          }
        } break;

    }
}