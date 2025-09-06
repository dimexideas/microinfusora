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
const int BTN_BACK   = 13; // Escape/Atrás/Cancelar
const int BTN_OK     = A4; // Confirmar/Entrar
const int BTN_UP     = A5; // Subir
const int BTN_DOWN   = A1; // Bajar

// Otros
const int buzzer = A2;
const int voltagePin = A0; // batería simulada (potenciómetro)

// ===================== Parametría =====================
const int MICROSTEP = 16;            // 1/16 microstepping
const int BASE_STEPS_PER_U = 400;    // pasos por U a paso completo
const int STEPS_PER_U = BASE_STEPS_PER_U * MICROSTEP;

const uint32_t MIN_PULSE_US = 2;     // ancho mínimo de pulso (A4988 ~>1us)
const uint32_t BOLUS_SPEED_US = 900; // intervalo entre pasos para bolo rápido (~1.1k pps)
const uint32_t PRIME_SPEED_US = 1200;
const uint32_t MAX_PRIME_STEPS = 2000; // tope seguridad cebado

// Basal
float basalRate = 0.8f;             // U/h
// Límites
float bolusLimitPerDose = 10.0f;    // U máx por bolo
float bolusDailyLimit   = 50.0f;    // U máx por día (desde arranque)

// Simulación insulina/batería
float insulinRemaining = 300.0f;     // U
float batteryVoltage = 4.2f;         // Calculado desde A0
int   batteryPercent = 100;

// ===================== Persistencia EEPROM =====================
// (usa enteros escalados para evitar float en EEPROM)
const int EE_ADDR_BASAL_10X        = 0;   // uint16 (basal*10)
const int EE_ADDR_LIMIT_DOSE_10X   = 2;   // uint16
const int EE_ADDR_LIMIT_DAILY_10X  = 4;   // uint16

// ===================== Timing / Antirrebote =====================
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
const uint16_t DEBOUNCE_MS = 35;
const uint16_t REPEAT_START_MS = 500;
const uint16_t REPEAT_RATE_MS  = 120;

