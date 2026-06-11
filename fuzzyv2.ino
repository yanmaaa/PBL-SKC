#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ========================
//  PIN DEFINITIONS
// ========================
#define I2C_SDA           21
#define I2C_SCL           22
#define ONE_WIRE_BUS      4
#define SOIL_PIN          34
#define MQ135_PIN         35

#define KIPAS_PIN         26
#define HEATER_PIN        27
#define HUMIDIFIER_PIN    14
#define MOTOR_PENGADUK_PIN 12

// ========================
//  PWM PARAMETERS (ESP32 Core 3.x)
// ========================
#define PWM_FREQ          5000
#define PWM_RESOLUTION    8

// Nilai PWM untuk kecepatan kipas (0-255)
#define KIPAS_MATI       0
#define KIPAS_LAMBAT     64    // 25% duty cycle
#define KIPAS_SEDANG     128   // 50% duty cycle
#define KIPAS_CEPAT      255   // 100% duty cycle

// Nilai PWM untuk heater (0-255)
#define HEATER_MATI      0
#define HEATER_LAMBAT    85    // 33% duty cycle
#define HEATER_SEDANG    170   // 67% duty cycle
#define HEATER_CEPAT     255   // 100% duty cycle

// ========================
//  SENSOR OBJECTS
// ========================
Adafruit_AHTX0 aht;
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature ds18b20(&oneWire);

// ========================
//  FUZZY SET PARAMETERS (INPUT)
// ========================
const float SOIL_DRY[]    = {0, 0, 35, 45};
const float SOIL_NORMAL[] = {35, 50, 50, 65};
const float SOIL_WET[]    = {55, 65, 100, 100};

const float MQ_NORMAL[] = {0, 0, 5, 10};
const float MQ_SEDANG[] = {5, 15, 30, 40};
const float MQ_TINGGI[] = {35, 50, 100, 100};

const float TEMP_DINGIN[] = {0, 0, 20, 30};
const float TEMP_NORMAL[] = {20, 35, 35, 50};
const float TEMP_PANAS[]  = {40, 55, 100, 100};

const float AMB_DINGIN[] = {0, 0, 20, 30};
const float AMB_NORMAL[] = {20, 35, 35, 50};
const float AMB_PANAS[]  = {40, 55, 100, 100};

// ========================
//  MEMBERSHIP FUNCTIONS
// ========================
float trapezoid(float x, float a, float b, float c, float d) {
  if (x <= a || x >= d) return 0;
  if (x >= b && x <= c) return 1;
  if (x < b) return (x - a) / (b - a);
  return (d - x) / (d - c);
}

float mf_soilDry(float x)    { return trapezoid(x, SOIL_DRY[0], SOIL_DRY[1], SOIL_DRY[2], SOIL_DRY[3]); }
float mf_soilNormal(float x) { return trapezoid(x, SOIL_NORMAL[0], SOIL_NORMAL[1], SOIL_NORMAL[2], SOIL_NORMAL[3]); }
float mf_soilWet(float x)    { return trapezoid(x, SOIL_WET[0], SOIL_WET[1], SOIL_WET[2], SOIL_WET[3]); }

float mf_mqNormal(float x) { return trapezoid(x, MQ_NORMAL[0], MQ_NORMAL[1], MQ_NORMAL[2], MQ_NORMAL[3]); }
float mf_mqSedang(float x) { return trapezoid(x, MQ_SEDANG[0], MQ_SEDANG[1], MQ_SEDANG[2], MQ_SEDANG[3]); }
float mf_mqTinggi(float x) { return trapezoid(x, MQ_TINGGI[0], MQ_TINGGI[1], MQ_TINGGI[2], MQ_TINGGI[3]); }

float mf_tempDingin(float x) { return trapezoid(x, TEMP_DINGIN[0], TEMP_DINGIN[1], TEMP_DINGIN[2], TEMP_DINGIN[3]); }
float mf_tempNormal(float x) { return trapezoid(x, TEMP_NORMAL[0], TEMP_NORMAL[1], TEMP_NORMAL[2], TEMP_NORMAL[3]); }
float mf_tempPanas(float x)  { return trapezoid(x, TEMP_PANAS[0], TEMP_PANAS[1], TEMP_PANAS[2], TEMP_PANAS[3]); }

