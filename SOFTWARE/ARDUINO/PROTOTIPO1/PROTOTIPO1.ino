#include <LiquidCrystal.h>
#include <EEPROM.h>

// Configuración LCD (RS, E, D4, D5, D6, D7)
LiquidCrystal lcd(12, 13, 4, 5, 6, 7);

// Pines para driver A4988
const int stepPin = 9;     // Pin STEP del A4988
const int dirPin = 8;      // Pin DIR del A4988
const int enablePin = 2;   // Pin ENABLE del A4988 (activo bajo)
// const int ms1Pin = 9;      // Pin MS1 para microstepping
// const int ms2Pin = 10;     // Pin MS2 para microstepping
// const int ms3Pin = A3;     // Pin MS3 para microstepping

// Pines de sensores y botones
const int voltagePin = A0;  // Potenciómetro para voltaje
const int button1 = 10;     // Botón modo
const int button2 = 11;     // Botón configuración
const int buzzer = 3;      // Buzzer

// Variables del sistema
float batteryVoltage = 4.2;
int batteryPercent = 100;
bool basalMode = true;      // true = automático, false = manual
bool criticalBattery = false;
bool sleepMode = false;

// Variables de dosificación (valores estándar clínicos)
float basalRate = 1.0;       // U/h (rango típico: 0.5-3.0 U/h)
float insulinRemaining = 300; // Cartucho estándar de 300 unidades
unsigned long lastBasalTime = 0;
const unsigned long basalInterval = 60000; // 1 minuto para dosis basal

// Variables del motor (ajustes para precisión clínica)
const int stepsPerUnit = 400;  // Alta precisión (0.0025 U por paso)
const int stepDelay = 500;     // Microsegundos entre pasos

// Variables del modo sueño
unsigned long sleepStartTime = 0;
const unsigned long sleepTimeout = 180000; // 3 minutos sin actividad (estándar clínico)

// Estados del sistema
enum SystemState {
  INIT,
  BATTERY_CHECK,
  SYSTEM_CHECK,
  OPERATION_MODE,
  BASAL_CONFIG,
  DOSE_DELIVERY,
  BOLUS_REQUEST,
  SLEEP_MODE,
  ERROR_STATE
};

SystemState currentState = INIT;


void setup() {
  Serial.begin(9600);

  // Inicializar LCD
  lcd.begin(16, 2);
  lcd.print("Bomba Insulina");
  lcd.setCursor(0, 1);
  lcd.print("Iniciando...");
  
  // Delay inicial de 2 segundos
  delay(2000);
  
  // Inicializar variables
  initializeSystem();
}

void loop() {
  // Verificar timeout para modo sueño
  if (!sleepMode && (millis() - sleepStartTime > sleepTimeout)) {
    enterSleepMode();
  }
  
  updateSensors();
  
  // No procesar si batería crítica
  if (criticalBattery && currentState != ERROR_STATE) {
    handleCriticalBattery();
  }
  else {
    switch(currentState) {
      case INIT:
        handleInit();
        break;
      case BATTERY_CHECK:
        handleBatteryCheck();
        break;
      case SYSTEM_CHECK:
        handleSystemCheck();
        break;
      case OPERATION_MODE:
        handleOperationMode();
        break;
      case BASAL_CONFIG:
        handleBasalConfig();
        break;
      case DOSE_DELIVERY:
        handleDoseDelivery();
        break;
      case BOLUS_REQUEST:
        handleBolusRequest();
        break;
      case SLEEP_MODE:
        handleSleepMode();
        break;
      case ERROR_STATE:
        handleErrorState();
        break;
    }
  }
  
  continuousMonitoring();
  resetSleepTimer();
  delay(100);
}


void initializeSystem() {
  // Cargar configuración desde EEPROM
  basalRate = EEPROM.read(0) / 10.0;
  if (basalRate < 0.1 || basalRate > 5.0) basalRate = 1.0; // Rango clínico seguro
  
  currentState = BATTERY_CHECK;
  resetSleepTimer();
  criticalBattery = false;
}

void enterSleepMode() {
  sleepMode = true;
  currentState = SLEEP_MODE;
  
  // Apagar LCD
  lcd.noDisplay();
  
  // Deshabilitar motor
  digitalWrite(enablePin, HIGH);
  
  lcd.clear();
  lcd.print("Modo Suspension");
  lcd.setCursor(0, 1);
  lcd.print("Presione BTN1");
  delay(1000);
  lcd.noDisplay();
}

