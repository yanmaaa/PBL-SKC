#include <Arduino.h>

#define SOIL_MOISTURE_PIN 34
#define MQ135_PIN 35           // analog input
#define DS18B20_PIN 4          // onewire
#define AHT10_SDA 21
#define AHT10_SCL 22

// Aktuator
#define KIPAS_PIN 26           // PWM capable
#define HEATER_PIN 27          // PWM capable
#define HUMIDIFIER_PIN 14      // digital
#define MOTOR_PENGADUK_PIN 12  // digital

const float MQ135_RL = 10.0;      // kOhm
const float MQ135_VCC = 5.0;
const float MQ135_RO = 299.22;    // hasil kalibrasi

const float SOIL_DRY[3]   = {0, 0, 45};    // trapezoid [a b c d] -> here b=c? simplified triangular? Actually we use trapezoid: left foot, left top, right top, right foot. For simplicity, use triangular/ trapezoid functions.
const float SOIL_NORMAL[3] = {35, 50, 65}; // triangular
const float SOIL_WET[3]   = {55, 100, 100}; // trapezoid

// Input: MQ135 (normalized 0-100)
const float MQ_NORMAL[3] = {0, 0, 10};
const float MQ_MEDIUM[3] = {5, 22.5, 40};
const float MQ_HIGH[3]   = {35, 100, 100};

// Input: DS18B20 (suhu kompos, 0-100)
const float TEMP_DINGIN[3] = {0, 0, 30};
const float TEMP_NORMAL[3] = {20, 35, 50};
const float TEMP_PANAS[3]  = {40, 100, 100};

// Input: AHT10 (suhu ruang)
const float AMB_DINGIN[3] = {0, 0, 30};
const float AMB_NORMAL[3] = {20, 35, 50};
const float AMB_PANAS[3]  = {40, 100, 100};

// Output: Kipas & Heater
const float OUT_LAMBAT[3] = {0, 0, 30};
const float OUT_SEDANG[3] = {20, 50, 70};
const float OUT_CEPAT[3]  = {60, 100, 100};

// Output: Status kompos (0-100)
const float STATUS_BELUM[3]  = {0, 0, 40};
const float STATUS_SETENGAH[3] = {30, 50, 70};
const float STATUS_MATANG[3] = {60, 100, 100};

// ========================
//  MEMBERSHIP FUNCTIONS
// ========================
float triangular(float x, float a, float b, float c) {
  if (x <= a || x >= c) return 0;
  if (x == b) return 1;
  if (x < b) return (x - a) / (b - a);
  return (c - x) / (c - b);
}

float trapezoid(float x, float a, float b, float c, float d) {
  if (x <= a || x >= d) return 0;
  if (x >= b && x <= c) return 1;
  if (x < b) return (x - a) / (b - a);
  return (d - x) / (d - c);
}

// Generic handler for our sets (most are triangular/trapezoid)
float mf_soilKering(float x) { return trapezoid(x, SOIL_DRY[0], SOIL_DRY[1], SOIL_DRY[2], 45); } // a=0,b=0,c=35,d=45? Wait we need 4 params. Simplify: use triangular for all except edges. Better: use trapezoid for edges.
// We'll define quick functions:
float mf_dry(float x) { return trapezoid(x, 0, 0, 35, 45); }
float mf_normal(float x) { return triangular(x, 35, 50, 65); }
float mf_wet(float x) { return trapezoid(x, 55, 65, 100, 100); }

float mf_mqNormal(float x) { return trapezoid(x, 0, 0, 5, 10); }
float mf_mqSedang(float x) { return triangular(x, 5, 22.5, 40); }
float mf_mqTinggi(float x) { return trapezoid(x, 35, 40, 100, 100); }

float mf_tempDingin(float x) { return trapezoid(x, 0, 0, 20, 30); }
float mf_tempNormal(float x) { return triangular(x, 20, 35, 50); }
float mf_tempPanas(float x) { return trapezoid(x, 40, 50, 100, 100); }