float mf_ambDingin(float x) { return trapezoid(x, AMB_DINGIN[0], AMB_DINGIN[1], AMB_DINGIN[2], AMB_DINGIN[3]); }
float mf_ambNormal(float x) { return trapezoid(x, AMB_NORMAL[0], AMB_NORMAL[1], AMB_NORMAL[2], AMB_NORMAL[3]); }
float mf_ambPanas(float x)  { return trapezoid(x, AMB_PANAS[0], AMB_PANAS[1], AMB_PANAS[2], AMB_PANAS[3]); }

// ========================
//  RULE STRUCTURE (27 Rules)
// Index: soil, mq, ds, aht, kipas, heater, humid, status
// kipas: 0=Mati, 1=Lambat, 2=Sedang, 3=Cepat
// heater: 0=Mati, 1=Lambat, 2=Sedang, 3=Cepat
// humid: 0=OFF, 1=ON
// status: 0=Belum, 1=Setengah, 2=Matang
// ========================
struct Rule {
  byte soil; byte mq; byte ds; byte aht;
  byte kipas; byte heater; byte humid; byte status;
};

const Rule rules[27] = {
  {0,0,0,0, 1,3,1,0},   // No.1: Lambat, Cepat, ON, Belum
  {0,0,0,1, 1,2,1,0},   // No.2: Lambat, Sedang, ON, Belum
  {0,0,0,2, 1,1,1,0},   // No.3: Lambat, Lambat, ON, Belum
  {0,1,0,1, 2,2,1,1},   // No.4: Sedang, Sedang, ON, Setengah
  {0,2,0,1, 3,1,1,0},   // No.5: Cepat, Lambat, ON, Belum
  {1,0,0,0, 1,3,0,1},   // No.6: Lambat, Cepat, OFF, Setengah
  {1,0,0,1, 1,2,0,1},   // No.7: Lambat, Sedang, OFF, Setengah
  {1,0,0,2, 1,1,0,2},   // No.8: Lambat, Lambat, OFF, Matang
  {1,1,0,1, 2,2,0,1},   // No.9: Sedang, Sedang, OFF, Setengah
  {1,2,0,1, 3,1,0,0},   // No.10: Cepat, Lambat, OFF, Belum
  {1,0,1,1, 1,1,0,1},   // No.11: Lambat, Lambat, OFF, Setengah
  {1,1,1,1, 2,1,0,1},   // No.12: Sedang, Lambat, OFF, Setengah
  {1,2,1,1, 3,1,0,0},   // No.13: Cepat, Lambat, OFF, Belum
  {1,0,1,2, 1,1,1,1},   // No.14: Lambat, Lambat, ON, Setengah
  {1,1,1,2, 2,1,1,1},   // No.15: Sedang, Lambat, ON, Setengah
  {1,2,2,1, 3,1,0,0},   // No.16: Cepat, Lambat, OFF, Belum
  {1,0,2,1, 3,1,1,0},   // No.17: Cepat, Lambat, ON, Belum
  {1,1,2,1, 3,1,0,0},   // No.18: Cepat, Lambat, OFF, Belum
  {2,0,1,1, 2,1,0,1},   // No.19: Sedang, Lambat, OFF, Setengah
  {2,1,1,1, 2,1,0,0},   // No.20: Sedang, Lambat, OFF, Belum
  {2,2,1,1, 3,1,0,0},   // No.21: Cepat, Lambat, OFF, Belum
  {2,0,2,1, 3,1,0,0},   // No.22: Cepat, Lambat, OFF, Belum
  {2,1,2,1, 3,1,0,0},   // No.23: Cepat, Lambat, OFF, Belum
  {2,2,2,1, 3,1,0,0},   // No.24: Cepat, Lambat, OFF, Belum
  {0,0,1,1, 1,1,1,1},   // No.25: Lambat, Lambat, ON, Setengah
  {1,0,0,0, 1,3,0,2},   // No.26: Lambat, Cepat, OFF, Matang
  {1,0,0,1, 1,2,0,2}    // No.27: Lambat, Sedang, OFF, Matang
};

// ========================
//  GLOBAL VARIABLES
// ========================
float soilVal, mqVal, dsTemp, ahtTemp;
int kipasOutput, heaterOutput, humidOutput, statusOutput;
int motorOutput;