void wakeUp() {
  sleepMode = false;
  
  // Reactivar LCD
  lcd.display();
  
  // Mostrar mensaje de despertar
  lcd.clear();
  lcd.print("Reactivando...");
  delay(1000);
  
  currentState = DOSE_DELIVERY;
  resetSleepTimer();
}

void resetSleepTimer() {
  sleepStartTime = millis();
}

void updateSensors() {
  // Leer potenciómetro para simular voltaje
  int voltageReading = analogRead(voltagePin);
  
  // Convertir lectura a voltaje (3.0V a 4.2V)
  batteryVoltage = map(voltageReading, 0, 1023, 300, 420) / 100.0;
  
  // Calcular porcentaje de batería (0-100%)
  batteryPercent = map(voltageReading, 0, 1023, 0, 100);
  batteryPercent = constrain(batteryPercent, 0, 100);
  
  // Detectar batería crítica (umbral clínico)
  criticalBattery = (batteryPercent < 10 || batteryVoltage < 3.4);
}

void handleCriticalBattery() {
  lcd.clear();
  lcd.print("BATERIA CRITICA!");
  lcd.setCursor(0, 1);
  lcd.print(batteryPercent);
  lcd.print("% ");
  lcd.print(batteryVoltage);
  lcd.print("V");
  
  // Parar todas las operaciones
  digitalWrite(enablePin, HIGH);
  
  // Sonar alarma continua
  static unsigned long lastBeep = 0;
  if (millis() - lastBeep > 500) {
    digitalWrite(buzzer, !digitalRead(buzzer));
    lastBeep = millis();
  }
}

void handleInit() {
  lcd.clear();
  lcd.print("Inicializando...");
  lcd.setCursor(0, 1);
  lcd.print("Sistema OK");
  delay(2000);
  currentState = BATTERY_CHECK;
}

void handleBatteryCheck() {
  lcd.clear();
  lcd.print("Bateria: ");
  lcd.print(batteryPercent);
  lcd.print("% ");
  lcd.print(batteryVoltage);
  lcd.print("V");
  
  if (batteryVoltage < 3.4 || batteryPercent < 20) {
    lcd.setCursor(0, 1);
    lcd.print("BATERIA BAJA!");
    soundAlarm();
    delay(2000);
    
    if (batteryVoltage < 3.3 || batteryPercent < 10) {
      criticalBattery = true;
      currentState = ERROR_STATE;
      return;
    }
  }
  
  currentState = SYSTEM_CHECK;
}

void handleSystemCheck() {
  lcd.clear();
  lcd.print("Verificando...");
  
  // Verificar estado del motor
  if (!checkMotorStatus()) {
    lcd.setCursor(0, 1);
    lcd.print("ERROR MOTOR");
    soundAlarm();
    currentState = ERROR_STATE;
    return;
  }
  
  // Verificar insulina
  if (insulinRemaining < 20) { // Umbral clínico para alerta
    lcd.setCursor(0, 1);
    lcd.print("INSULINA BAJA!");
    soundAlarm();
    delay(2000);
  }
  
  lcd.setCursor(0, 1);
  lcd.print("Sistema OK");
  delay(2000);
  
  // Cebar motor
  primeMotor();
  
  currentState = OPERATION_MODE;
}

bool checkMotorStatus() {
  lcd.clear();
  lcd.print("Test Motor...");
  lcd.setCursor(0, 1);
  lcd.print("Pasos: 0");
  
  // Habilitar motor
  digitalWrite(enablePin, LOW);
  digitalWrite(dirPin, HIGH);
  
  // Hacer 40 pasos de prueba (estándar clínico)
  for (int i = 0; i < 40; i++) {
    digitalWrite(stepPin, HIGH);
    delay(10);
    digitalWrite(stepPin, LOW);
    delay(10);
    
    // Actualizar contador
    if (i % 10 == 0) {
      lcd.setCursor(7, 1);
      lcd.print(i+1);
    }
  }
  
  // Deshabilitar motor
  digitalWrite(enablePin, HIGH);
  
  lcd.clear();
  lcd.print("Motor OK!");
  delay(1000);
  
  return true;
}

void handleOperationMode() {
  lcd.clear();
  lcd.print("Modo:");
  lcd.setCursor(0, 1);
  lcd.print(basalMode ? "Automatico" : "Manual");
  
  if (digitalRead(button1) == LOW) {
    basalMode = !basalMode;
    delay(300);
    resetSleepTimer();
  }
  
  if (digitalRead(button2) == LOW) {
    currentState = BASAL_CONFIG;
    delay(300);
    resetSleepTimer();
  }
}

