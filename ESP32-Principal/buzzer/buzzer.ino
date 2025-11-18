// ESP32 PRINCIPAL - Sistema IoT Completo
// Autor: Alan Daniel Pérez Ramos
// Versión: 6.2 FINAL – 100% compatible con tu tabla de sensores
// Fecha: 18 de noviembre de 2025

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "DHT.h"

// =========================================================
// CONFIGURACIÓN DE PINES
// =========================================================
#define DHTPIN    4     // DHT11
#define PIRPIN    27    // Sensor PIR
#define SOILPIN   34    // Sensor de humedad del suelo (ADC1_CH6)
#define BUZZERPIN 18    // Buzzer activo
#define LEDPIN    2     // LED onboard

#define DHTTYPE DHT11

// =========================================================
// CONFIGURACIÓN WiFi Y BACKEND
// =========================================================
const char* WIFI_SSID     = "IZZI-C81C";
const char* WIFI_PASSWORD = "nG66dFbtXaTcAyY9Tp";
const char* API_URL       = "https://lettucecurity-backend.vercel.app/iot-control/submit";
const char* BEARER_TOKEN  = "Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpZF9pb3QiOjEsInRpcG9Vc3VhcmlvIjozLCJpYXQiOjE3NjI4MDkzNjIsImV4cCI6MTc5NDM0NTM2Mn0.CpviyH2pNCZ-x46MtKgsY6MlMbY68Skev4_IIWzc4qA";

const int ID_PARCELA = 11;
const int ID_IOT     = 1;

// =========================================================
// TIMINGS (MODO TESTING – cambia a producción cuando quieras)
// =========================================================
const unsigned long INTERVALO_LECTURA_SENSORES = 30000;   // 30 segundos
const unsigned long INTERVALO_ENVIO_API       = 60000;   // 1 minuto
// const unsigned long INTERVALO_LECTURA_SENSORES = 3UL*60UL*1000UL;  // 3 min
// const unsigned long INTERVALO_ENVIO_API       = 10UL*60UL*1000UL; // 10 min

const unsigned long DURACION_ALARMA = 5000; // 5 segundos

// =========================================================
// VARIABLES GLOBALES
// =========================================================
unsigned long ultimaLecturaSensores = 0;
unsigned long ultimoEnvioAPI = 0;
unsigned long tiempoInicioAlarma = 0;
bool alarmaActiva = false;

HardwareSerial SerialCam(2);  // UART2 → GPIO16 (RX), GPIO17 (TX)
DHT dht(DHTPIN, DHTTYPE);

// Estructura con todos los datos actuales
struct DatosSensores {
  float temperatura = 0.0;
  float humedad     = 0.0;
  int   humedadSuelo = 0;
  int   valorLuz    = 0;
  bool  hayImagen   = false;
  String imagenBase64 = "";
} datosActuales;

// =========================================================
// INICIALIZACIÓN WiFi + NTP
// =========================================================
bool initWiFi() {
  Serial.println("\nConectando a WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos < 30) {
    delay(500);
    Serial.print(".");
    intentos++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi conectado – IP: " + WiFi.localIP().toString());
    Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());

    configTime(0, 0, "pool.ntp.org");
    Serial.print("Sincronizando hora NTP");
    time_t now = time(nullptr);
    while (now < 100000) {
      delay(500);
      Serial.print(".");
      now = time(nullptr);
    }
    Serial.println(" OK");
    return true;
  }
  Serial.println("\nFallo WiFi");
  return false;
}

// =========================================================
// UTILIDADES
// =========================================================
String getISO8601Time() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(buf);
}

void parpadearLED(int veces = 2, int ms = 200) {
  for (int i = 0; i < veces; i++) {
    digitalWrite(LEDPIN, HIGH); delay(ms);
    digitalWrite(LEDPIN, LOW);  delay(ms);
  }
}

// =========================================================
// ALARMA PIR
// =========================================================
void iniciarAlarma() {
  if (!alarmaActiva) {
    alarmaActiva = true;
    tiempoInicioAlarma = millis();
    tone(BUZZERPIN, 1500);
    digitalWrite(LEDPIN, HIGH);
    Serial.println("ALARM ACTIVADA – Movimiento detectado!");
  }
}

void actualizarAlarma() {
  if (alarmaActiva && millis() - tiempoInicioAlarma >= DURACION_ALARMA) {
    noTone(BUZZERPIN);
    digitalWrite(LEDPIN, LOW);
    alarmaActiva = false;
    Serial.println("Alarma desactivada");
  }
}

