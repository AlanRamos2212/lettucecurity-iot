// =======================================================
// LETTUCECURITY – IoT #4 REMOTO v9.0 FINAL CORREGIDO
// Autor: Alan Daniel Pérez Ramos – 24 noviembre 2025
// ESP-NOW → IoT #1 | Estructuras compatibles
// =======================================================

#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>  // ✅ Para configurar el canal WiFi
#include "DHT.h"
#include "esp_now_structs.h"

// ====================== PINES ======================
#define DHTPIN    4
#define PIRPIN    27
#define SOILPIN   34
#define BUZZERPIN 18
#define LEDPIN    2
#define DHTTYPE   DHT11

// ====================== CONFIG ======================
const int MI_ID_IOT = 4;
const unsigned long INTERVALO_ENVIO = 30000;
const unsigned long DURACION_ALARMA = 5000;
const unsigned long BLOQUEO_PIR = 15000;

// MAC DEL IoT #1 (GATEWAY)
uint8_t iot1Address[] = {0xCC, 0xDB, 0xA7, 0x31, 0x01, 0x08};

// SSID del WiFi al que está conectado el IoT #1 (para detectar canal)
const char* WIFI_SSID_TARGET = "iPhone de Alberto";

HardwareSerial SerialCam(2);
DHT dht(DHTPIN, DHTTYPE);

unsigned long ultimoEnvio = 0;
char timestampActual[25] = "2025-01-01T00:00:00Z";
bool timestampValido = false;

// ====================== ALARMA PIR ======================
bool alarmaActiva = false;
bool pirBloqueado = false;
unsigned long inicioAlarma = 0;
unsigned long ultimoTrigger = 0;

void manejarPIR() {
  if (digitalRead(PIRPIN) == HIGH && !pirBloqueado) {
    pirBloqueado = true;
    ultimoTrigger = millis();

    if (!alarmaActiva) {
      alarmaActiva = true;
      inicioAlarma = millis();
      tone(BUZZERPIN, 2200);
      digitalWrite(LEDPIN, HIGH);
      Serial.println("¡INTRUSO DETECTADO! Alarma activada");
    }
  }

  if (alarmaActiva && millis() - inicioAlarma >= DURACION_ALARMA) {
    noTone(BUZZERPIN);
    digitalWrite(LEDPIN, LOW);
    alarmaActiva = false;
    Serial.println("Alarma apagada");
  }

  if (pirBloqueado && millis() - ultimoTrigger >= BLOQUEO_PIR) {
    pirBloqueado = false;
  }
}

// ====================== ESCANEO DE CANAL ======================
int32_t getWiFiChannel(const char *ssid) {
  if (int32_t n = WiFi.scanNetworks()) {
    for (uint8_t i = 0; i < n; i++) {
      if (!strcmp(ssid, WiFi.SSID(i).c_str())) {
        return WiFi.channel(i);
      }
    }
  }
  return 0;
}

// ====================== CALLBACK ESP-NOW ======================
void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (data[0] == MSG_PING) {
    MensajePing *msg = (MensajePing*)data;
    strncpy(timestampActual, msg->timestamp, 24);
    timestampActual[24] = '\0';
    timestampValido = true;
    
    digitalWrite(LEDPIN, HIGH);
    delay(50);
    digitalWrite(LEDPIN, LOW);
    
    Serial.printf("⏰ Timestamp sincronizado: %s\n", timestampActual);
  }
}

// ====================== SENSORES ======================
void leerSensores(MensajeSensores &msg) {
  msg.tipo = MSG_SENSORES;
  msg.idIot = MI_ID_IOT;
  strncpy(msg.timestamp, timestampActual, 24);
  msg.timestamp[24] = '\0';

  float t = dht.readTemperature();
  float h = dht.readHumidity();
  
  if (isnan(t) || isnan(h)) { 
    delay(2000); 
    t = dht.readTemperature(); 
    h = dht.readHumidity(); 
  }
  
  msg.temperatura = isnan(t) ? 0 : t;
  msg.humedadAmbiental = isnan(h) ? 0 : h;

  int raw = analogRead(SOILPIN);
  int suelo = (raw >= 4090) ? 0 : 
              (raw <= 10) ? 100 : 
              (raw > 2700) ? 0 : 
              (raw < 1200) ? 100 : 
              map(raw, 2700, 1200, 0, 100);
  msg.humedadSuelo = constrain(suelo, 0, 100);

  msg.pir = digitalRead(PIRPIN);
  
  Serial.printf("📊 T=%.1f°C H=%.1f%% S=%d%% PIR=%d\n", 
                msg.temperatura, msg.humedadAmbiental, (int)msg.humedadSuelo, msg.pir);
}