void handleBasalConfig() {
  static unsigned long lastChange = 0;
  
  lcd.clear();
  lcd.print("Config Basal");
  lcd.setCursor(0, 1);
  lcd.print("Rate: ");
  lcd.print(basalRate, 1);
  lcd.print(" U/h");
  
  if (digitalRead(button1) == LOW && millis() - lastChange > 300) {
    basalRate += 0.1;
    if (basalRate > 3.0) basalRate = 0.5; // Rango clínico: 0.5-3.0 U/h
    lastChange = millis();
    resetSleepTimer();
  }
  
  if (digitalRead(button2) == LOW) {
    EEPROM.write(0, (int)(basalRate * 10));
    currentState = DOSE_DELIVERY;
    delay(300);
    resetSleepTimer();
  }
}

void handleDoseDelivery() {
  // Administrar dosis basal
  if (millis() - lastBasalTime >= basalInterval) {
    float doseAmount = basalRate / 60.0; // Dosis por minuto
    deliverDose(doseAmount);
    lastBasalTime = millis();
    resetSleepTimer();
  }
  
  // Mostrar información
  lcd.clear();
  lcd.print("Basal: ");
  lcd.print(basalRate, 1);
  lcd.print(" U/h");
  
  lcd.setCursor(0, 1);
  lcd.print("Ins: ");
  lcd.print(insulinRemaining, 0);
  lcd.print("U Bat: ");
  lcd.print(batteryPercent);
  lcd.print("%");
  
  // Verificar solicitud de bolo
  if (digitalRead(button1) == LOW) {
    currentState = BOLUS_REQUEST;
    delay(300);
    resetSleepTimer();
  }
}

void handleBolusRequest() {
  static float bolusSize = 1.0; // Dosis inicial estándar
  
  lcd.clear();
  lcd.print("BOLO: ");
  lcd.print(bolusSize, 1);
  lcd.print(" U");
  
  if (digitalRead(button1) == LOW) {
    bolusSize += 0.5;
    if (bolusSize > 15.0) bolusSize = 0.5; // Rango clínico: 0.5-15.0 U
    delay(300);
    resetSleepTimer();
  }
  
  if (digitalRead(button2) == LOW) {
    if (batteryPercent >= 15 && insulinRemaining >= bolusSize) {
      deliverBolus(bolusSize);
      currentState = DOSE_DELIVERY;
    } else {
      lcd.setCursor(0, 1);
      if (batteryPercent < 15) {
        lcd.print("BATERIA BAJA!");
      } else {
        lcd.print("INSULINA INSUF!");
      }
      soundAlarm();
      delay(2000);
    }
    resetSleepTimer();
  }
  else {
    lcd.setCursor(0, 1);
    lcd.print("Confirme BTN2");
  }
}

void handleSleepMode() {
  // Administrar dosis en modo sueño
  if (millis() - lastBasalTime >= basalInterval) {
    float doseAmount = basalRate / 60.0;
    deliverDoseQuiet(doseAmount);
    lastBasalTime = millis();
  }
  
  // Verificar botón de despertar
  if (digitalRead(button1) == LOW) {
    wakeUp();
  }
}

void handleErrorState() {
  lcd.clear();
  lcd.print("ERROR SISTEMA");
  lcd.setCursor(0, 1);
  lcd.print("Reinicie");
  
  soundAlarm();
  
  if (digitalRead(button1) == LOW && digitalRead(button2) == LOW) {
    currentState = INIT;
    lcd.clear();
    lcd.print("Reiniciando...");
    delay(1000);
    resetSleepTimer();
  }
}

void deliverDose(float amount) {
  // Verificar cantidad clínicamente segura
  if (amount <= 0 || amount > 1.0 || insulinRemaining < amount) {
    soundAlarm();
    lcd.clear();
    lcd.print("Error dosis!");
    lcd.setCursor(0, 1);
    lcd.print("Revise config");
    delay(2000);
    return;
  }
  
  lcd.clear();
  lcd.print("Dosificando...");
  lcd.setCursor(0, 1);
  lcd.print(amount, 3);
  lcd.print(" U");
  
  // Calcular pasos con microstepping 1/16
  int steps = (int)(amount * stepsPerUnit * 16);
  
  // Habilitar motor
  digitalWrite(enablePin, LOW);
  digitalWrite(dirPin, HIGH);
  
  for (int i = 0; i < steps; i++) {
    digitalWrite(stepPin, HIGH);
    delayMicroseconds(stepDelay);
    digitalWrite(stepPin, LOW);
    delayMicroseconds(stepDelay);
    
    // Mostrar progreso
    if (i % 100 == 0) {
      lcd.setCursor(0, 1);
      lcd.print("                ");
      lcd.setCursor(0, 1);
      lcd.print(amount, 3);
      lcd.print("U ");
      lcd.print(map(i, 0, steps, 0, 100));
      lcd.print("%");
    }
  }
  
  // Deshabilitar motor
  digitalWrite(enablePin, HIGH);
  insulinRemaining -= amount;
  
  // Confirmación
  lcd.clear();
  lcd.print("Dosis completa!");
  lcd.setCursor(0, 1);
  lcd.print(amount, 3);
  lcd.print("U administradas");
  delay(1000);
}

