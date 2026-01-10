// https://wokwi.com/projects/452790095580616705//
#include <EEPROM.h>
#include <LiquidCrystal.h>

// LCD (RS, E, D4, D5, D6, D7)
// LCD (RS, E, D4, D5, D6, D7)
// LCD (RS, E, D4, D5, D6, D7)
LiquidCrystal lcd(12, 11, 5, 4, 3, 2);

// A4988
// A4988
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

// VARIABLES MOTOR & INSULINA.
// VARIABLES MOTOR & INSULINA.
// VARIABLES MOTOR & INSULINA.
int curStep = 0; // PASO ACTUAL.
const float reservoirCapacity =
    300; // CONFIGURACION DE CAPACIDAD DE RESERVORIO.
float reservoirLevel = reservoirCapacity; // NIVEL DE RESERVORIO.
float stepUnits = 0.1; // UNIDADES DE INSULINA POR PASOS DE MOTOR.

// VARIABLES PERFIL BASAL.
// VARIABLES PERFIL BASAL.
// VARIABLES PERFIL BASAL.

const uint8_t BASAL_SLOTS = 48;
const uint8_t MIN_PER_SLOT = 30;
const uint16_t VALUE10_MAX = 100; // 10.0 U/hr max

// "Cache" array for fast motor lookup.
uint16_t basal10[BASAL_SLOTS]; // 0..47

struct BasalSegment {
  uint8_t endSlot; // 1..48. The slot index where this segment ENDS.
                   // Previous segment ends at startSlot of this one.
                   // endSlot=48 means 24:00.
  uint16_t rate10;
};

// Fixed max segments.
BasalSegment segments[BASAL_SLOTS];
uint8_t segmentCount = 1;

// Editing state
uint8_t selectedSegmentIdx = 0;
bool isEditingTime = false; // true if editing time, false if editing rate

void lcdPrintHHMM_fromSlot(uint8_t slot) {
  uint16_t minutes = (uint16_t)slot * MIN_PER_SLOT; // 0..1440
  uint8_t hh = minutes / 60;
  uint8_t mm = minutes % 60;

  if (hh < 10)
    lcd.print("0");
  lcd.print(hh);
  lcd.print(":");
  if (mm < 10)
    lcd.print("0");
  lcd.print(mm);
}

void lcdPrintValue10(uint16_t v10) {
  uint16_t whole = v10 / 10;
  uint8_t frac = v10 % 10;
  lcd.print(whole);
  lcd.print(".");
  lcd.print(frac);
}

// Helper: Rebuild the fast lookup array from segments
void rebuildBasal10() {
  uint8_t currentSlot = 0;
  for (uint8_t i = 0; i < segmentCount; i++) {
    uint8_t end = segments[i].endSlot;
    // Safety clamp
    if (end > BASAL_SLOTS)
      end = BASAL_SLOTS;

    // Fill slots
    for (uint8_t s = currentSlot; s < end; s++) {
      basal10[s] = segments[i].rate10;
    }
    currentSlot = end;
  }
  // Fill remainder if any (shouldn't happen if logic is correct)
  for (uint8_t s = currentSlot; s < BASAL_SLOTS; s++) {
    basal10[s] = (segmentCount > 0) ? segments[segmentCount - 1].rate10 : 0;
  }
}

// Ensure profile covers 0..48
void validateAndFixProfile() {
  if (segmentCount == 0) {
    segmentCount = 1;
    segments[0].endSlot = 48;
    segments[0].rate10 = 0;
  }
  // Ensure last segment ends at 48
  if (segments[segmentCount - 1].endSlot != 48) {
    segments[segmentCount - 1].endSlot = 48;
  }
}

// Botones
// Botones
// Botones
const int BTN_BACK = 13; // ESC  // Escape/ Atrás / Cancelar
const int BTN_OK = A4;   // ACT  // Confirmar / Entrar
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
  ST_BASAL, // Acceso a sub-menú de configuración de perfiles basal. Muestra
            // Lista.
  ST_BASAL_EDIT_TIME, // Edición de hora fin de segmento
  ST_BASAL_EDIT_RATE, // Edición de tasa de segmento
  ST_BASAL_TEMP, // — Acceso a sub-sub-menú de configuración de basal temporal

  ST_CONFIG, // Acceso a menú de configuraciones
  ST_TIME,   // — Acceso a sub-menú de configuración de fecha
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

  if (state == ST_BASAL) {
    // List Mode
    // Calculate start time of selected segment
    uint8_t startSlot = 0;
    if (selectedSegmentIdx > 0) {
      startSlot = segments[selectedSegmentIdx - 1].endSlot;
    }

    lcd.setCursor(0, 0);
    lcd.print(selectedSegmentIdx + 1);
    lcd.print(". ");
    lcdPrintHHMM_fromSlot(startSlot);
    lcd.print("-");
    lcdPrintHHMM_fromSlot(segments[selectedSegmentIdx].endSlot);

    lcd.setCursor(0, 1);
    lcd.print("TASA: ");
    lcdPrintValue10(segments[selectedSegmentIdx].rate10);
    lcd.print(" U/h");

  } else if (state == ST_BASAL_EDIT_TIME) {
    lcd.setCursor(0, 0);
    lcd.print("EDITAR HORA FIN");
    lcd.setCursor(0, 1);
    lcdPrintHHMM_fromSlot(segments[selectedSegmentIdx].rate10);
    lcd.setCursor(6, 1);
    lcd.print("-");
    lcd.setCursor(8, 1);
    lcdPrintHHMM_fromSlot(segments[selectedSegmentIdx].endSlot);

  } else if (state == ST_BASAL_EDIT_RATE) {
    lcd.setCursor(0, 0);
    lcd.print("EDITAR TASA");
    lcd.setCursor(0, 1);
    lcdPrintValue10(segments[selectedSegmentIdx].rate10);
    lcd.print(" U/h");
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
  if (direction == 0) {
    digitalWrite(dirPin, LOW); // anti Horario

    digitalWrite(stepPin, HIGH);
    delayMicroseconds(1000);
    digitalWrite(stepPin, LOW);
    delayMicroseconds(1000);

    curStep--;
    updateReservoir();
  } else if (direction == 1) {
    digitalWrite(dirPin, HIGH); // Horario

    digitalWrite(stepPin, HIGH);
    delayMicroseconds(10);
    digitalWrite(stepPin, LOW);
    delayMicroseconds(10);

    curStep++;
    updateReservoir();
  }
}