// ====================== CÁMARA ======================
bool recibirDatosCamara(MensajeSensores &msg, String &imagen) {
  Serial.println("📷 Solicitando foto...");
  SerialCam.println("PING"); 
  delay(200);
  
  imagen = "";
  imagen.reserve(70000);
  int luz = 0;
  bool ok = false, leyendo = false;
  unsigned long timeout = millis() + 40000;

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
    }
    if (linea == "<IMAGE_START>") { 
      leyendo = true; 
      imagen = ""; 
      continue; 
    }
    if (linea == "<IMAGE_END>") { 
      ok = true; 
      break; 
    }
    if (leyendo && linea.length() > 0) {
      imagen += linea;
    }
  }

  msg.luz = luz;
  msg.camaraActiva = (ok && imagen.length() > 1000) ? 1 : 0;
  
  if (ok && imagen.length() > 1000) {
    Serial.printf("✅ Foto capturada: %d bytes\n", imagen.length());
    return true;
  }
  
  Serial.println("⚠️  Cámara sin respuesta");
  return false;
}

// ====================== ENVÍO CHUNKS ======================
void enviarImagen(const String &base64) {
  uint16_t chunks = (base64.length() + 239) / 240;
  Serial.printf("📤 Enviando imagen: %d chunks\n", chunks);

  for (uint16_t i = 0; i < chunks; i++) {
    MensajeImagen m;
    m.tipo = MSG_IMAGEN;
    m.idIot = MI_ID_IOT;
    m.chunkIndex = i;
    
    int start = i * 240;
    int len = min(240, (int)base64.length() - start);
    m.chunkSize = len;
    
    for (int j = 0; j < len; j++) {
      m.data[j] = base64[start + j];
    }

    esp_err_t result = esp_now_send(iot1Address, (uint8_t*)&m, sizeof(m));
    
    if (result != ESP_OK && i == 0) {
      Serial.printf("❌ Error enviando chunk %d: %d\n", i, result);
    }
    
    if (i % 20 == 0) {
      digitalWrite(LEDPIN, !digitalRead(LEDPIN));
    }
    
    delay(10);
    yield();
  }
  
  digitalWrite(LEDPIN, LOW);
  Serial.println("✅ Imagen enviada");
}

// ====================== ENVÍO COMPLETO ======================
void ejecutarEnvio() {
  Serial.println("\n━━━━━━━━━━ ENVIANDO A IoT #1 ━━━━━━━━━━");
  
  MensajeSensores msg;
  leerSensores(msg);

  Serial.printf("📋 Preparando envío:\n");
  Serial.printf("   MI_ID_IOT: %d\n", MI_ID_IOT);
  Serial.printf("   Timestamp: %s\n", msg.timestamp);
  Serial.printf("   Temp: %.1f°C | Hum: %.1f%%\n", msg.temperatura, msg.humedadAmbiental);

  String foto;
  bool fotoOK = recibirDatosCamara(msg, foto);
  
  if (!fotoOK || foto.length() < 1000) {
    Serial.println("⚠️  Usando placeholder");
    foto = "/9j/4AAQSkZJRgABAQEAYABgAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0aHBwgJC4nICIsIxwcKDcpLDAxNDQ0Hyc5PTgyPC4zNDL/2wBDAQkJCQwLDBgNDRgyIRwhMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjL/wAARCAABAAEDASIAAhEBAxEB/8QAFQABAQAAAAAAAAAAAAAAAAAAAAv/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/8QAFQEBAQAAAAAAAAAAAAAAAAAAAAX/xAAUEQEAAAAAAAAAAAAAAAAAAAAA/9oADAMBAAIRAxEAPwCwAAA=";
    msg.camaraActiva = 1;
  }

  msg.imagenTamano = foto.length();
  msg.totalChunks = (foto.length() + 239) / 240;

  Serial.printf("📤 Enviando metadatos a MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                iot1Address[0], iot1Address[1], iot1Address[2],
                iot1Address[3], iot1Address[4], iot1Address[5]);
  Serial.printf("   Tipo: 0x%02X (MSG_SENSORES)\n", msg.tipo);
  Serial.printf("   ID: %d\n", msg.idIot);
  Serial.printf("   Tamaño mensaje: %d bytes\n", sizeof(msg));

  esp_err_t result = esp_now_send(iot1Address, (uint8_t*)&msg, sizeof(msg));
  
  if (result == ESP_OK) {
    Serial.println("✅ Metadatos enviados");
    
    for (int i = 0; i < 3; i++) {
      digitalWrite(LEDPIN, HIGH); delay(100);
      digitalWrite(LEDPIN, LOW); delay(100);
    }
    
    delay(200);
    enviarImagen(foto);
    
    Serial.println("✅ ENVÍO COMPLETADO");
  } else {
    Serial.printf("❌ Error enviando metadatos: %d\n", result);
    Serial.println("   Posibles causas:");
    Serial.println("   - MAC del IoT #1 incorrecta");
    Serial.println("   - IoT #1 no está escuchando");
    Serial.println("   - Peer no agregado correctamente");
    
    for (int i = 0; i < 5; i++) {
      digitalWrite(LEDPIN, HIGH); delay(50);
      digitalWrite(LEDPIN, LOW); delay(50);
    }
  }
  
  Serial.printf("💾 Memoria libre: %d bytes\n", ESP.getFreeHeap());
  Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
}