void deliverDoseQuiet(float amount) {
  if (amount <= 0 || amount > 1.0 || insulinRemaining < amount) return;
  
  // Calcular pasos con microstepping 1/16
  int steps = (int)(amount * stepsPerUnit * 16);
  
  // Habilitar motor
  digitalWrite(enablePin, LOW);
  digitalWrite(dirPin, HIGH);
  
  for (int i = 0; i < steps; i++) {
    digitalWrite(stepPin, HIGH);
    delayMicroseconds(stepDelay * 2);
    digitalWrite(stepPin, LOW);
    delayMicroseconds(stepDelay * 2);
  }
  
  // Deshabilitar motor
  digitalWrite(enablePin, HIGH);
  insulinRemaining -= amount;
}

void deliverBolus(float amount) {
  // Verificar cantidad clínicamente segura
  if (amount <= 0 || amount > 15.0 || insulinRemaining < amount) {
    soundAlarm();
    lcd.clear();
    lcd.print("Error bolo!");
    lcd.setCursor(0, 1);
    lcd.print("Revise dosis");
    delay(2000);
    return;
  }
  
  lcd.clear();
  lcd.print("BOLO: ");
  lcd.print(amount, 1);
  lcd.print(" U");
  
  // Calcular pasos con microstepping 1/16
  int steps = (int)(amount * stepsPerUnit * 16);
  
  // Habilitar motor
  digitalWrite(enablePin, LOW);
  digitalWrite(dirPin, HIGH);
  
  for (int i = 0; i < steps; i++) {
    digitalWrite(stepPin, HIGH);
    delayMicroseconds(stepDelay / 2);
    digitalWrite(stepPin, LOW);
    delayMicroseconds(stepDelay / 2);
    
    // Mostrar progreso
    if (i % 400 == 0) {
      lcd.setCursor(0, 1);
      lcd.print("Progreso: ");
      lcd.print(map(i, 0, steps, 0, 100));
      lcd.print("%");
    }
  }
  
  // Deshabilitar motor
  digitalWrite(enablePin, HIGH);
  insulinRemaining -= amount;
  
  lcd.clear();
  lcd.print("Bolo completado!");
  lcd.setCursor(0, 1);
  lcd.print(amount, 1);
  lcd.print("U administrados");
  delay(2000);
}

void primeMotor() {
  lcd.clear();
  lcd.print("Cebando sistema");
  lcd.setCursor(0, 1);
  lcd.print("0/100 pasos");
  
  // Habilitar motor
  digitalWrite(enablePin, LOW);
  digitalWrite(dirPin, HIGH);
  
  // Cebado estándar: 100 pasos
  for (int i = 0; i < 100; i++) {
    digitalWrite(stepPin, HIGH);
    delay(10);
    digitalWrite(stepPin, LOW);
    delay(10);
    
    // Actualizar contador
    if (i % 10 == 0) {
      lcd.setCursor(0, 1);
      lcd.print("                ");
      lcd.setCursor(0, 1);
      lcd.print(i);
      lcd.print("/100 pasos");
    }
  }
  
  // Deshabilitar motor
  digitalWrite(enablePin, HIGH);
  
  lcd.clear();
  lcd.print("Sistema cebado!");
  delay(1000);
}

void continuousMonitoring() {
  // Alerta batería baja (umbral clínico)
  if (batteryPercent < 20) {
    static unsigned long lastBlink = 0;
    if (millis() - lastBlink > 1000) {
      digitalWrite(13, !digitalRead(13));
      lastBlink = millis();
    }
  }
}

void soundAlarm() {
  // Alarma clínica estándar (3 pulsos)
  for (int i = 0; i < 3; i++) {
    digitalWrite(buzzer, HIGH);
    delay(300);
    digitalWrite(buzzer, LOW);
    delay(300);
  }
}