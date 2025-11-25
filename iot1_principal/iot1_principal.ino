// ========================================================
// LETTUCECURITY – GATEWAY PRINCIPAL v9.0 FINAL CORREGIDO
// ESP-NOW con estructuras compatibles + logs detallados
// Autor: Alan Daniel Pérez Ramos – 24 noviembre 2025
// ========================================================

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <esp_now.h>
#include "DHT.h"
#include "esp_now_structs.h"  // ✅ IMPORTANTE: Estructuras compartidas

#define DHTPIN      4
#define PIRPIN      27
#define SOILPIN     34
#define BUZZERPIN   18
#define LEDPIN      2
#define DHTTYPE     DHT11

// ==================== CONFIG ====================
const char* WIFI_SSID     = "iPhone de Alberto";
const char* WIFI_PASSWORD = "bebeto12345";
const char* API_URL       = "https://lettucecurity-backend.vercel.app/iot-control/submit";
const char* BEARER_TOKEN  = "Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpZF9pb3QiOjEsInRpcG9Vc3VhcmlvIjozLCJpYXQiOjE3NjI4MDkzNjIsImV4cCI6MTc5NDM0NTM2Mn0.CpviyH2pNCZ-x46MtKgsY6MlMbY68Skev4_IIWzc4qA";

const int ID_PARCELA = 11;
const int ID_IOT_LOCAL = 1;
const int ID_IOT_REMOTO = 4;  // ✅ Coincide con MI_ID_IOT del IoT #4

// MAC del IoT #4 (secundario)
uint8_t macRemoto[] = {0x6C, 0xC8, 0x40, 0x35, 0x1A, 0x68};

const unsigned long INTERVALO_ENVIO = 60000;
const unsigned long DURACION_ALARMA = 5000;
const unsigned long BLOQUEO_PIR    = 15000;

HardwareSerial SerialCam(2);
DHT dht(DHTPIN, DHTTYPE);

// ==================== DATOS ====================
struct Datos {
  float temp = 0, hum = 0;
  int suelo = 0, luz = 0, movimiento = 0;
  String imagen = "";
  bool hayImagen = false;
  String hora = "";
} local, remoto;

// ✅ Variables para recepción ESP-NOW
String imagenRemota = "";
int chunksOK = 0, totalChunks = 0;

// ==================== ALARMA PIR PROFESIONAL ====================
bool alarmaOn = false;
unsigned long inicioAlarma = 0;
unsigned long ultimoTrigger = 0;
bool pirBloqueado = false;

void manejarAlarmaPIR() {
  if (digitalRead(PIRPIN) == HIGH && !pirBloqueado) {
    pirBloqueado = true;
    ultimoTrigger = millis();

    if (!alarmaOn) {
      alarmaOn = true;
      inicioAlarma = millis();
      tone(BUZZERPIN, 2000);
      digitalWrite(LEDPIN, HIGH);
      Serial.println("¡INTRUSO! → ALARMA ACTIVADA (2000 Hz)");
    }
  }

  if (alarmaOn && millis() - inicioAlarma >= DURACION_ALARMA) {
    noTone(BUZZERPIN);
    digitalWrite(LEDPIN, LOW);
    alarmaOn = false;
    Serial.println("Alarma apagada");
  }

  if (pirBloqueado && millis() - ultimoTrigger >= BLOQUEO_PIR) {
    pirBloqueado = false;
  }
}