bool pressedEdge(uint8_t index) {
  bool now = digitalRead(btns[index].pin);
  uint32_t t = millis();
  if (now != btns[index].last && (t - btns[index].lastChange) > DEBOUNCE_MS) {
    btns[index].last = now;
    btns[index].lastChange = t;
    // Botones con PULLUP: LOW = presionado
    return (now == LOW);
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

// ===================== Motor no bloqueante =====================
struct MotorJob {
  bool active = false;
  uint32_t stepIntervalUs = 1000;
  uint32_t lastStepUs = 0;
  uint32_t stepsRemaining = 0;
  uint8_t  type = 0; // 0=none,1=PRIME,2=BOLUS_FAST,3=BOLUS_EXT,4=BASAL
};
MotorJob job;

void motorEnable(bool en) { digitalWrite(enablePin, en ? LOW : HIGH); } // A4988 EN activo bajo
void motorPulse() {
  // Un pulso (HIGH->LOW). En Arduino, la pareja suele durar >1us sin delay
  digitalWrite(stepPin, HIGH);
  // delayMicroseconds(MIN_PULSE_US); // opcional
  digitalWrite(stepPin, LOW);
}
void startJob(uint8_t type, uint32_t steps, uint32_t intervalUs) {
  job.type = type;
  job.stepsRemaining = steps;
  job.stepIntervalUs = intervalUs;
  job.lastStepUs = micros();
  job.active = (steps > 0);
  motorEnable(true);
  digitalWrite(dirPin, HIGH);
}
void stopJob() {
  job.active = false;
  job.stepsRemaining = 0;
  job.type = 0;
  motorEnable(false);
}
void serviceMotor() {
  if (!job.active) return;
  uint32_t now = micros();
  if (now - job.lastStepUs >= job.stepIntervalUs) {
    motorPulse();
    job.lastStepUs = now;
    if (job.stepsRemaining > 0) {
      job.stepsRemaining--;
      if (job.stepsRemaining == 0) {
        stopJob();
      }
    }
  }
}

// ===================== Basal distribuida =====================
struct BasalTask {
  bool enabled = false;
  uint32_t stepIntervalUs = 0; // calculado desde basalRate
  uint32_t lastStepUs = 0;
} basalTask;

void recalcBasal() {
  if (basalRate <= 0.0f) { basalTask.enabled=false; return; }
  // pasos/hora = basalRate * STEPS_PER_U
  // intervalo por paso (us) = 3600e6 / pasosHora
  double stepsPerHour = (double)basalRate * (double)STEPS_PER_U;
  if (stepsPerHour < 1.0) stepsPerHour = 1.0;
  basalTask.stepIntervalUs = (uint32_t)(3600000000.0 / stepsPerHour);
  basalTask.enabled = true;
  basalTask.lastStepUs = micros();
}

void serviceBasal() {
  if (!basalTask.enabled || job.active) return; // no competir con un bolo/cebado
  uint32_t now = micros();
  if (now - basalTask.lastStepUs >= basalTask.stepIntervalUs) {
    // entregar 1 paso
    motorEnable(true);
    digitalWrite(dirPin, HIGH);
    motorPulse();
    basalTask.lastStepUs = now;
    // contabilidad de insulina
    insulinRemaining -= (1.0f / (float)STEPS_PER_U);
  }
}

// ===================== Bolo extendido / automático =====================
struct ExtendedBolus {
  bool active=false;
  uint32_t totalSteps=0;
  uint32_t stepsDelivered=0;
  uint32_t stepIntervalUs=0;
  uint32_t lastStepUs=0;
  uint32_t endMillis=0;
  bool repeating=false;
  uint32_t repeatEveryMs=0;
} ext;

void startExtended(float units, uint32_t durationMin) {
  if (units <= 0) return;
  uint32_t steps = (uint32_t)(units * STEPS_PER_U + 0.5f);
  if (steps == 0) return;
  uint64_t totalUs = (uint64_t)durationMin * 60000000ULL;
  uint32_t intervalUs = (uint32_t)(totalUs / steps);
  ext.active=true;
  ext.totalSteps = steps;
  ext.stepsDelivered = 0;
  ext.stepIntervalUs = (intervalUs == 0 ? 1 : intervalUs);
  ext.lastStepUs = micros();
  ext.endMillis = millis() + durationMin*60000UL;
}

void serviceExtended() {
  if (!ext.active || job.active) return; // no solapar con job rápido
  uint32_t nowUs = micros();
  if (nowUs - ext.lastStepUs >= ext.stepIntervalUs) {
    motorEnable(true);
    digitalWrite(dirPin, HIGH);
    motorPulse();
    ext.lastStepUs = nowUs;
    ext.stepsDelivered++;
    insulinRemaining -= (1.0f / (float)STEPS_PER_U);
    if (ext.stepsDelivered >= ext.totalSteps) {
      ext.active = false;
      motorEnable(false);
      if (ext.repeating && ext.repeatEveryMs > 0) {
        // reprogramar
        startExtended((float)ext.totalSteps/(float)STEPS_PER_U, (ext.repeatEveryMs/60000UL));
      }
    }
  }
}

void cancelAnyInfusion() {
  stopJob();
  ext.active = false;
  motorEnable(false);
  tone(buzzer, 1900, 120);
}

// ===================== Utilidades =====================
void beepOK()   { tone(buzzer, 1400, 80); }
void beepWarn() { tone(buzzer, 900,  120); }
void beepErr()  { tone(buzzer, 500,  200); }

void readBattery() {
  int v = analogRead(voltagePin);
  // Simula 3.0V .. 4.2V en el potenciómetro
  batteryVoltage = map(v, 0, 1023, 300, 420)/100.0;
  batteryPercent = map(v, 0, 1023, 0, 100);
  if (batteryPercent < 0) batteryPercent=0;
  if (batteryPercent > 100) batteryPercent=100;
}

// ===================== Menú / Estados =====================
enum State {
  ST_HOME,
  ST_MENU,
  ST_PRIME,
  ST_BOLUS_MANUAL,
  ST_BOLUS_MANUAL_EXTDUR,
  ST_BOLUS_CANCEL,
  ST_LIMITS,
  ST_BASAL,
  ST_AUTO_PROG,         // “X U en Y min, repetir cada Z”
  ST_CONFIRM_START
};
State state = ST_HOME;

int menuIndex = 0;
const int MENU_COUNT = 7;

float tmpUnits = 1.0f;   // buffer para selección de U
int   tmpMinutes = 10;   // buffer para selección de minutos
int   tmpRepeatMin = 0;  // 0 = no repetir

// Estadística simple
float dailyDelivered = 0.0f;

void drawHome() {
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Bat ");
  lcd.print(batteryPercent);
  lcd.print("%  Ins ");
  if (insulinRemaining < 0) insulinRemaining = 0;
  lcd.print((int)insulinRemaining);
  lcd.print("U");
  lcd.setCursor(0,1);
  lcd.print(ext.active ? "EXBolus ON   " : (job.active ? "Bolo/Cebado  " : "Listo         "));
}

void drawMenu() {
  lcd.clear();
  const char* items[MENU_COUNT] = {
    "Cebado",
    "Bolo manual",
    "Cancelar bolo",
    "Limitar bolo",
    "Infusion basal",
    "Auto programar",
    "Salir"
  };
  // Mostrar 2 líneas con cursor
  int next = (menuIndex+1) % MENU_COUNT;
  lcd.setCursor(0,0); lcd.print(">");
  lcd.print(items[menuIndex]);
  lcd.setCursor(0,1); lcd.print(" ");
  lcd.print(items[next]);
}

void eepromLoad() {
  uint16_t b10 = 0, l110=0, l210=0;
  EEPROM.get(EE_ADDR_BASAL_10X, b10);
  EEPROM.get(EE_ADDR_LIMIT_DOSE_10X, l110);
  EEPROM.get(EE_ADDR_LIMIT_DAILY_10X, l210);
  if (b10 > 0 && b10 < 500) basalRate = b10 / 10.0f;
  if (l110 >= 5 && l110 <= 500) bolusLimitPerDose = l110 / 10.0f;
  if (l210 >= 10 && l210 <= 1000) bolusDailyLimit = l210 / 10.0f;
  recalcBasal();
}

void eepromSave() {
  uint16_t b10 = (uint16_t)(basalRate*10.0f + 0.5f);
  uint16_t l110= (uint16_t)(bolusLimitPerDose*10.0f + 0.5f);
  uint16_t l210= (uint16_t)(bolusDailyLimit*10.0f + 0.5f);
  EEPROM.put(EE_ADDR_BASAL_10X,       b10);
  EEPROM.put(EE_ADDR_LIMIT_DOSE_10X,  l110);
  EEPROM.put(EE_ADDR_LIMIT_DAILY_10X, l210);
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
  motorEnable(false);

  lcd.begin(16,2);
  lcd.print("Microinfusora");
  lcd.setCursor(0,1); lcd.print("Iniciando...");
  delay(900);
  eepromLoad();
  drawHome();
}

// ===================== Loop =====================
void loop() {
  // servicios en background
  readBattery();
  serviceMotor();
  serviceExtended();
  serviceBasal();

  // bloqueo por batería crítica
  if (batteryPercent < 5 || batteryVoltage < 3.30) {
    cancelAnyInfusion();
    lcd.setCursor(0,1);
    lcd.print("Bateria critica");
    delay(5);
    return;
  }

  // ---- Navegación / estados ----
  switch (state) {
    case ST_HOME: {
      // Entrar a menú
      if (pressedEdge(1)) { // OK
        state = ST_MENU;
        menuIndex = 0;
        drawMenu();
        beepOK();
      }
      // Cancelación rápida
      if (pressedEdge(0)) {
        cancelAnyInfusion();
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
        beepOK();
        switch (menuIndex) {
          case 0: state = ST_PRIME; break;
          case 1: state = ST_BOLUS_MANUAL; tmpUnits = 1.0f; break;
          case 2: state = ST_BOLUS_CANCEL; break;
          case 3: state = ST_LIMITS; break;
          case 4: state = ST_BASAL; break;
          case 5: state = ST_AUTO_PROG; tmpUnits=1.0f; tmpMinutes=10; tmpRepeatMin=0; break;
          case 6: state = ST_HOME; drawHome(); break;
        }
      }
      if (pressedEdge(0)) { // BACK
        state = ST_HOME; drawHome();
      }
    } break;

    case ST_PRIME: {
      // Cebado configurable por pasos (U) o tiempo
      static bool asUnits = true; // alterna con OK
      static float primeUnits = 0.5f;
      static int   primeSecs  = 10;

      lcd.setCursor(0,0);
      lcd.print("Cebado ");
      lcd.print(asUnits ? "Unid" : "Tiempo");
      lcd.setCursor(0,1);
      if (asUnits) {
        lcd.print(primeUnits,1); lcd.print("U  OK=Go ");
      } else {
        lcd.print(primeSecs); lcd.print("s  OK=Go ");
      }

      if (heldRepeat(2)) { // UP
        if (asUnits) { if (primeUnits < 5.0f) primeUnits += 0.1f; }
        else          { if (primeSecs  < 60)   primeSecs  += 1;   }
        lcd.setCursor(0,1); lcd.print("                ");
      }
      if (heldRepeat(3)) { // DOWN
        if (asUnits) { if (primeUnits > 0.1f) primeUnits -= 0.1f; }
        else          { if (primeSecs  > 1)    primeSecs  -= 1;   }
        lcd.setCursor(0,1); lcd.print("                ");
      }
      if (pressedEdge(1)) { // OK -> iniciar
        uint32_t steps = asUnits ? (uint32_t)(primeUnits*STEPS_PER_U+0.5f)
                                 : (uint32_t)((1000000UL*primeSecs)/PRIME_SPEED_US);
        if (steps > MAX_PRIME_STEPS) steps = MAX_PRIME_STEPS;
        if (insulinRemaining < (float)steps/(float)STEPS_PER_U) { beepErr(); break; }
        startJob(1, steps, PRIME_SPEED_US);
        insulinRemaining -= (float)steps/(float)STEPS_PER_U;
        dailyDelivered   += (float)steps/(float)STEPS_PER_U;
        state = ST_HOME; drawHome();
      }
      if (pressedEdge(0)) { // BACK
        state = ST_MENU; drawMenu();
      }
      // alterna unidades/tiempo con pulsación larga OK
      static uint32_t lastOk = 0;
      if (digitalRead(BTN_OK)==LOW) {
        if (millis()-lastOk>700) { asUnits = !asUnits; lastOk=millis(); lcd.clear(); }
      } else lastOk=millis();
    } break;

    case ST_BOLUS_MANUAL: {
      // Paso 1: elegir unidades
      lcd.setCursor(0,0); lcd.print("Bolo manual U:");
      lcd.setCursor(0,1); lcd.print("  "); lcd.print(tmpUnits,1); lcd.print("U");
      if (heldRepeat(2)) { if (tmpUnits < bolusLimitPerDose) tmpUnits += 0.1f; }
      if (heldRepeat(3)) { if (tmpUnits > 0.1f)              tmpUnits -= 0.1f; }
      if (pressedEdge(1)) { // OK -> escoger tipo
        state = ST_BOLUS_MANUAL_EXTDUR;
        lcd.clear();
      }
      if (pressedEdge(0)) { state = ST_MENU; drawMenu(); }
    } break;

    case ST_BOLUS_MANUAL_EXTDUR: {
      // Elegir rápido o extendido (duración)
      static bool extended = false;
      lcd.setCursor(0,0); lcd.print(extended ? "Extendido" : "Rapido");
      lcd.setCursor(0,1);
      if (extended) { lcd.print("Dur: "); lcd.print(tmpMinutes); lcd.print(" min"); }
      else          { lcd.print("OK=Iniciar  "); }

      if (heldRepeat(2)) { // UP
        if (extended && tmpMinutes < 240) tmpMinutes++;
        else extended = true;
      }
      if (heldRepeat(3)) { // DOWN
        if (extended && tmpMinutes > 1) tmpMinutes--;
        else extended = false;
      }
      if (pressedEdge(1)) { // OK -> iniciar
        // Chequeos de seguridad
        if (tmpUnits > bolusLimitPerDose) { beepErr(); break; }
        if (dailyDelivered + tmpUnits > bolusDailyLimit) { beepErr(); break; }
        if (insulinRemaining < tmpUnits) { beepErr(); break; }

        if (extended) {
          startExtended(tmpUnits, tmpMinutes);
          dailyDelivered   += tmpUnits;
          insulinRemaining -= tmpUnits;
        } else {
          uint32_t steps = (uint32_t)(tmpUnits*STEPS_PER_U+0.5f);
          startJob(2, steps, BOLUS_SPEED_US);
          dailyDelivered   += tmpUnits;
          insulinRemaining -= tmpUnits;
        }
        state = ST_HOME; drawHome();
      }
      if (pressedEdge(0)) { state = ST_BOLUS_MANUAL; lcd.clear(); }
    } break;

    case ST_BOLUS_CANCEL: {
      cancelAnyInfusion();
      state = ST_MENU; drawMenu();
    } break;

    case ST_LIMITS: {
      static uint8_t page=0; // 0=per-dose,1=daily
      lcd.setCursor(0,0);
      lcd.print(page==0 ? "Limite x Bolo" : "Limite Diario");
      lcd.setCursor(0,1);
      lcd.print(page==0 ? bolusLimitPerDose : bolusDailyLimit);
      lcd.print("U     ");

      if (heldRepeat(2)) { if (page==0) bolusLimitPerDose += 0.5f; else bolusDailyLimit += 1.0f; }
      if (heldRepeat(3)) { if (page==0 && bolusLimitPerDose>0.5f) bolusLimitPerDose -= 0.5f;
                           if (page==1 && bolusDailyLimit>1.0f)   bolusDailyLimit   -= 1.0f; }
      if (pressedEdge(1)) { // OK -> cambiar página o guardar
        if (page==0) page=1;
        else { eepromSave(); page=0; beepOK(); }
        lcd.clear();
      }
      if (pressedEdge(0)) { state = ST_MENU; drawMenu(); }
    } break;

    case ST_BASAL: {
      lcd.setCursor(0,0); lcd.print("Basal: "); lcd.print(basalRate,1); lcd.print("U/h");
      lcd.setCursor(0,1); lcd.print("OK=Guardar  ");
      if (heldRepeat(2)) { if (basalRate < 5.0f) basalRate += 0.1f; }
      if (heldRepeat(3)) { if (basalRate > 0.0f) basalRate -= 0.1f; }
      if (pressedEdge(1)) { eepromSave(); recalcBasal(); state = ST_MENU; drawMenu(); }
      if (pressedEdge(0)) { state = ST_MENU; drawMenu(); }
    } break;

    case ST_AUTO_PROG: {
      // “manda X U en Y min, repetir cada Z min (0=una vez)”
      lcd.setCursor(0,0);
      lcd.print("U:"); lcd.print(tmpUnits,1);
      lcd.print(" T:"); lcd.print(tmpMinutes); lcd.print("m");
      lcd.setCursor(0,1);
      lcd.print("Rep: "); lcd.print(tmpRepeatMin); lcd.print("m  OK=Go");

      if (heldRepeat(2)) {
        // ciclo de edición: U -> T -> Rep
        static uint8_t field=0;
        // rotación de campo con OK largo
        if (digitalRead(BTN_OK)==LOW) { field=(field+1)%3; delay(180); }
        if (field==0 && tmpUnits < bolusLimitPerDose) tmpUnits+=0.1f;
        if (field==1 && tmpMinutes < 600) tmpMinutes++;
        if (field==2 && tmpRepeatMin < 600) tmpRepeatMin++;
      }
      if (heldRepeat(3)) {
        static uint8_t field=0;
        if (digitalRead(BTN_OK)==LOW) { field=(field+1)%3; delay(180); }
        if (field==0 && tmpUnits > 0.1f) tmpUnits-=0.1f;
        if (field==1 && tmpMinutes > 1) tmpMinutes--;
        if (field==2 && tmpRepeatMin > 0) tmpRepeatMin--;
      }
      if (pressedEdge(1)) {
        if (tmpUnits > bolusLimitPerDose) { beepErr(); break; }
        if (dailyDelivered + tmpUnits > bolusDailyLimit) { beepErr(); break; }
        if (insulinRemaining < tmpUnits) { beepErr(); break; }
        // programar como bolo extendido
        startExtended(tmpUnits, tmpMinutes);
        dailyDelivered   += tmpUnits;
        insulinRemaining -= tmpUnits;
        // configurar repetición
        ext.repeating = (tmpRepeatMin > 0);
        ext.repeatEveryMs = (uint32_t)tmpRepeatMin * 60000UL;
        state = ST_HOME; drawHome();
      }
      if (pressedEdge(0)) { state = ST_MENU; drawMenu(); }
    } break;

    case ST_CONFIRM_START:
      // (no usado en esta versión; se dejó hueco si quieres pantallas de confirmación)
      state = ST_HOME;
      break;
  }
}