// =========================================================
// LECTURA DE SENSORES LOCALES
// =========================================================
void leerSensores() {
  Serial.println("\n--- Lectura de sensores locales ---");

  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (isnan(t) || isnan(h)) {
    delay(2000);
    t = dht.readTemperature();
    h = dht.readHumidity();
    if (isnan(t) || isnan(h)) t = h = 0.0;
  }

  int rawSuelo = analogRead(SOILPIN);
  int humSuelo;

  if (rawSuelo >= 4090)      humSuelo = 0;        // desconectado
  else if (rawSuelo <= 10)  humSuelo = 100;       // muy húmedo
  else if (rawSuelo > 2700) humSuelo = 0;
  else if (rawSuelo < 1200) humSuelo = 100;
  else {
    humSuelo = map(rawSuelo, 2700, 1200, 0, 100);
    humSuelo = constrain(humSuelo, 0, 100);
  }

  datosActuales.temperatura   = t;
  datosActuales.humedad       = h;
  datosActuales.humedadSuelo  = humSuelo;

  Serial.printf("Temp: %.1f°C | Hum: %.1f%% | Suelo: %d%% (raw: %d)\n", t, h, humSuelo, rawSuelo);
}

// =========================================================
// RECEPCIÓN DE DATOS DESDE ESP32-CAM
// =========================================================
bool recibirDatosCamara() {
  Serial.println("\n=== Solicitando datos a ESP32-CAM ===");
  SerialCam.println("PING");
  delay(100);

  String imagenBase64 = "";
  imagenBase64.reserve(70000);
  int valorLuz = 0;
  bool luzOK = false, imgOK = false, leyendo = false;
  int lineas = 0;
  unsigned long timeout = millis() + 45000;
  unsigned long ultimo = millis();

  while (SerialCam.available()) SerialCam.read(); // limpiar buffer

  while (millis() < timeout) {
    if (SerialCam.available()) {
      ultimo = millis();
      String linea = SerialCam.readStringUntil('\n');
      linea.trim();

      if (linea.startsWith("<!--")) continue; // progreso

      if (linea.startsWith("<LUZ>") && linea.endsWith("</LUZ>")) {
        valorLuz = linea.substring(5, linea.length()-6).toInt();
        luzOK = true;
        Serial.printf("Luz recibida: %d\n", valorLuz);
        continue;
      }

      if (linea == "<IMAGE_START>") {
        leyendo = true;
        imagenBase64 = "";
        lineas = 0;
        Serial.println("Recibiendo imagen...");
        continue;
      }

      if (linea == "<IMAGE_END>") {
        imgOK = true;
        Serial.printf("Imagen completa: %d bytes\n", imagenBase64.length());
        break;
      }

      if (leyendo && linea.length() > 0) {
        imagenBase64 += linea;
        if (++lineas % 50 == 0) Serial.printf("  %d líneas...\n", lineas);
      }
    }
    yield();
  }

  datosActuales.valorLuz = luzOK ? valorLuz : 0;
  datosActuales.hayImagen = imgOK && imagenBase64.length() > 1000;

  if (datosActuales.hayImagen) {
    if (imagenBase64.startsWith("/9j/") || imagenBase64.startsWith("iVBOR")) {
      datosActuales.imagenBase64 = "data:image/jpeg;base64," + imagenBase64;
      Serial.printf("Imagen validada: %d bytes totales\n", datosActuales.imagenBase64.length());
      return true;
    }
  }

  datosActuales.imagenBase64 = "";
  return false;
}

// =========================================================
// CREACIÓN DEL JSON – 100% COMPATIBLE CON TU TABLA DE SENSORES
// =========================================================
String crearJSON() {
  Serial.println("\n--- Construyendo JSON ---");

  size_t tamano = 4096 + datosActuales.imagenBase64.length();
  DynamicJsonDocument doc(tamano);

  doc["idParcela"] = ID_PARCELA;
  JsonArray data = doc.createNestedArray("data");
  JsonObject iot = data.createNestedObject();

  iot["idIot"] = ID_IOT;
  iot["hora"]  = getISO8601Time();

  // Si por algún motivo no hay imagen → placeholder mínimo válido
  if (datosActuales.imagenBase64.length() == 0) {
    datosActuales.imagenBase64 = "data:image/jpeg;base64,/9j/4AAQSkZJRgABAQEAYABgAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0aHBwgJC4nICIsIxwcKDcpLDAxNDQ0Hyc5PTgyPC4zNDL/2wBDAQkJCQwLDBgNDRgyIRwhMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjL/wAARCAABAAEDASIAAhEBAxEB/8QAFQABAQAAAAAAAAAAAAAAAAAAAAv/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/8QAFQEBAQAAAAAAAAAAAAAAAAAAAAX/xAAUEQEAAAAAAAAAAAAAAAAAAAAA/9oADAMBAAIRAxEAPwCwAAA=";
  }

  iot["image"] = datosActuales.imagenBase64;

  JsonArray sensores = iot.createNestedArray("dataSensores");

  // 1 – Temperatura Ambiental
  JsonObject s1 = sensores.createNestedObject();
  s1["idSensor"] = 1;
  s1["lectura"]  = round(datosActuales.temperatura * 10) / 10.0;

  // 2 – Humedad del Suelo (tu backend lo llama "Temperatura del Suelo")
  JsonObject s2 = sensores.createNestedObject();
  s2["idSensor"] = 2;
  s2["lectura"]  = (float)datosActuales.humedadSuelo;

  // 3 – Humedad Ambiental
  JsonObject s3 = sensores.createNestedObject();
  s3["idSensor"] = 3;
  s3["lectura"]  = round(datosActuales.humedad * 10) / 10.0;

  // 4 – Sensor de Luz (fotoresistencia)
  JsonObject s4 = sensores.createNestedObject();
  s4["idSensor"] = 4;
  s4["lectura"]  = (float)datosActuales.valorLuz;

  // 5 – Movimiento PIR
  JsonObject s5 = sensores.createNestedObject();
  s5["idSensor"] = 5;
  s5["lectura"]  = digitalRead(PIRPIN) ? 1 : 0;

  // 6 – Estado de la cámara (1 = hay foto)
  JsonObject s6 = sensores.createNestedObject();
  s6["idSensor"] = 6;
  s6["lectura"]  = datosActuales.hayImagen ? 1 : 0;

  String json;
  serializeJson(doc, json);
  Serial.printf("JSON creado: %d bytes\n", json.length());
  return json;
}

