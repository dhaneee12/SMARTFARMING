#include <DHT.h>
#include <FirebaseESP32.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ESP32Servo.h>
#include <time.h>

const char* ssid = "WIFI@PNP";
const char* password = "";

#define FIREBASE_HOST "https://smartfarming-82e42-default-rtdb.firebaseio.com/"
#define FIREBASE_AUTH "AIzaSyBmD-N6zhzyhmuen3KxJyueNfNPPxpVoIE"

FirebaseData firebaseData;
FirebaseAuth auth;
FirebaseConfig config;

#define buzzer 15
#define DHTPIN 16
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

#define servo2 18   // dipakai sebagai relay pompa (digitalWrite), bukan objek Servo
#define jarakEcho 13
#define jarakTrig 12
#define soil 34
#define rain 35
#define led1 22
#define led2 23

Servo myservo1;
int pos = 0;
int servoPin = 19;

// ---- Konfigurasi NTP (waktu asli) ----
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 7 * 3600;   // WIB = UTC+7. Sesuaikan jika zona waktu beda.
const int   daylightOffset_sec = 0;

unsigned long waktuSkarang = 0;
unsigned long waktuFirebase = 0;
unsigned long waktuSensorJarak = 0;
unsigned long waktuSensorSoil = 0;
unsigned long waktuSensorRain = 0;

const unsigned long jedaFirebase = 3000;
const unsigned long jedaSensorJarak = 1000;
const unsigned long jedaSensorSoil = 1000;   // sekarang benar-benar dipakai
const unsigned long jedaSensorRain = 1000;   // sekarang benar-benar dipakai

int persentase = 0;
float kelembapan = 0.0;
float suhu = 0.0;
bool hujan = 0;

String dataWaktu1 = "";
String dataWaktu2 = "";

// Ambil waktu sekarang dalam format "HH:MM" agar bisa dibandingkan dengan waktu1/waktu2
String getWaktuSekarang() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "";
  }
  char buffer[6];
  strftime(buffer, sizeof(buffer), "%H:%M", &timeinfo);
  return String(buffer);
}

void setup() {
  Serial.begin(115200);

  pinMode(jarakTrig, OUTPUT);
  pinMode(jarakEcho, INPUT);
  pinMode(buzzer, OUTPUT);
  pinMode(soil, INPUT);
  pinMode(rain, INPUT);
  pinMode(led1, OUTPUT);
  pinMode(led2, OUTPUT);
  pinMode(servo2, OUTPUT);

  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  myservo1.setPeriodHertz(50);
  myservo1.attach(servoPin, 1000, 2000);

  dht.begin();
  
  WiFi.begin(ssid, password);
  Serial.print("Menghubungkan ke WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected!");

  // Sinkronisasi waktu asli via NTP
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  config.host = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
}

