// =======================================================
// ESP32-CAM - VERSIÓN FINAL DEFINITIVA (SIN SOLDAR NADA)
// Autor: Alan Daniel Pérez Ramos
// Comunicación por U0TXD (GPIO1) → Pin ya soldado en la placa
// Solo 3 cables: 5V + GND + U0TXD → ESP32 Principal
// =======================================================

#include "esp_camera.h"
#include "img_converters.h"
#include <Arduino.h>
#include "mbedtls/base64.h"

// ==================== PINES CÁMARA ====================
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// ==================== SENSORES ====================
#define LDR_PIN           13   // Pin libre y con header
#define FLASH_PIN          4   // Flash LED

// ==================== CONFIGURACIÓN ====================
const int UMBRAL_LUZ = 1500;                    // >1500 = poca luz
//const unsigned long INTERVALO_FOTO = 30000;     // 30 seg (cambiar a 5min en producción)
const unsigned long INTERVALO_FOTO = 5UL * 60UL * 1000UL;

unsigned long ultimaFoto = 0;

// ==================== INICIALIZACIÓN CÁMARA ====================
bool initCamera() {
  if (!psramFound()) {
    // Sin Serial → parpadeo de flash como error
    while (true) {
      digitalWrite(FLASH_PIN, HIGH); delay(100);
      digitalWrite(FLASH_PIN, LOW);  delay(100);
    }
  }

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM; config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_YUV422;
  config.frame_size   = FRAMESIZE_QQVGA;    // 160x120 → ~20KB → perfecto
  config.jpeg_quality = 12;
  config.fb_count     = 2;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;

  if (esp_camera_init(&config) != ESP_OK) {
    while (true) {
      digitalWrite(FLASH_PIN, HIGH); delay(500);
      digitalWrite(FLASH_PIN, LOW);  delay(500);
    }
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_brightness(s, 0);
    s->set_contrast(s, 0);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
  }
  return true;
}

// ==================== LECTURA LUZ + FLASH ====================
int leerSensorLuz() {
  int luz = analogRead(LDR_PIN);
  if (luz > UMBRAL_LUZ) {
    digitalWrite(FLASH_PIN, HIGH);
    delay(200);  // Tiempo para iluminar
  } else {
    digitalWrite(FLASH_PIN, LOW);
  }
  return luz;
}

// ==================== CAPTURA Y CONVERSIÓN ====================
String capturarYConvertir() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    digitalWrite(FLASH_PIN, LOW);
    return "";
  }

  uint8_t *jpg = NULL;
  size_t jpg_len = 0;
  bool ok = fmt2jpg(fb->buf, fb->len, fb->width, fb->height, PIXFORMAT_YUV422, 85, &jpg, &jpg_len);

  digitalWrite(FLASH_PIN, LOW);  // Apagar flash

  if (!ok || jpg_len == 0) {
    esp_camera_fb_return(fb);
    return "";
  }

  size_t b64_len = 0;
  mbedtls_base64_encode(NULL, 0, &b64_len, jpg, jpg_len);
  char *b64 = (char*)malloc(b64_len + 1);
  if (!b64) { free(jpg); esp_camera_fb_return(fb); return ""; }

  mbedtls_base64_encode((uint8_t*)b64, b64_len, &b64_len, jpg, jpg_len);
  b64[b64_len] = '\0';
  String resultado = String(b64);

  free(b64); free(jpg); esp_camera_fb_return(fb);
  return resultado;
}

// ==================== ENVÍO POR GPIO1 (U0TXD) ====================
void enviarPorUART(String base64, int luz) {
  Serial.print("<LUZ>"); Serial.print(luz); Serial.println("</LUZ>");
  Serial.println("<IMAGE_START>");

  size_t offset = 0;
  const int CHUNK = 512;
  while (offset < base64.length()) {
    int tam = min(CHUNK, (int)(base64.length() - offset));
    Serial.println(base64.substring(offset, offset + tam));
    offset += tam;
    delay(3);
  }

  Serial.println("<IMAGE_END>");
  Serial.flush();
}

// ==================== CICLO PRINCIPAL ====================
void ejecutarCiclo() {
  int luz = leerSensorLuz();
  String foto = capturarYConvertir();
  if (foto.length() > 100) {
    enviarPorUART(foto, luz);
  }
}

// ==================== SETUP ====================
void setup() {
  pinMode(FLASH_PIN, OUTPUT);
  digitalWrite(FLASH_PIN, LOW);
  pinMode(LDR_PIN, INPUT);

  // === CANAL LIMPIO: USAMOS UART0 (GPIO1) PARA COMUNICACIÓN ===
  Serial.begin(115200);           // TX = GPIO1 (U0TXD) → pin con header
  Serial.flush();
  delay(100);
  Serial.end();                   // ← CIERRA Serial USB → GPIO1 queda 100% limpio
  delay(100);
  Serial.begin(115200);           // ← REABRE SOLO PARA ENVÍO (no recibe logs)

  initCamera();

  delay(5000);
  ejecutarCiclo();
  ultimaFoto = millis();
}

// ==================== LOOP ====================
void loop() {
  if (millis() - ultimaFoto >= INTERVALO_FOTO) {
    ultimaFoto = millis();
    ejecutarCiclo();
  }
  delay(100);
}
