// https://wokwi.com/projects/449649246302984193//
#include <EEPROM.h>
#include <LiquidCrystal.h>

// LCD (RS, E, D4, D5, D6, D7)
// LCD (RS, E, D4, D5, D6, D7)
// LCD (RS, E, D4, D5, D6, D7)
LiquidCrystal lcd(12, 13, 4, 5, 6, 7);

// A4988
// A4988
// A4988
const int stepPin = 9;   // PASOS. Cada pulso, avanza un paso.
const int dirPin = 8;    // DIRECCIÓN. 0 = Antihorario, 1 = Horario.
const int enablePin = 2; // ACTIVACION. 0 = Activo, 1 = Inactivo.
// const int ms1Pin = 9;    // TAMAÑO DE PASO. 0 = 1/16, 1 = 1/8, 2 = 1/4, 3 = 1/2, 4 = 1/1.
// const int ms2Pin = 10;   // TAMAÑO DE PASO. 0 = 1/16, 1 = 1/8, 2 = 1/4, 3 = 1/2, 4 = 1/1.
// const int ms3Pin = A3;   // TAMAÑO DE PASO. 0 = 1/16, 1 = 1/8, 2 = 1/4, 3 = 1/2, 4 = 1/1.

// VARIABLES MOTOR & INSULINA.
// VARIABLES MOTOR & INSULINA.
// VARIABLES MOTOR & INSULINA.
int curStep = 0;         // PASO ACTUAL.
const float reservoirCapacity = 300; // CONFIGURACION DE CAPACIDAD DE RESERVORIO.
float reservoirLevel = reservoirCapacity; // NIVEL DE RESERVORIO.
float stepUnits = 0.1; // UNIDADES DE INSULINA POR PASOS DE MOTOR.

// VARIABLES PERFIL BASAL.
// VARIABLES PERFIL BASAL.
// VARIABLES PERFIL BASAL.

const uint8_t BASAL_SLOTS = 48;
const uint8_t MIN_PER_SLOT = 30;
const uint16_t VALUE10_MAX = 100;

uint16_t basal10[BASAL_SLOTS];
uint8_t editingStartSlot = 0;
uint8_t editingEndSlot = 0;
uint16_t editingValue10 = 0;

void lcdPrintHHMM_fromSlot(uint8_t slot) {
  uint16_t minutes = (uint16_t)slot * MIN_PER_SLOT; // 0..1440
  uint8_t hh = minutes / 60;
  uint8_t mm = minutes % 60;

  if (hh < 10) lcd.print("0");
  lcd.print(hh);
  lcd.print(":");
  if (mm < 10) lcd.print("0");
  lcd.print(mm);
}

void nudgeEndSlot(int8_t dir) { // dir = +1 or -1
  int16_t next = (int16_t)editingEndSlot + dir;

  if (next < (int16_t)editingStartSlot) next = editingStartSlot;
  if (next > 48) next = 48;

  editingEndSlot = (uint8_t)next;
}

void lcdPrintValue10(uint16_t v10) {
  uint16_t whole = v10 / 10;
  uint8_t frac = v10 % 10;
  lcd.print(whole);
  lcd.print(".");
  lcd.print(frac);
}

void nudgeValue10(int8_t dir) { // dir = +1 or -1
  int32_t next = (int32_t)editingValue10 + dir;

  if (next < 0) next = 0;
  if (next > VALUE10_MAX) next = VALUE10_MAX;

  editingValue10 = (uint16_t)next;
}

bool commitRangeAndAdvance(bool resetValueToZero = true) {
  // Must be at least one 30-min slot
  if (editingEndSlot <= editingStartSlot) return false;

  // Fill basal10 slots in the range [start, end)
  // editingEndSlot can be 48; last valid index is 47
  for (uint8_t i = editingStartSlot; i < editingEndSlot; i++) {
    if (i < BASAL_SLOTS) basal10[i] = editingValue10;
  }

  // Advance start to end
  editingStartSlot = editingEndSlot;

  // Prepare for next segment
  editingEndSlot = editingStartSlot;

  if (resetValueToZero) editingValue10 = 0;

  return true;
}

// Botones
// Botones
// Botones
const int BTN_BACK = 10; // ESC  // Escape/ Atrás / Cancelar
const int BTN_OK = 11;   // ACT  // Confirmar / Entrar
const int BTN_UP = A5;   // UP   // Subir
const int BTN_DOWN = A1; // DOWN / Bajar

// Otros
// Otros
// Otros
const int buzzer = A2;     // Para alarmas
const int voltagePin = A0; // Batería simulada (potenciómetro)

// ===================== Timing de botones / Antirrebote =====================
// ===================== Timing de botones / Antirrebote =====================
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
// ===================== Alarmas audibles =====================
// ===================== Alarmas audibles =====================
void beepOK() { tone(buzzer, 1400, 80); }
void beepWarn() { tone(buzzer, 900, 120); }
void beepErr() { tone(buzzer, 500, 200); }