// ==================== ESP-NOW CON ESTRUCTURAS CORRECTAS ====================
void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  Serial.printf("\n🔔 ESP-NOW: Datos recibidos (%d bytes)\n", len);
  
  uint8_t tipo = data[0];
  Serial.printf("   Tipo de mensaje: 0x%02X\n", tipo);
  
  if (tipo == MSG_SENSORES) {
    MensajeSensores *msg = (MensajeSensores*)data;
    
    Serial.printf("   ID recibido: %d (esperando: %d)\n", msg->idIot, ID_IOT_REMOTO);
    Serial.printf("   Temp: %.1f°C | Hum: %.1f%%\n", msg->temperatura, msg->humedadAmbiental);
    Serial.printf("   Timestamp: %s\n", msg->timestamp);
    
    if (msg->idIot != ID_IOT_REMOTO) {
      Serial.printf("❌ ID no coincide - Paquete rechazado\n");
      return;
    }
    
    Serial.println("✅ ID correcto - Procesando metadatos...");
    
    // Guardar datos de sensores
    remoto.temp = msg->temperatura;
    remoto.hum = msg->humedadAmbiental;
    remoto.suelo = msg->humedadSuelo;
    remoto.luz = msg->luz;
    remoto.movimiento = msg->pir;
    remoto.hora = String(msg->timestamp);
    
    // Preparar para recibir imagen
    imagenRemota = "";
    imagenRemota.reserve(msg->imagenTamano + 1000);
    totalChunks = msg->totalChunks;
    chunksOK = 0;
    
    Serial.printf("📡 Esperando %d chunks de imagen (%d bytes)\n", totalChunks, msg->imagenTamano);
  }
  else if (tipo == MSG_IMAGEN) {
    MensajeImagen *msg = (MensajeImagen*)data;
    
    if (msg->idIot != ID_IOT_REMOTO) {
      return;
    }
    
    // Agregar chunk a la imagen
    for (int i = 0; i < msg->chunkSize; i++) {
      imagenRemota += (char)msg->data[i];
    }
    chunksOK++;
    
    if (chunksOK % 10 == 0 || chunksOK == totalChunks) {
      Serial.printf("   Chunk %d/%d recibido\n", chunksOK, totalChunks);
    }
    
    // Si recibimos todos los chunks
    if (chunksOK >= totalChunks) {
      remoto.imagen = "data:image/jpeg;base64," + imagenRemota;
      remoto.hayImagen = true;
      Serial.printf("✅ IoT #%d → Foto completa (%d bytes, %d/%d chunks)\n", 
                    msg->idIot, imagenRemota.length(), chunksOK, totalChunks);
    }
  }
  else {
    Serial.printf("⚠️  Tipo de mensaje desconocido: 0x%02X\n", tipo);
  }
}

// ==================== UTILIDADES ====================
String getISO8601Time() {
  time_t now = time(nullptr);
  struct tm ti;
  gmtime_r(&now, &ti);
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &ti);
  return String(buf);
}

void parpadear(int n=3, int d=150) {
  for(int i=0; i<n; i++) { 
    digitalWrite(LEDPIN,HIGH); 
    delay(d); 
    digitalWrite(LEDPIN,LOW); 
    delay(d); 
  }
}

// ==================== SENSORES LOCALES ====================
void leerSensores() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  
  if (isnan(t) || isnan(h)) { 
    delay(2000); 
    t = dht.readTemperature(); 
    h = dht.readHumidity(); 
  }

  int raw = analogRead(SOILPIN);
  Serial.printf("Raw: %d\n", raw);
  int suelo = (raw >= 4090) ? 0 : 
              (raw <= 10) ? 100 : 
              (raw > 2700) ? 0 : 
              (raw < 1200) ? 100 : 
              map(raw, 2700, 1200, 0, 100);
  suelo = constrain(suelo, 0, 100);

  local.temp = isnan(t) ? 0 : t;
  local.hum = isnan(h) ? 0 : h;
  local.suelo = suelo;
  local.movimiento = digitalRead(PIRPIN);
  
  Serial.printf("📊 Sensores: T=%.1f°C H=%.1f%% S=%d%% PIR=%d\n", 
                local.temp, local.hum, local.suelo, local.movimiento);
}