float mf_ambDingin(float x) { return trapezoid(x, 0, 0, 20, 30); }
float mf_ambNormal(float x) { return triangular(x, 20, 35, 50); }
float mf_ambPanas(float x) { return trapezoid(x, 40, 50, 100, 100); }

// Output membership functions (for centroid defuzz)
float mf_outLambat(float x) { return triangular(x, 0, 0, 30); } // actually triangular with puncak di 0? bisa juga trapezoid
float mf_outSedang(float x) { return triangular(x, 20, 50, 70); }
float mf_outCepat(float x) { return triangular(x, 60, 100, 100); }

float mf_statusBelum(float x) { return trapezoid(x, 0, 0, 20, 40); }
float mf_statusSetengah(float x) { return triangular(x, 30, 50, 70); }
float mf_statusMatang(float x) { return trapezoid(x, 60, 80, 100, 100); }

// ========================
//  STRUCT RULE
// ========================
struct Rule {
  byte soilIdx;   // 0=Kering, 1=Normal, 2=Lembab
  byte mqIdx;     // 0=Normal, 1=Sedang, 2=Tinggi
  byte dsIdx;     // 0=Dingin, 1=Normal, 2=Panas
  byte ahtIdx;    // 0=Dingin, 1=Normal, 2=Panas
  byte kipasIdx;  // 0=Lambat, 1=Sedang, 2=Cepat
  byte heaterIdx; // 0=Lambat, 1=Sedang, 2=Cepat
  byte humIdx;    // 0=OFF, 1=ON
  byte statusIdx; // 0=BelumMatang, 1=SetengahMatang, 2=Matang
};