// Firing strengths per output
float kipasStr[4] = {0,0,0,0};  // [0]=Mati, 1=Lambat, 2=Sedang, 3=Cepat
float heaterStr[4] = {0,0,0,0}; // [0]=Mati, 1=Lambat, 2=Sedang, 3=Cepat
float humidStr[2] = {0,0};      // [0]=OFF, 1=ON
float statusStr[3] = {0,0,0};   // [0]=Belum, 1=Setengah, 2=Matang

// ========================
//  FUZZIFICATION
// ========================
void getSoilDegrees(float val, float deg[3]) {
  deg[0] = mf_soilDry(val); deg[1] = mf_soilNormal(val); deg[2] = mf_soilWet(val);
}
void getMQDegrees(float val, float deg[3]) {
  deg[0] = mf_mqNormal(val); deg[1] = mf_mqSedang(val); deg[2] = mf_mqTinggi(val);
}
void getDSDegrees(float val, float deg[3]) {
  deg[0] = mf_tempDingin(val); deg[1] = mf_tempNormal(val); deg[2] = mf_tempPanas(val);
}
void getAHTDegrees(float val, float deg[3]) {
  deg[0] = mf_ambDingin(val); deg[1] = mf_ambNormal(val); deg[2] = mf_ambPanas(val);
}

// ========================
//  RULE EVALUATION
// ========================
void evaluateRules() {
  memset(kipasStr, 0, sizeof(kipasStr));
  memset(heaterStr, 0, sizeof(heaterStr));
  memset(humidStr, 0, sizeof(humidStr));
  memset(statusStr, 0, sizeof(statusStr));
  
  float soilDeg[3], mqDeg[3], dsDeg[3], ahtDeg[3];
  getSoilDegrees(soilVal, soilDeg);
  getMQDegrees(mqVal, mqDeg);
  getDSDegrees(dsTemp, dsDeg);
  getAHTDegrees(ahtTemp, ahtDeg);
  
  for (int i = 0; i < 27; i++) {
    const Rule& r = rules[i];
    float fire = min(min(soilDeg[r.soil], mqDeg[r.mq]), min(dsDeg[r.ds], ahtDeg[r.aht]));
    if (fire <= 0.001) continue;
    kipasStr[r.kipas] = max(kipasStr[r.kipas], fire);
    heaterStr[r.heater] = max(heaterStr[r.heater], fire);
    humidStr[r.humid] = max(humidStr[r.humid], fire);
    statusStr[r.status] = max(statusStr[r.status], fire);
  }
}

// ========================
//  DEFUZZIFICATION (Maximum Membership / Center of Maximum)
// ========================
int defuzzifyToIndex(float strengths[], int numOutputs) {
  float maxStrength = 0;
  int maxIndex = 0;
  for (int i = 0; i < numOutputs; i++) {
    if (strengths[i] > maxStrength) {
      maxStrength = strengths[i];
      maxIndex = i;
    }
  }
  return maxIndex;
}

// ========================
//  SENSOR READING
// ========================
void readSensors() {
  sensors_event_t humidity, temp;
  if (aht.getEvent(&humidity, &temp)) {
    ahtTemp = constrain(temp.temperature, 0, 100);
  } else {
    ahtTemp = 25;
  }
  
  ds18b20.requestTemperatures();
  dsTemp = ds18b20.getTempCByIndex(0);
  if (dsTemp == DEVICE_DISCONNECTED_C) dsTemp = 30;
  dsTemp = constrain(dsTemp, 0, 100);
  
  int soilRaw = analogRead(SOIL_PIN);
  soilVal = constrain(map(soilRaw, 0, 4095, 0, 100), 0, 100);
  
  int mqRaw = analogRead(MQ135_PIN);
  mqVal = constrain(map(mqRaw, 0, 4095, 0, 100), 0, 100);
  
  Serial.print("Sensors -> Soil:"); Serial.print(soilVal);
  Serial.print("% MQ:"); Serial.print(mqVal);
  Serial.print(" DS:"); Serial.print(dsTemp);
  Serial.print("°C AHT:"); Serial.println(ahtTemp);
}