// ==================== CÁMARA LOCAL ====================
bool tomarFotoLocal() {
  Serial.println("📷 Capturando foto local...");
  SerialCam.println("PING"); 
  delay(200);
  
  String img = ""; 
  img.reserve(70000);
  int luz = 0;
  bool leyendo = false, imgOK = false;
  unsigned long timeout = millis() + 45000;

  while (SerialCam.available()) SerialCam.read();

  while (millis() < timeout) {
    if (!SerialCam.available()) {
      delay(10);
      continue;
    }
    
    String linea = SerialCam.readStringUntil('\n'); 
    linea.trim();
    
    if (linea.startsWith("<LUZ>")) { 
      luz = linea.substring(5, linea.length()-6).toInt(); 
      local.luz = luz; 
      Serial.printf("💡 Luz: %d\n", luz);
    }
    if (linea == "<IMAGE_START>") { 
      leyendo = true; 
      img = ""; 
      Serial.println("📥 Recibiendo imagen...");
      continue; 
    }
    if (linea == "<IMAGE_END>") { 
      imgOK = true; 
      break; 
    }
    if (leyendo && linea.length() > 0) {
      img += linea;
    }
  }

  if (imgOK && img.length() > 1000) {
    local.imagen = "data:image/jpeg;base64," + img;
    local.hayImagen = true;
    local.hora = getISO8601Time();
    Serial.printf("✅ Foto capturada: %d bytes\n", img.length());
    return true;
  }
  
  Serial.println("⚠️  Cámara sin respuesta o imagen inválida");
  local.hayImagen = false;
  return false;
}

// ==================== JSON + ENVÍO ====================
String crearJSON() {
  size_t tamano = 8192 + local.imagen.length() + remoto.imagen.length();
  DynamicJsonDocument doc(tamano);
  
  doc["idParcela"] = ID_PARCELA;
  JsonArray data = doc.createNestedArray("data");

  // ===== IoT Local =====
  JsonObject l = data.createNestedObject();
  l["idIot"] = ID_IOT_LOCAL;
  l["hora"] = local.hora.length() ? local.hora : getISO8601Time();
  l["image"] = local.hayImagen ? local.imagen : "";

  JsonArray s1 = l.createNestedArray("dataSensores");
  
  JsonObject sensor1_1 = s1.createNestedObject();
  sensor1_1["idSensor"] = 1;
  sensor1_1["lectura"] = round(local.temp * 10) / 10.0;
  
  JsonObject sensor1_2 = s1.createNestedObject();
  sensor1_2["idSensor"] = 2;
  sensor1_2["lectura"] = local.suelo;
  
  JsonObject sensor1_3 = s1.createNestedObject();
  sensor1_3["idSensor"] = 3;
  sensor1_3["lectura"] = round(local.hum * 10) / 10.0;
  
  JsonObject sensor1_4 = s1.createNestedObject();
  sensor1_4["idSensor"] = 4;
  sensor1_4["lectura"] = local.luz;
  
  JsonObject sensor1_5 = s1.createNestedObject();
  sensor1_5["idSensor"] = 5;
  sensor1_5["lectura"] = local.movimiento;
  
  JsonObject sensor1_6 = s1.createNestedObject();
  sensor1_6["idSensor"] = 6;
  sensor1_6["lectura"] = local.hayImagen ? 1 : 0;

  // ===== IoT Remoto (solo si tiene datos válidos) =====
  if (remoto.hayImagen) {
    JsonObject r = data.createNestedObject();
    r["idIot"] = ID_IOT_REMOTO;
    r["hora"] = remoto.hora;
    r["image"] = remoto.imagen;

    JsonArray s2 = r.createNestedArray("dataSensores");
    
    JsonObject sensor2_1 = s2.createNestedObject();
    sensor2_1["idSensor"] = 1;
    sensor2_1["lectura"] = round(remoto.temp * 10) / 10.0;
    
    JsonObject sensor2_2 = s2.createNestedObject();
    sensor2_2["idSensor"] = 2;
    sensor2_2["lectura"] = remoto.suelo;
    
    JsonObject sensor2_3 = s2.createNestedObject();
    sensor2_3["idSensor"] = 3;
    sensor2_3["lectura"] = round(remoto.hum * 10) / 10.0;
    
    JsonObject sensor2_4 = s2.createNestedObject();
    sensor2_4["idSensor"] = 4;
    sensor2_4["lectura"] = remoto.luz;
    
    JsonObject sensor2_5 = s2.createNestedObject();
    sensor2_5["idSensor"] = 5;
    sensor2_5["lectura"] = remoto.movimiento;
    
    JsonObject sensor2_6 = s2.createNestedObject();
    sensor2_6["idSensor"] = 6;
    sensor2_6["lectura"] = 1;
    
    Serial.println("✅ JSON incluye datos del IoT remoto");
  } else {
    Serial.println("ℹ️  JSON solo con datos locales (remoto sin datos)");
  }

  String json; 
  serializeJson(doc, json);
  Serial.printf("📦 JSON generado: %d bytes (capacidad: %d)\n", json.length(), tamano);
  
  return json;
}

