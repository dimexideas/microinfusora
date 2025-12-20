// https://wokwi.com/projects/449649246302984193//
#include <EEPROM.h>
#include <LiquidCrystal.h>

// ===================== Hardware =====================
// LCD (RS, E, D4, D5, D6, D7)
LiquidCrystal lcd(12, 11, 5, 4, 3, 2);

// A4988
const int stepPin = 6;   // PASOS. Cada pulso, avanza un paso.
const int dirPin = 7;    // DIRECCIÓN. 0 = Antihorario, 1 = Horario.
const int enablePin = 8; // ACTIVACION. 0 = Activo, 1 = Inactivo.
const int ms1Pin =
    9; // TAMAÑO DE PASO. 0 = 1/16, 1 = 1/8, 2 = 1/4, 3 = 1/2, 4 = 1/1.
const int ms2Pin =
    10; // TAMAÑO DE PASO. 0 = 1/16, 1 = 1/8, 2 = 1/4, 3 = 1/2, 4 = 1/1.
const int ms3Pin =
    A3; // TAMAÑO DE PASO. 0 = 1/16, 1 = 1/8, 2 = 1/4, 3 = 1/2, 4 = 1/1.
int curStep = 0; // PASO ACTUAL.

// Botones
const int BTN_BACK = 13; // ESC  // Escape/ Atrás / Cancelar
const int BTN_OK = A4;   // ACT  // Confirmar / Entrar
const int BTN_UP = A5;   // UP   // Subir
const int BTN_DOWN = A1; // DOWN / Bajar

// Otros
const int buzzer = A2;     // Para alarmas
const int voltagePin = A0; // Batería simulada (potenciómetro)

// ===================== Timing de botones / Antirrebote =====================
struct Btn {
  uint8_t pin;
  bool last;
  uint32_t lastChange;
};

Btn btns[4] = {{BTN_BACK, true, 0},
               {BTN_OK, true, 0},
               {BTN_UP, true, 0},
               {BTN_DOWN, true, 0}};

const uint16_t DEBOUNCE_MS = 100;
const uint16_t REPEAT_START_MS = 0;
const uint16_t REPEAT_RATE_MS = 0;

bool pressedEdge(uint8_t index) {
  bool now = digitalRead(btns[index].pin);
  uint32_t t = millis();
  if (now != btns[index].last && (t - btns[index].lastChange) > DEBOUNCE_MS) {
    btns[index].last = now;
    btns[index].lastChange = t;
    return (now == LOW); // Botones con PULLUP: LOW = presionado
  }
  return false;
}

bool heldRepeat(uint8_t index) {
  // auto-repetición cuando se mantiene presionado
  static uint32_t startHeld[4] = {0, 0, 0, 0};
  static uint32_t lastRpt[4] = {0, 0, 0, 0};
  bool now = (digitalRead(btns[index].pin) == LOW);
  uint32_t t = millis();
  if (pressedEdge(index)) {
    startHeld[index] = t;
    lastRpt[index] = t;
    return true;
  }
  if (now) {
    if (t - startHeld[index] > REPEAT_START_MS &&
        t - lastRpt[index] > REPEAT_RATE_MS) {
      lastRpt[index] = t;
      return true;
    }
  }
  return false;
}

// ===================== Alarmas audibles =====================
void beepOK() { tone(buzzer, 1400, 80); }
void beepWarn() { tone(buzzer, 900, 120); }
void beepErr() { tone(buzzer, 500, 200); }

// ===================== Menú / Estados =====================
enum State {
  ST_HOME,          // Pantalla muestra fecha, iconos de que esta sucediendo.
  ST_MENU,          // Acceso a menú de opciones
  ST_PRIME,         // Acceso a menú de cebado
  ST_PRIMING,       // — Acceso a sub-menú de cebado
  ST_PRIMING_START, // — — Acceso a sub-menú de cebado iniciar ( rebobina el
                    // motor completamente )
  ST_REWINDING,     // — — Acceso a sub-sub-menú de rebobinado
  ST_BOLUS,         // Acceso a menú de bolos
  ST_BOLUS_MANUAL,  // — Acceso a sub-menú de bolo manual
  ST_BOLUS_MANUAL_EXTDUR, // — Acceso a sub-menú de bolo cuadrado
  ST_BOLUS_CANCEL,        // — Acceso a sub-menú de cancelación de bolo cuadrado
  ST_BASAL,             // Acceso a sub-menú de configuración de perfiles basal
  ST_BASAL_CONF_SELECT, // — Acceso a sub-sub-menú de configuración de basal de
                        // seleccionar basales.
  ST_BASAL_TEMP, // — Acceso a sub-sub-menú de configuración de basal temporal
                 // (porcentaje arriba / abajo de basal por X tiempo )
  ST_BASAL_CONF, // — Acceso a sub-sub-menú de configuración de basal ( de hora
                 // A a hora B que cantidad de insulina U/hr )
  ST_BASAL_CONF_ADD,  // — — Acceso a sub-sub-menú de configuración de basal de
                      // agregar basales.
  ST_BASAL_CONF_EDIT, // — — Acceso a sub-sub-menú de configuración de basal de
                      // editar basales.
  ST_CONFIG,          // Acceso a menú de configuraciones
  ST_TIME,            // — Acceso a sub-menú de configuración de fecha
  ST_LIMITS, // — Acceso a sub-menú de configuración de máximos de insulina (
             // bolos, etc )
  ST_COMMS,  // — Acceso a sub-menú de configuración de comunicación bluetooth /
             // otros.
  ST_SUSPEND_SELECT, // Acceso a menú de suspender todo.
  ST_SUSPEND,        // — Acceso a sub-menú de suspender.
  ST_CONTINUE        // — Acceso a sub-menú de reanudar.
};
State state = ST_HOME;