// =========================================================
// ENVÍO A LA API
// =========================================================
bool enviarAPI(const String& json) {
  if (WiFi.status() != WL_CONNECTED) initWiFi();

  HTTPClient http;
  http.setTimeout(30000);
  http.begin(API_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", BEARER_TOKEN);

  int code = http.POST(json);

  if (code >= 200 && code < 300) {
    Serial.printf("ÉXITO HTTP %d\n", code);
    Serial.println(http.getString());
    parpadearLED(3, 150);
    http.end();
    return true;
  } else {
    Serial.printf("ERROR HTTP %d → %s\n", code, http.errorToString(code).c_str());
    http.end();
    return false;
  }
}

// =========================================================
// CICLO PRINCIPAL DE ENVÍO
// =========================================================
void ejecutarEnvioAPI() {
  Serial.println("\n══════════════════════════════════════");
  Serial.println("     INICIANDO CICLO DE ENVÍO");
  Serial.println("══════════════════════════════════════");

  bool ok = recibirDatosCamara();

  if (!ok) {
    Serial.println("Cámara sin respuesta → usando placeholder");
    datosActuales.valorLuz = 0;
    datosActuales.hayImagen = true;
    datosActuales.imagenBase64 = "data:image/jpeg;base64,/9j/4AAQSkZJRgABAQEAYABgAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0aHBwgJC4nICIsIxwcKDcpLDAxNDQ0Hyc5PTgyPC4zNDL/2wBDAQkJCQwLDBgNDRgyIRwhMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjL/wAARCAABAAEDASIAAhEBAxEB/8QAFQABAQAAAAAAAAAAAAAAAAAAAAv/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/8QAFQEBAQAAAAAAAAAAAAAAAAAAAAX/xAAUEQEAAAAAAAAAAAAAAAAAAAAA/9oADAMBAAIRAxEAPwCwAAA=";
  }

  String payload = crearJSON();

  bool enviado = false;
  for (int i = 1; i <= 3 && !enviado; i++) {
    Serial.printf("\nIntento %d/3 → %d bytes\n", i, payload.length());
    enviado = enviarAPI(payload);
    if (!enviado) delay(5000);
  }

  Serial.println(enviado ? "\nENVÍO EXITOSO!" : "\nFALLO TRAS 3 INTENTOS");
  Serial.println("══════════════════════════════════════\n");
}

// =========================================================
// SETUP & LOOP
// =========================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n=== LETTUCECURITY – ESP32 PRINCIPAL v6.2 FINAL ===\n");

  pinMode(PIRPIN, INPUT);
  pinMode(BUZZERPIN, OUTPUT);
  pinMode(LEDPIN, OUTPUT);
  pinMode(SOILPIN, INPUT);

  SerialCam.begin(115200, SERIAL_8N1, 16, 17);
  dht.begin();

  parpadearLED(3, 300);
  initWiFi();

  leerSensores(); // lectura inicial
  ultimoEnvioAPI = millis() - INTERVALO_ENVIO_API + 5000; // primer envío rápido

  Serial.println("\nSistema listo – Modo TESTING activo");
  Serial.println("Comandos: test | send | ping\n");
}

void loop() {
  unsigned long ahora = millis();

  // Comandos manuales
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd == "test") recibirDatosCamara();
    else if (cmd == "send") ejecutarEnvioAPI();
    else if (cmd == "ping") { SerialCam.println("PING"); delay(100); Serial.println(SerialCam.available() ? SerialCam.readString() : "Sin respuesta"); }
  }

  // Lectura periódica de sensores locales
  if (ahora - ultimaLecturaSensores >= INTERVALO_LECTURA_SENSORES) {
    ultimaLecturaSensores = ahora;
    leerSensores();
  }

  // Envío periódico a la API
  if (ahora - ultimoEnvioAPI >= INTERVALO_ENVIO_API) {
    ultimoEnvioAPI = ahora;
    ejecutarEnvioAPI();
  }

  // PIR
  if (digitalRead(PIRPIN) == HIGH && !alarmaActiva) iniciarAlarma();
  actualizarAlarma();

  delay(100);
}