bool enviarAPI(String json) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️  WiFi desconectado, reconectando...");
    WiFi.reconnect();
    delay(5000);
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("❌ WiFi no disponible");
      return false;
    }
  }
  
  HTTPClient http;
  http.setTimeout(30000);
  http.begin(API_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", BEARER_TOKEN);
  
  Serial.println("📤 Enviando a API...");
  int code = http.POST(json);
  String response = http.getString();
  http.end();
  
  if (code >= 200 && code < 300) {
    Serial.printf("✅ API OK (HTTP %d): %s\n", code, response.c_str());
    parpadear(3, 100);
    return true;
  } else {
    Serial.printf("❌ API ERROR (HTTP %d): %s\n", code, response.c_str());
    return false;
  }
}

// ==================== SETUP & LOOP ====================
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n╔════════════════════════════════════════════╗");
  Serial.println("║  LETTUCECURITY GATEWAY v9.0 FINAL         ║");
  Serial.println("║  ESP-NOW Multi-IoT (Estructuras corregidas)║");
  Serial.println("╚════════════════════════════════════════════╝\n");
  
  pinMode(PIRPIN, INPUT); 
  pinMode(BUZZERPIN, OUTPUT); 
  pinMode(LEDPIN, OUTPUT);
  pinMode(SOILPIN, INPUT);
  
  SerialCam.begin(115200, SERIAL_8N1, 16, 17);
  dht.begin();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  Serial.print("Conectando WiFi");
  int intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos++ < 60) {
    delay(500);
    Serial.print(".");
  }
  
  int32_t channel = 0;
  if (WiFi.status() == WL_CONNECTED) {
    channel = WiFi.channel();
    Serial.printf("\n✅ WiFi conectado a '%s' | Canal: %d\n", WIFI_SSID, channel);
  } else {
    Serial.println("\n❌ Error conectando WiFi");
  }

  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }
  esp_now_register_recv_cb(OnDataRecv);

  // Configurar Peer con el canal correcto
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, macRemoto, 6);
  peer.channel = channel; // ✅ Usar el mismo canal del WiFi
  peer.encrypt = false;
  
  if (esp_now_add_peer(&peer) == ESP_OK) {
    Serial.printf("✅ Peer agregado: %02X:%02X:%02X:%02X:%02X:%02X (Canal %d)\n", 
                  macRemoto[0], macRemoto[1], macRemoto[2], 
                  macRemoto[3], macRemoto[4], macRemoto[5], channel);
  } else {
    Serial.println("⚠️  Error agregando peer (puede que ya exista)");
  }

  Serial.printf("\n🚀 Sistema listo | Memoria libre: %d bytes\n", ESP.getFreeHeap());
  Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
  
  parpadear(6, 100);

}

void loop() {
  static unsigned long ultimo = 0;
  
  manejarAlarmaPIR();

  if (millis() - ultimo >= INTERVALO_ENVIO) {
    ultimo = millis();
    
    Serial.println("\n━━━━━━━━━━ CICLO DE ENVÍO ━━━━━━━━━━");
    leerSensores();
    tomarFotoLocal();
    String json = crearJSON();

    bool enviado = false;
    for (int i = 1; i <= 3 && !enviado; i++) {
      Serial.printf("\n🔄 Intento %d/3...\n", i);
      enviado = enviarAPI(json);
      if (!enviado && i < 3) {
        Serial.println("⏳ Esperando 5s antes de reintentar...");
        delay(5000);
      }
    }
    
    if (enviado) {
      Serial.println("\n🎉 ENVÍO EXITOSO");
      remoto.hayImagen = false;
      imagenRemota = "";
    } else {
      Serial.println("\n❌ FALLO TRAS 3 INTENTOS");
    }
    
    Serial.printf("💾 Memoria libre: %d bytes\n", ESP.getFreeHeap());
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
  }
  
  delay(100);
}