// 27 rules from the table (No.1 to 27). Indeks as above.
const Rule rules[27] = {
  // No 1: Kering,Normal,Dingin,Dingin -> Kipas Lambat, Heater Cepat, Hum ON, Status Belum
  {0,0,0,0, 0,2,1,0},
  // No 2: Kering,Normal,Dingin,Normal -> Lambat, Sedang, ON, Belum
  {0,0,0,1, 0,1,1,0},
  // No 3: Kering,Normal,Dingin,Panas -> Lambat, Lambat, ON, Belum
  {0,0,0,2, 0,0,1,0},
  // No 4: Kering,Sedang,Dingin,Normal -> Sedang, Sedang, ON, Setengah
  {0,1,0,1, 1,1,1,1},
  // No 5: Kering,Tinggi,Dingin,Normal -> Cepat, Lambat, ON, Belum
  {0,2,0,1, 2,0,1,0},
  // No 6: Normal,Normal,Dingin,Dingin -> Lambat, Cepat, OFF, Setengah
  {1,0,0,0, 0,2,0,1},
  // No 7: Normal,Normal,Dingin,Normal -> Lambat, Sedang, OFF, Setengah
  {1,0,0,1, 0,1,0,1},
  // No 8: Normal,Normal,Dingin,Panas -> Lambat, Lambat, OFF, Matang
  {1,0,0,2, 0,0,0,2},
  // No 9: Normal,Sedang,Dingin,Normal -> Sedang, Sedang, OFF, Setengah
  {1,1,0,1, 1,1,0,1},
  // No 10: Normal,Tinggi,Dingin,Normal -> Cepat, Lambat, OFF, Belum
  {1,2,0,1, 2,0,0,0},
  // No 11: Normal,Normal,Normal,Normal -> Lambat, Lambat, OFF, Setengah
  {1,0,1,1, 0,0,0,1},
  // No 12: Normal,Sedang,Normal,Normal -> Sedang, Lambat, OFF, Setengah
  {1,1,1,1, 1,0,0,1},
  // No 13: Normal,Tinggi,Normal,Normal -> Cepat, Lambat, OFF, Belum
  {1,2,1,1, 2,0,0,0},
  // No 14: Normal,Normal,Normal,Panas -> Lambat, Lambat, ON, Setengah
  {1,0,1,2, 0,0,1,1},
  // No 15: Normal,Sedang,Normal,Panas -> Sedang, Lambat, ON, Setengah
  {1,1,1,2, 1,0,1,1},
  // No 16: Normal,Tinggi,Panas,Normal -> Cepat, Lambat, OFF, Belum
  {1,2,2,1, 2,0,0,0},
  // No 17: Normal,Normal,Panas,Normal -> Cepat, Lambat, ON, Belum
  {1,0,2,1, 2,0,1,0},
  // No 18: Normal,Sedang,Panas,Normal -> Cepat, Lambat, OFF, Belum
  {1,1,2,1, 2,0,0,0},
  // No 19: Lembab,Normal,Normal,Normal -> Sedang, Lambat, OFF, Setengah
  {2,0,1,1, 1,0,0,1},
  // No 20: Lembab,Sedang,Normal,Normal -> Sedang, Lambat, OFF, Belum
  {2,1,1,1, 1,0,0,0},
  // No 21: Lembab,Tinggi,Normal,Normal -> Cepat, Lambat, OFF, Belum
  {2,2,1,1, 2,0,0,0},
  // No 22: Lembab,Normal,Panas,Normal -> Cepat, Lambat, OFF, Belum
  {2,0,2,1, 2,0,0,0},
  // No 23: Lembab,Sedang,Panas,Normal -> Cepat, Lambat, OFF, Belum
  {2,1,2,1, 2,0,0,0},
  // No 24: Lembab,Tinggi,Panas,Normal -> Cepat, Lambat, OFF, Belum
  {2,2,2,1, 2,0,0,0},
  // No 25: Kering,Normal,Normal,Normal -> Lambat, Lambat, ON, Setengah
  {0,0,1,1, 0,0,1,1},
  // No 26: Normal,Normal,Dingin,Dingin -> Lambat, Cepat, OFF, Matang (sama dengan No6? No6 sudah, tapi No26 kedua? Table punya 2 aturan matang: No8 dan No26? No26 status Matang, kita tambah)
  // Sesuai tabel revisi: No26: Normal,Normal,Dingin,Dingin -> Lambat,Cepat,OFF,Matang (lagi? mungkin duplikat, tapi kita ikut)
  {1,0,0,0, 0,2,0,2},
  // No 27: Normal,Normal,Dingin,Normal -> Lambat, Sedang, OFF, Matang (No27)
  {1,0,0,1, 0,1,0,2}
};

// ========================
//  FUZZY INFERENCE
// ========================
float fuzzifySoil(float val) {
  // returns array of 3 membership degrees: [dry, normal, wet]
  float deg[3];
  deg[0] = mf_dry(val);
  deg[1] = mf_normal(val);
  deg[2] = mf_wet(val);
  return deg;
}
// Similarly for other inputs: returns array of 3
void fuzzifyMQ(float val, float res[3]) {
  res[0] = mf_mqNormal(val);
  res[1] = mf_mqSedang(val);
  res[2] = mf_mqTinggi(val);
}
void fuzzifyDS(float val, float res[3]) {
  res[0] = mf_tempDingin(val);
  res[1] = mf_tempNormal(val);
  res[2] = mf_tempPanas(val);
}
void fuzzifyAHT(float val, float res[3]) {
  res[0] = mf_ambDingin(val);
  res[1] = mf_ambNormal(val);
  res[2] = mf_ambPanas(val);
}

// Defuzzification using centroid method for a given output membership (for continuous outputs)
float defuzzify(float (*mfFuncs[3])(float), float strengths[3], float step=0.5) {
  // mfFuncs array of pointers to membership functions for each index
  // strengths: firing strength for each consequent set
  float numerator=0, denominator=0;
  for (float x=0; x<=100; x+=step) {
    float mu = 0;
    for (int i=0; i<3; i++) {
      if (strengths[i] > 0) {
        float m = mfFuncs[i](x);
        mu = max(mu, min(strengths[i], m)); // Mamdani min implication, then max aggregation
      }
    }
    numerator += x * mu;
    denominator += mu;
  }
  if (denominator == 0) return 0;
  return numerator / denominator;
}