void loop() {
  waktuSkarang = millis();

  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(led1, HIGH);
    Firebase.setString(firebaseData, "/output/led1", "on");  // FIX: path disamakan dengan led1 fisik

    // ==========================================
    // 1. BLOK BACA SENSOR JARAK (Setiap 1 Detik)
    // ==========================================
    if (waktuSkarang - waktuSensorJarak >= jedaSensorJarak) {
      waktuSensorJarak = waktuSkarang;

      digitalWrite(jarakTrig, LOW);
      delayMicroseconds(2);
      digitalWrite(jarakTrig, HIGH);
      delayMicroseconds(10);
      digitalWrite(jarakTrig, LOW);

      long durasi = pulseIn(jarakEcho, HIGH, 30000);
      int jarak = durasi * 0.034 / 2;

      const float PI_TABUNG = 3.14159265;
      const float JARI_JARI = 3.5;
      // !! KALIBRASI ULANG dua nilai ini sesuai ukur fisik tandon kamu !!
      // jarakKosong = jarak sensor->air saat tandon BENAR-BENAR KOSONG
      // jarakPenuh  = jarak sensor->air saat tandon BENAR-BENAR PENUH
      const int jarakKosong = 7;
      const int jarakPenuh = 2;
      float volumeML = 0.0;

      // FIX: rumus dibalik agar sesuai arah fisik (jarak kecil = penuh)
      // FIX: dipaksa float agar tidak kena integer division
      persentase = ((float)(jarakKosong - jarak) / (float)(jarakKosong - jarakPenuh)) * 100.0;
      if (persentase > 100) persentase = 100;
      if (persentase < 0) persentase = 0;

      // FIX: volume sekarang dihitung LANGSUNG dari persentase (satu sumber
      // kebenaran), jadi tidak mungkin lagi kapasitas dan volume tidak sinkron
      float volumeMaksimalML = PI_TABUNG * JARI_JARI * JARI_JARI * (jarakKosong - jarakPenuh);
      volumeML = volumeMaksimalML * (persentase / 100.0);

      if (durasi == 0) {
        Serial.println("Gagal membaca data dari sensor jarak");
      } else {
        Serial.print("Jarak Fisik: ");
        Serial.print(jarak);
        Serial.print(" cm | Kapasitas Air: ");
        Serial.print(persentase);
        Serial.println(" %");
        Serial.print("Volume Tandon: ");
        Serial.print(volumeML);
        Serial.println(" mL");
        Firebase.setFloat(firebaseData, "/output/volume", volumeML);

        if (persentase <= 10) {
          pos = 90;
          tone(buzzer, 2000);
          Firebase.setString(firebaseData, "/output/buzzer", "on");
          delay(1000);
          noTone(buzzer);
          Firebase.setString(firebaseData, "/output/buzzer", "off");
          myservo1.write(pos);
          Firebase.setString(firebaseData, "/output/servo1", "on");
        } else if (persentase >= 100) {
          pos = 0;
          myservo1.write(pos);
          Firebase.setString(firebaseData, "/output/servo1", "off");
        } else {
          pos = 0;
          noTone(buzzer);
          Firebase.setString(firebaseData, "/output/buzzer", "off");
          myservo1.write(pos);
        }
      }
    }

    // ==========================================
    // 2. BLOK FIREBASE & DHT22 (Setiap 3 Detik)
    // ==========================================
    if (waktuSkarang - waktuFirebase >= jedaFirebase) {
      waktuFirebase = waktuSkarang;
      kelembapan = dht.readHumidity();
      suhu = dht.readTemperature();

      if (isnan(kelembapan) || isnan(suhu)) {
        Serial.println("Gagal membaca data dari sensor DHT22!");
      } else {
        Serial.print("Suhu: ");
        Serial.print(suhu);
        Serial.print(" C | Kelembapan: ");
        Serial.print(kelembapan);
        Serial.println(" %");

        digitalWrite(led2, suhu > 30 ? HIGH : LOW);
        Firebase.setString(firebaseData, "/output/led2", suhu > 30 ? "on" : "off");

        if (Firebase.setFloat(firebaseData, "/output/suhu", suhu)) {
          Serial.println("-> Suhu berhasil dikirim.");
        } else {
          Serial.println("-> Gagal kirim suhu: " + firebaseData.errorReason());
        }

        if (Firebase.setFloat(firebaseData, "/output/kelembapan", kelembapan)) {
          Serial.println("-> Kelembapan berhasil dikirim.");
        } else {
          Serial.println("-> Gagal kirim kelembapan: " + firebaseData.errorReason());
        }
      }

      if (Firebase.setFloat(firebaseData, "/output/kapasitas", persentase)) {
        Serial.println("-> Kapasitas Air berhasil dikirim.");
      } else {
        Serial.println("-> Gagal kirim kapasitas: " + firebaseData.errorReason());
      }

      // --- Ambil jadwal waktu dari Firebase ---
      if (Firebase.getString(firebaseData, "/output/waktu1")) {
        dataWaktu1 = firebaseData.stringData();
        Serial.print("Waktu 1 dari Firebase: ");
        Serial.println(dataWaktu1);
      }

      if (Firebase.getString(firebaseData, "/output/waktu2")) {
        dataWaktu2 = firebaseData.stringData();
        Serial.print("Waktu 2 dari Firebase: ");
        Serial.println(dataWaktu2);
      }

      // ==========================================
      // FITUR BARU: Pompa otomatis sesuai jadwal waktu asli
      // Format waktu1/waktu2 di Firebase harus "HH:MM", contoh "07:30"
      // ==========================================
      String waktuNow = getWaktuSekarang();
      if (waktuNow != "") {
        Serial.print("Waktu sekarang: ");
        Serial.println(waktuNow);

        if (waktuNow == dataWaktu1 || waktuNow == dataWaktu2) {
          digitalWrite(servo2, HIGH);
          Firebase.setString(firebaseData, "/output/servo2", "on");
          Serial.println("-> Jadwal tercapai, pompa DINYALAKAN otomatis.");
        } else {
          digitalWrite(servo2, LOW);
          Firebase.setString(firebaseData, "/output/servo2", "off");
        }
      }

      Serial.println("--------------------------------------------------");
    }

    // ==========================================
    // 3. BLOK SENSOR HUJAN (Setiap 1 Detik) - FIX: sekarang di-throttle
    // ==========================================
    if (waktuSkarang - waktuSensorRain >= jedaSensorRain) {
      waktuSensorRain = waktuSkarang;
      hujan = digitalRead(rain);
      Firebase.setString(firebaseData, "/output/cuaca", hujan == 1 ? "Cerah" : "Hujan");
    }
  }

  // ==========================================
  // 4. BLOK SENSOR TANAH (Setiap 1 Detik) - FIX: sekarang di-throttle
  // ==========================================
  if (waktuSkarang - waktuSensorSoil >= jedaSensorSoil) {
    waktuSensorSoil = waktuSkarang;
    int tanah = analogRead(soil);
    int persentaseTanah = map(tanah, 0, 4095, 100, 0);
    Firebase.setInt(firebaseData, "/output/tanah", persentaseTanah);

    if (persentaseTanah < 30 && hujan != 0 && persentase >= 90 && suhu < 30) {
      digitalWrite(servo2, HIGH);
      Firebase.setString(firebaseData, "/output/servo2", "on");
    }
    if (persentaseTanah > 60 && hujan != 0) {
      digitalWrite(servo2, LOW);
      Firebase.setString(firebaseData, "/output/servo2", "off");
    }
  }
}