int menuIndex = 0;
const int MENU_COUNT = 5;

const char *items[MENU_COUNT] = {"CEBADO", "BOLO", "BASAL", "AJUSTES",
                                 "SUSPENDER"};

const State base_states[MENU_COUNT] = {ST_PRIME, ST_BOLUS, ST_BASAL, ST_CONFIG,
                                       ST_SUSPEND};

void drawHome() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("BAT ");
  lcd.print("100%");
  lcd.print(" ");
  lcd.print(300.0);
  lcd.print("U");

  // TODO: AGREGAR FECHA & HORA
}

void drawMenu() {
  lcd.clear();
  int next = (menuIndex + 1) % MENU_COUNT;
  lcd.setCursor(0, 0);
  lcd.print(">");
  lcd.print(items[menuIndex]);
  lcd.setCursor(0, 1);
  lcd.print(" ");
  lcd.print(items[next]);
}

void drawPrime() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("CEBADO");
  lcd.setCursor(0, 1);
  if (state == ST_REWINDING) {
    lcd.print("REBOBINANDO");
  } else if (state == ST_PRIME || state == ST_PRIMING) {
    if (curStep == 0) {
      lcd.print("OK INICIA");
    } else {
      lcd.print("OK REBOBINA");
    }
  } else if (state == ST_PRIMING_START) {
    lcd.print("CEBANDO...");
  } else {
    lcd.print(state);
  }
}

void drawBolus() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("BOLO");
}

void drawBasal() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("BASAL");
}

void drawConfig() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("AJUSTES");
}

void drawSuspend() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("SUSPENDER");
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
  pinMode(BTN_OK, INPUT_PULLUP);
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);

  pinMode(buzzer, OUTPUT);

  // Microstepping 1/16
  digitalWrite(ms1Pin, HIGH);
  digitalWrite(ms2Pin, HIGH);
  analogWrite(ms3Pin, HIGH);
  digitalWrite(enablePin, LOW); // Activo

  lcd.begin(16, 2);
  lcd.print("MICROINFUSORA");
  lcd.setCursor(0, 1);
  lcd.print("INICIANDO...");

  if (curStep > 0.0) {
    state = ST_PRIME;
    drawPrime();
  } else {
    state = ST_HOME;
    drawHome();
  }
}

// ================ Motor Control =================

void motorAdvance(int direction) {
  if (direction == 0) {
    digitalWrite(dirPin, LOW); // anti Horario

    digitalWrite(stepPin, HIGH);
    delayMicroseconds(1000);
    digitalWrite(stepPin, LOW);
    delayMicroseconds(1000);

    curStep--;
  } else if (direction == 1) {
    digitalWrite(dirPin, HIGH); // Horario

    digitalWrite(stepPin, HIGH);
    delayMicroseconds(10);
    digitalWrite(stepPin, LOW);
    delayMicroseconds(10);

    curStep++;
  }
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
      menuIndex = 0;
      drawHome();
    }
    // Scroll para refrescar pantalla
    if (heldRepeat(2) || heldRepeat(3)) {
      drawHome();
    }
  } break;

  case ST_MENU: {
    if (pressedEdge(2)) { // UP
      if (menuIndex == 0) {
        menuIndex = MENU_COUNT - 1;
      } else {
        menuIndex = (menuIndex - 1);
      }
      drawMenu();
    }

    if (pressedEdge(3)) { // DOWN
      if (menuIndex == MENU_COUNT - 1) {
        menuIndex = 0;
      } else {
        menuIndex = (menuIndex + 1);
      }
      drawMenu();
    }

    if (pressedEdge(1)) { // OK
      state = static_cast<State>(base_states[menuIndex]);

      if (state == ST_PRIME)
        drawPrime();
      if (state == ST_BOLUS)
        drawBolus();
      if (state == ST_BASAL)
        drawBasal();
      if (state == ST_CONFIG)
        drawConfig();
      if (state == ST_SUSPEND)
        drawSuspend();
    }

    if (pressedEdge(0)) { // BACK
      state = ST_HOME;
      drawHome();
    }

  } break;

  case ST_PRIME: {
    if (pressedEdge(1)) { // OK
      if (curStep >
          0.0) { // Si el motor está en mas de 0 pasos, debemos rebobinar.
        state = ST_REWINDING;
      } else { // Si el motor está en 0 pasos, debemos cebar.
        state = ST_PRIMING;
      }
      drawPrime();
    }
    if (pressedEdge(0)) { // BACK
      state = ST_MENU;
      drawMenu();
    }
  } break;

  case ST_PRIMING: {
    if (heldRepeat(1)) { // HOLD OK para cebar.
      state = ST_PRIMING_START;
      drawPrime();
    }
    if (pressedEdge(0)) { // BACK para salir de cebado.
      state = ST_PRIME;
      drawPrime();
    }
  } break;

  case ST_PRIMING_START: {
    if (heldRepeat(1)) { // HOLD OK para confirmar cebado y mover motor.
      motorAdvance(1);
      drawPrime();
    }

    if (pressedEdge(0)) { // BACK para salir de cebado.
      state = ST_PRIME;
      drawPrime();
    }
  } break;

  case ST_REWINDING: {
    while (curStep > 0) { // Mientras los pasos de motor sean mas de 0, mover el
                          // motor de regreso a 0.
      motorAdvance(0);
    }
    if (curStep == 0) { // Si el motor ya está en 0, regresar al menú de cebado.
      state = ST_PRIME;
      drawPrime();
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