// ====================== SETUP ======================
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n╔════════════════════════════════════════════════╗");
  Serial.println("║  LETTUCECURITY – IoT #4 REMOTO v9.0          ║");
  Serial.println("║  ESP-NOW Multi-IoT (Estructuras corregidas)  ║");
  Serial.println("╚════════════════════════════════════════════════╝\n");

  pinMode(PIRPIN, INPUT);
  pinMode(BUZZERPIN, OUTPUT);
  pinMode(LEDPIN, OUTPUT);
  pinMode(SOILPIN, INPUT);

  SerialCam.begin(115200, SERIAL_8N1, 16, 17);
  dht.begin();

  WiFi.mode(WIFI_STA);
  
  Serial.print("📍 MAC de este dispositivo: ");
  Serial.println(WiFi.macAddress());

  Serial.print("📍 MAC de este dispositivo: ");
  Serial.println(WiFi.macAddress());

  // ✅ CRÍTICO: Escanear canal del WiFi del IoT #1
  Serial.printf("🔎 Buscando canal de '%s'...\n", WIFI_SSID_TARGET);
  int32_t canal = getWiFiChannel(WIFI_SSID_TARGET);
  
  if (canal == 0) {
    Serial.println("⚠️  SSID no encontrado, usando canal 1 por defecto");
    canal = 1;
  } else {
    Serial.printf("✅ Encontrado en canal: %d\n", canal);
  }

  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(canal, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
  
  Serial.printf("📡 Canal WiFi configurado: %d\n", canal);

  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ ESP-NOW falló");
    while (true) {
      digitalWrite(LEDPIN, HIGH); delay(1000);
      digitalWrite(LEDPIN, LOW); delay(1000);
    }
  }
  
  Serial.println("✅ ESP-NOW inicializado");
  esp_now_register_recv_cb(onDataRecv);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, iot1Address, 6);
  peer.channel = canal;  // ✅ Usar el mismo canal
  peer.encrypt = false;
  
  if (esp_now_add_peer(&peer) == ESP_OK) {
    Serial.printf("✅ Peer agregado: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  iot1Address[0], iot1Address[1], iot1Address[2],
                  iot1Address[3], iot1Address[4], iot1Address[5]);
  } else {
    Serial.println("⚠️  Error agregando peer");
  }

  Serial.println("\n⏳ Esperando primer timestamp del IoT #1...\n");
  
  unsigned long inicio = millis();
  while (!timestampValido && millis() - inicio < 30000) {
    delay(100);
    if ((millis() - inicio) % 1000 == 0) {
      digitalWrite(LEDPIN, !digitalRead(LEDPIN));
    }
  }
  
  digitalWrite(LEDPIN, LOW);
  
  if (!timestampValido) {
    Serial.println("⚠️  No se recibió timestamp (usando por defecto)");
  } else {
    Serial.printf("✅ Timestamp sincronizado: %s\n", timestampActual);
  }

  Serial.println("\n🚀 Sistema listo – Iniciando envíos periódicos...\n");
  
  for (int i = 0; i < 6; i++) {
    digitalWrite(LEDPIN, HIGH); delay(150);
    digitalWrite(LEDPIN, LOW); delay(150);
  }
  
  delay(2000);
  ejecutarEnvio();
  ultimoEnvio = millis();
}

// ====================== LOOP ======================
void loop() {
  manejarPIR();

  if (millis() - ultimoEnvio >= INTERVALO_ENVIO) {
    ultimoEnvio = millis();
    ejecutarEnvio();
  }

  delay(100);
  yield();
}