// ================== Reservoir ===================
// ================== Reservoir ===================
// ================== Reservoir ===================

void updateReservoir() {
  reservoirLevel = reservoirCapacity - (curStep * stepUnits);
}

// ===================== Setup =====================
// ===================== Setup =====================
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

  // Initialize Profile
  validateAndFixProfile();
  rebuildBasal10();
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
    if (pressedEdge(1)) { // OK -> Edit Time
      state = ST_BASAL_EDIT_TIME;
      drawBasal();
    }
    if (pressedEdge(0)) { // BACK -> Menu
      state = ST_MENU;
      drawMenu();
    }
    if (pressedEdge(2)) { // UP -> Prev Segment
      if (selectedSegmentIdx > 0) {
        selectedSegmentIdx--;
        drawBasal();
      }
    }
    if (pressedEdge(3)) { // DOWN -> Next Segment
      if (selectedSegmentIdx < segmentCount - 1) {
        selectedSegmentIdx++;
        drawBasal();
      }
    }
  } break;

  case ST_BASAL_EDIT_TIME: {
    if (pressedEdge(1)) { // OK -> Next (Edit Rate)
      state = ST_BASAL_EDIT_RATE;
      drawBasal();
    }
    if (pressedEdge(0)) { // BACK -> List
      state = ST_BASAL;
      drawBasal();
    }

    if (pressedEdge(2)) { // UP -> Increase End Time
      // Constraint: Max 48
      if (segments[selectedSegmentIdx].endSlot < 48) {
        segments[selectedSegmentIdx].endSlot++;
        // Overlap check
        if (selectedSegmentIdx < segmentCount - 1) {
          // Check if we fully consumed the next segment
          if (segments[selectedSegmentIdx].endSlot >=
              segments[selectedSegmentIdx + 1].endSlot) {
            // Delete next segment
            for (int k = selectedSegmentIdx + 1; k < segmentCount - 1; k++) {
              segments[k] = segments[k + 1];
            }
            segmentCount--;
          }
          // Implicitly, next segment start is now pushed forward
        }
        validateAndFixProfile();
        rebuildBasal10();
        drawBasal();
      }
    }

    if (pressedEdge(3)) { // DOWN -> Decrease End Time
      // Constraint: Min time is previous end + 1 (min 30 min duration)
      uint8_t minTime = 0;
      if (selectedSegmentIdx > 0)
        minTime = segments[selectedSegmentIdx - 1].endSlot;

      if (segments[selectedSegmentIdx].endSlot > minTime + 1) {

        // Special Case: Shrinking the LAST segment creates a new one
        if (selectedSegmentIdx == segmentCount - 1) {
          if (segmentCount < BASAL_SLOTS) {
            // Create new segment at the end
            segments[segmentCount].endSlot = 48;
            segments[segmentCount].rate10 = 0;
            segmentCount++;

            // Now decrement current
            segments[selectedSegmentIdx].endSlot--;
          }
        } else {
          // Just decrement, next segment effectively grows
          segments[selectedSegmentIdx].endSlot--;
        }

        validateAndFixProfile();
        rebuildBasal10();
        drawBasal();
      }
    }
  } break;

  case ST_BASAL_EDIT_RATE: {
    if (pressedEdge(1)) { // OK -> Done (List)
      state = ST_BASAL;
      drawBasal();
    }
    if (pressedEdge(0)) { // BACK -> Edit Time
      state = ST_BASAL_EDIT_TIME;
      drawBasal();
    }

    if (pressedEdge(2)) { // UP
      if (segments[selectedSegmentIdx].rate10 < VALUE10_MAX) {
        segments[selectedSegmentIdx].rate10++;
        rebuildBasal10();
        drawBasal();
      }
    }
    if (pressedEdge(3)) { // DOWN
      if (segments[selectedSegmentIdx].rate10 > 0) {
        segments[selectedSegmentIdx].rate10--;
        rebuildBasal10();
        drawBasal();
      }
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