// ===================== Menú / Estados =====================
// ===================== Menú / Estados =====================
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
  ST_BASAL_EDIT_START,  // — Acceso a sub-sub-menú de edición de basal
  ST_BASAL_EDIT_END,  // — Acceso a sub-sub-menú de edición de basal
  ST_BASAL_EDIT_VALUE,  // — Acceso a sub-sub-menú de edición de basal
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
  lcd.print(reservoirLevel);
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
    lcd.print("REBOBINANDO...");
  } else if (state == ST_PRIME || state == ST_PRIMING) {
    if ( curStep == 0 ) {
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
  lcd.setCursor(0, 1);
  if ( state == ST_BASAL_EDIT_START ) {
    lcd.print("INICIO: ");
    lcdPrintHHMM_fromSlot(editingStartSlot);
  } else if ( state == ST_BASAL_EDIT_END ) {
    lcd.print("FIN: ");
    lcdPrintHHMM_fromSlot(editingEndSlot);
  } else if ( state == ST_BASAL_EDIT_VALUE ) {
    lcd.print("UNIDADES/HR: ");
    lcdPrintValue10(editingValue10);
   }else {
    lcd.print("OK PARA EDITAR");
  }
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

// ================ Motor Control =================
// ================ Motor Control =================
// ================ Motor Control =================
void motorAdvance(int direction) {
    if ( direction == 0 ) {
      digitalWrite(dirPin, LOW);    // anti Horario
      
      digitalWrite(stepPin, HIGH);
      delayMicroseconds(1000);
      digitalWrite(stepPin, LOW);
      delayMicroseconds(1000);
      
      curStep --;
      updateReservoir();
    } else if ( direction == 1 ) {
      digitalWrite(dirPin, HIGH);    // Horario
      
      digitalWrite(stepPin, HIGH);
      delayMicroseconds(10);
      digitalWrite(stepPin, LOW);
      delayMicroseconds(10);
      
      curStep ++;
      updateReservoir();
    }
}

// ================== Reservoir ===================
// ================== Reservoir ===================
// ================== Reservoir ===================

void updateReservoir() {
  reservoirLevel = reservoirCapacity - ( curStep * stepUnits );
}

// ===================== Setup =====================
// ===================== Setup =====================
// ===================== Setup =====================
void setup() {
  pinMode(stepPin, OUTPUT);
  pinMode(dirPin, OUTPUT);
  pinMode(enablePin, OUTPUT);
  //pinMode(ms1Pin, OUTPUT);
  //pinMode(ms2Pin, OUTPUT);
  //pinMode(ms3Pin, OUTPUT);

  pinMode(BTN_BACK, INPUT_PULLUP);
  pinMode(BTN_OK, INPUT_PULLUP);
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);

  pinMode(buzzer, OUTPUT);

  // Microstepping 1/16
  //digitalWrite(ms1Pin, HIGH);
  //digitalWrite(ms2Pin, HIGH);
  //analogWrite(ms3Pin, HIGH);
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

// ===================== Loop =====================
// ===================== Loop =====================
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
      if (curStep > 0.0) { // Si el motor está en mas de 0 pasos, debemos rebobinar.
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
    while ( curStep > 0 ) { // Mientras los pasos de motor sean mas de 0, mover el motor de regreso a 0.
      motorAdvance(0);
    }
    if ( curStep == 0 ) { // Si el motor ya está en 0, regresar al menú de cebado.
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
    if (pressedEdge(1)) { // OK
      state = ST_BASAL_EDIT_START;
      drawBasal();
    }
    if (pressedEdge(0)) { // BACK
      state = ST_MENU;
      drawMenu();
    }
  } break;

  case ST_BASAL_EDIT_START: {
    if (pressedEdge(1)) { // OK
      state = ST_BASAL_EDIT_END;
      drawBasal();
    }
    if (pressedEdge(0)) { // BACK
      state = ST_BASAL;
      drawBasal();
    }
  } break;

  case ST_BASAL_EDIT_END: {
    if (pressedEdge(1)) { // OK
      state = ST_BASAL_EDIT_VALUE;
      drawBasal();
    }

    if (pressedEdge(0)) { // BACK
      state = ST_BASAL_EDIT_START;
      drawBasal();
    }

    if (pressedEdge(2)) { // UP to Increase 30 minutes
      nudgeEndSlot(1);
      drawBasal();
    }

    if (pressedEdge(3)) { // DOWN to Decrease 30 minutes
      nudgeEndSlot(-1);
      drawBasal();
    }
  } break;

  case ST_BASAL_EDIT_VALUE: {
    if (pressedEdge(1)) { // OK
      commitRangeAndAdvance(false);
      
      if ( editingStartSlot >= 48 ) {
        state = ST_BASAL;
      } else {
        state = ST_BASAL_EDIT_START;
      }
      
      drawBasal();
    }

    if (pressedEdge(0)) { // BACK
      state = ST_BASAL_EDIT_END;
      drawBasal();
    }

    if (pressedEdge(2)) { // UP to Increase 30 minutes
      nudgeValue10(1);
      drawBasal();
    }

    if (pressedEdge(3)) { // DOWN to Decrease 30 minutes
      nudgeValue10(-1);
      drawBasal();
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