// ========================
//  ACTUATOR CONTROL
// ========================
void controlActuators() {
  // Mapping output ke nilai PWM
  int kipasPWM;
  switch(kipasOutput) {
    case 0: kipasPWM = KIPAS_MATI; break;
    case 1: kipasPWM = KIPAS_LAMBAT; break;
    case 2: kipasPWM = KIPAS_SEDANG; break;
    case 3: kipasPWM = KIPAS_CEPAT; break;
    default: kipasPWM = KIPAS_MATI; break;
  }
  
  int heaterPWM;
  switch(heaterOutput) {
    case 0: heaterPWM = HEATER_MATI; break;
    case 1: heaterPWM = HEATER_LAMBAT; break;
    case 2: heaterPWM = HEATER_SEDANG; break;
    case 3: heaterPWM = HEATER_CEPAT; break;
    default: heaterPWM = HEATER_MATI; break;
  }
  
  // Setup PWM (cukup sekali)
  static bool pwmInitialized = false;
  if (!pwmInitialized) {
    ledcAttach(KIPAS_PIN, PWM_FREQ, PWM_RESOLUTION);
    ledcAttach(HEATER_PIN, PWM_FREQ, PWM_RESOLUTION);
    pwmInitialized = true;
  }
  
  ledcWrite(KIPAS_PIN, kipasPWM);
  ledcWrite(HEATER_PIN, heaterPWM);
  
  // Humidifier
  digitalWrite(HUMIDIFIER_PIN, (humidOutput == 1) ? HIGH : LOW);
  
  // Motor Pengaduk: ON jika status < 2 (Belum atau Setengah Matang)
  motorOutput = (statusOutput < 2) ? 1 : 0;
  digitalWrite(MOTOR_PENGADUK_PIN, motorOutput ? HIGH : LOW);
  
  // Print output dalam bentuk angka
  Serial.print("Output -> Kipas:");
  switch(kipasOutput) {
    case 0: Serial.print("0(Mati)"); break;
    case 1: Serial.print("1(Lambat)"); break;
    case 2: Serial.print("2(Sedang)"); break;
    case 3: Serial.print("3(Cepat)"); break;
  }
  
  Serial.print(" Heater:");
  switch(heaterOutput) {
    case 0: Serial.print("0(Mati)"); break;
    case 1: Serial.print("1(Lambat)"); break;
    case 2: Serial.print("2(Sedang)"); break;
    case 3: Serial.print("3(Cepat)"); break;
  }
  
  Serial.print(" Humid:");
  Serial.print(humidOutput);
  Serial.print("("); Serial.print(humidOutput == 1 ? "ON" : "OFF"); Serial.print(")");
  
  Serial.print(" Status:");
  switch(statusOutput) {
    case 0: Serial.print("0(BelumMatang)"); break;
    case 1: Serial.print("1(SetengahMatang)"); break;
    case 2: Serial.print("2(Matang)"); break;
  }
  
  Serial.print(" Motor:");
  Serial.print(motorOutput);
  Serial.print("("); Serial.print(motorOutput == 1 ? "ON" : "OFF"); Serial.println(")");
  Serial.println("------------------------");
}

// ========================
//  SETUP
// ========================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== Fuzzy Composter System Started ===");
  Serial.println("Output Format: Kipas(0=Mati,1=Lambat,2=Sedang,3=Cepat)");
  Serial.println("              Heater(0=Mati,1=Lambat,2=Sedang,3=Cepat)");
  Serial.println("              Humid(0=OFF,1=ON)");
  Serial.println("              Status(0=Belum,1=Setengah,2=Matang)");
  Serial.println("              Motor(0=OFF,1=ON)\n");
  
  Wire.begin(I2C_SDA, I2C_SCL);
  if (!aht.begin()) {
    Serial.println("ERROR: AHT10 tidak terdeteksi!");
  } else {
    Serial.println("OK: AHT10 detected.");
  }
  
  ds18b20.begin();
  Serial.println("OK: DS18B20 initialized.");
  
  pinMode(HUMIDIFIER_PIN, OUTPUT);
  pinMode(MOTOR_PENGADUK_PIN, OUTPUT);
  
  digitalWrite(HUMIDIFIER_PIN, LOW);
  digitalWrite(MOTOR_PENGADUK_PIN, LOW);
  
  Serial.println("Setup complete. Starting main loop...\n");
}

// ========================
//  MAIN LOOP
// ========================
void loop() {
  readSensors();
  evaluateRules();
  
  // Defuzzifikasi menggunakan metode Maximum Membership
  kipasOutput = defuzzifyToIndex(kipasStr, 4);
  heaterOutput = defuzzifyToIndex(heaterStr, 4);
  humidOutput = defuzzifyToIndex(humidStr, 2);
  statusOutput = defuzzifyToIndex(statusStr, 3);
  
  controlActuators();
  
  delay(3000);
}
