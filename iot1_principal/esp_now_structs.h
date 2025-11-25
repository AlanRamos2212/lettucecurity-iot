#ifndef ESP_NOW_STRUCTS_H
#define ESP_NOW_STRUCTS_H

#include <Arduino.h>

// ==================== TIPOS DE MENSAJES ====================
#define MSG_PING      0x00  // Ping con timestamp para sincronización
#define MSG_SENSORES  0x01  // Metadatos + sensores
#define MSG_IMAGEN    0x02  // Chunk de imagen base64

// ==================== MENSAJE DE PING (Sincronización) ====================
// IoT #1 envía este mensaje periódicamente para que IoT #2 tenga timestamp
struct MensajePing {
  uint8_t tipo;              // 0x00
  char timestamp[25];        // ISO8601: "2025-11-23T15:30:00Z"
} __attribute__((packed));

// ==================== MENSAJE DE SENSORES ====================
// IoT #2 envía primero este mensaje con todos los datos de sensores
struct MensajeSensores {
  uint8_t tipo;              // 0x01
  uint8_t idIot;             // ID del IoT (1 o 2)
  char timestamp[25];        // ISO8601 recibido del ping
  float temperatura;         // Sensor 1: Temperatura ambiental (°C)
  float humedadSuelo;        // Sensor 2: Humedad del suelo (%)
  float humedadAmbiental;    // Sensor 3: Humedad ambiental (%)
  float luz;                 // Sensor 4: Valor de luz (0-4095)
  uint8_t pir;               // Sensor 5: PIR (0 o 1)
  uint8_t camaraActiva;      // Sensor 6: Cámara activa (0 o 1)
  uint32_t imagenTamano;     // Tamaño total de la imagen base64
  uint16_t totalChunks;      // Número total de chunks de imagen
} __attribute__((packed));

// ==================== MENSAJE DE IMAGEN (Chunks) ====================
// IoT #2 envía múltiples mensajes de este tipo para transmitir la imagen
struct MensajeImagen {
  uint8_t tipo;              // 0x02
  uint8_t idIot;             // ID del IoT (1 o 2)
  uint16_t chunkIndex;       // Índice del chunk (0, 1, 2, ...)
  uint16_t chunkSize;        // Tamaño real de datos en este chunk
  uint8_t data[240];         // Payload de imagen base64 (240 bytes max)
} __attribute__((packed));

// ==================== VALIDACIÓN DE TAMAÑOS ====================
// ESP-NOW tiene límite de 250 bytes por mensaje
static_assert(sizeof(MensajePing) <= 250, "MensajePing excede 250 bytes");
static_assert(sizeof(MensajeSensores) <= 250, "MensajeSensores excede 250 bytes");
static_assert(sizeof(MensajeImagen) <= 250, "MensajeImagen excede 250 bytes");

#endif