// For output Humidifier (ON/OFF) we use strengths for ON and OFF
// ON=1, OFF=0 using singleton method. We'll compute weighted average.
float defuzzifyOnOff(float strengthON, float strengthOFF) {
  float numerator = strengthON * 100 + strengthOFF * 0;
  float denominator = strengthON + strengthOFF;
  if (denominator == 0) return 0;
  return numerator / denominator;
}

// For status output we need a separate defuzzification because its membership functions are different
float defuzzifyStatus(float strengths[3]) {
  float (*statusMf[3])(float) = {mf_statusBelum, mf_statusSetengah, mf_statusMatang};
  float numerator=0, denominator=0;
  for (float x=0; x<=100; x+=0.5) {
    float mu = 0;
    for (int i=0; i<3; i++) {
      if (strengths[i] > 0) {
        float m = statusMf[i](x);
        mu = max(mu, min(strengths[i], m));
      }
    }
    numerator += x * mu;
    denominator += mu;
  }
  if (denominator == 0) return 0;
  return numerator / denominator;
}

// ========================
//  VARIABLES
// ========================
float soilVal, mqVal, dsTemp, ahtTemp;
float kipasOut, heaterOut, humidOut, statusOut;
bool motorState;  // true = ON

// Firing strengths for each rule's consequent
float kipasStrengths[3] = {0,0,0};
float heaterStrengths[3] = {0,0,0};
float humidStrengths[2] = {0,0}; // index0: OFF, index1: ON
float statusStrengths[3] = {0,0,0};

void evaluateRules() {
  // Reset strengths
  memset(kipasStrengths, 0, sizeof(kipasStrengths));
  memset(heaterStrengths, 0, sizeof(heaterStrengths));
  humidStrengths[0] = humidStrengths[1] = 0;
  memset(statusStrengths, 0, sizeof(statusStrengths));

  // Fuzzify inputs
  float soilF[3], mqF[3], dsF[3], ahtF[3];
  fuzzifySoil(soilVal, soilF); // I'll implement fuzzifySoil that returns array
  // Actually need separate functions. Let's write helpers:
  // We'll just compute manually in loop:
  // But better: create arrays.
  soilF[0] = mf_dry(soilVal);
  soilF[1] = mf_normal(soilVal);
  soilF[2] = mf_wet(soilVal);

  fuzzifyMQ(mqVal, mqF);
  fuzzifyDS(dsTemp, dsF);
  fuzzifyAHT(ahtTemp, ahtF);

  // Iterate rules
  for (int r=0; r<27; r++) {
    const Rule &rule = rules[r];
    float fire = min( min(soilF[rule.soilIdx], mqF[rule.mqIdx]), min(dsF[rule.dsIdx], ahtF[rule.ahtIdx]) );
    if (fire <= 0) continue;

    // Kipas
    kipasStrengths[rule.kipasIdx] = max(kipasStrengths[rule.kipasIdx], fire);
    // Heater
    heaterStrengths[rule.heaterIdx] = max(heaterStrengths[rule.heaterIdx], fire);
    // Humidifier
    if (rule.humIdx == 0) humidStrengths[0] = max(humidStrengths[0], fire);
    else humidStrengths[1] = max(humidStrengths[1], fire);
    // Status
    statusStrengths[rule.statusIdx] = max(statusStrengths[rule.statusIdx], fire);
  }
}

// ========================
//  SENSOR READING (placeholder)
// ========================
void readSensors() {
  // Replace with actual sensor libraries
  // For simulation, we generate random values within range.
  // soil moisture 0-100%
  soilVal = analogRead(SOIL_MOISTURE_PIN) / 4095.0 * 100;
  // MQ135 - need calibration; assume mapping 0-100
  mqVal = readMQ135PPM();
  // DS18B20 read
  dsTemp = 25 + random(-10, 40); // simulate 0-100
  // AHT10 read
  ahtTemp = 25 + random(-5, 30);
  
  // Constrain
  soilVal = constrain(soilVal, 0, 100);
  mqVal = constrain(mqVal, 0, 100);
  dsTemp = constrain(dsTemp, 0, 100);
  ahtTemp = constrain(ahtTemp, 0, 100);
}

// ========================
//  ACTUATOR CONTROL
// ========================
void controlActuators() {
  // Kipas (PWM)
  ledcWrite(0, (int)(kipasOut * 255 / 100)); // assuming channel 0
  // Heater (PWM)
  ledcWrite(1, (int)(heaterOut * 255 / 100));
  // Humidifier: on if humidOut >= 50
  digitalWrite(HUMIDIFIER_PIN, humidOut >= 50 ? HIGH : LOW);
  // Motor pengaduk: ON if status is not Matang (statusOut < 60)
  motorState = (statusOut < 60);
  digitalWrite(MOTOR_PENGADUK_PIN, motorState ? HIGH : LOW);
}

float readMQ135PPM() {
  long adcSum = 0;
  for(int i = 0; i < 10; i++) {
    adcSum += analogRead(MQ135_PIN);
    delay(10);
  }

  float adc = adcSum / 10.0;
  float vout = adc * 3.3 / 4095.0;
  if(vout <= 0.001)
    return 0;

  float Rs = MQ135_RL * ((MQ135_VCC - vout) / vout);
  float ratio = Rs / MQ135_RO;
  float ppm = 1.0955 * pow(10, (1.3993 * ratio));
  return ppm;
}

// ========================
//  SETUP & LOOP
// ========================
void setup() {
  Serial.begin(115200);
  // Set pins
  pinMode(KIPAS_PIN, OUTPUT);
  pinMode(HEATER_PIN, OUTPUT);
  pinMode(HUMIDIFIER_PIN, OUTPUT);
  pinMode(MOTOR_PENGADUK_PIN, OUTPUT);
  
  // Configure LEDC for PWM (resolution 8-bit, freq 5kHz)
  ledcSetup(0, 5000, 8); // channel 0 for kipas
  ledcSetup(1, 5000, 8); // channel 1 for heater
  ledcAttachPin(KIPAS_PIN, 0);
  ledcAttachPin(HEATER_PIN, 1);
  
  // Initialize sensors (I2C, OneWire etc.) - skipped for brevity
}

void loop() {
  readSensors();
  evaluateRules();
  
  // Defuzzify kipas
  float (*kipasMfs[3])(float) = {mf_outLambat, mf_outSedang, mf_outCepat};
  kipasOut = defuzzify(kipasMfs, kipasStrengths, 1.0);
  
  // Defuzzify heater
  float (*heaterMfs[3])(float) = {mf_outLambat, mf_outSedang, mf_outCepat};
  heaterOut = defuzzify(heaterMfs, heaterStrengths, 1.0);
  
  // Defuzzify humidifier (ON/OFF)
  float onOffVal = defuzzifyOnOff(humidStrengths[1], humidStrengths[0]);
  humidOut = onOffVal; // 0-100
  // Or simply compare strengths: if (humidStrengths[1] > humidStrengths[0]) humidOut=100; else 0;
  
  // Defuzzify status
  statusOut = defuzzifyStatus(statusStrengths);
  
  controlActuators();
  
  // Debug print
  Serial.print("Soil:");
  Serial.print(soilVal);
  Serial.print(" MQ:");
  Serial.print(mqVal);
  Serial.print(" DS:");
  Serial.print(dsTemp);
  Serial.print(" AHT:");
  Serial.print(ahtTemp);
  Serial.print(" Kipas:");
  Serial.print(kipasOut);
  Serial.print(" Heater:");
  Serial.print(heaterOut);
  Serial.print(" Humid:");
  Serial.print(humidOut);
  Serial.print(" Status:");
  Serial.print(statusOut);
  Serial.print(" Motor:");
  Serial.println(motorState ? "ON" : "OFF");
  
  delay(2000);
}