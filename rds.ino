#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "driver/i2s.h"
#include <math.h>

// Pines PCM5102
#define I2S_BCK  26
#define I2S_WS   25
#define I2S_DOUT 22

// Configuración de audio
#define SAMPLE_RATE 228000
#define BUFFER_SIZE 512

// Datos RDS configurables
String rdsPS  = "FM TEST ";
String rdsRT  = "Texto de prueba RDS";
uint16_t rdsPI = 0x5401;  // Ejemplo PI para Argentina
uint8_t rdsPTY = 10;      // Pop Music
float rdsLevel = 0.10;    // Nivel por software (0.0 a 1.0)

// Portal cautivo
const byte DNS_PORT = 53;
DNSServer dnsServer;
WebServer server(80);
Preferences prefs;

// Estado portadora
float phase = 0;
float phaseInc;
int samplesPerBit;
int bitSampleCounter = 0;
int rdsBitPos = 0;

// Buffer de bits del grupo actual
#define RDS_GROUP_SIZE 104
uint8_t rdsBits[RDS_GROUP_SIZE];

// Rotación
int psSegment = 0;
int psSegmentsTotal = 1;
int rtSegment = 0;
int rtSegmentsTotal = 1;
int groupCounter = 0; // alterna 0A y 2A

// Polinomio CRC
#define RDS_POLY 0x1B9

// ---- Cálculo de CRC ----
uint16_t rdsCRC(uint16_t block) {
  uint16_t crc = 0;
  for (int i = 0; i < 16; i++) {
    crc ^= ((block >> (15 - i)) & 1) << 9;
    if (crc & 0x200) crc ^= RDS_POLY;
    crc &= 0x1FF;
  }
  return crc;
}

// ---- Construcción de grupos ----
void buildGroup0A(uint16_t pi, uint8_t pty, const char *ps, int segment) {
  uint16_t block1 = pi;
  uint16_t block2 = (0x0 << 11) | (pty << 5) | segment;
  uint16_t block3 = ((uint8_t)ps[segment * 2] << 8) | (uint8_t)ps[segment * 2 + 1];
  uint16_t block4 = 0;

  uint16_t b1 = (block1 << 10) | rdsCRC(block1);
  uint16_t b2 = (block2 << 10) | rdsCRC(block2 ^ 0x0);
  uint16_t b3 = (block3 << 10) | rdsCRC(block3 ^ 0x0);
  uint16_t b4 = (block4 << 10) | rdsCRC(block4 ^ 0x0);

  int bitIndex = 0;
  for (int i = 15; i >= 0; i--) rdsBits[bitIndex++] = (b1 >> i) & 1;
  for (int i = 15; i >= 0; i--) rdsBits[bitIndex++] = (b2 >> i) & 1;
  for (int i = 15; i >= 0; i--) rdsBits[bitIndex++] = (b3 >> i) & 1;
  for (int i = 15; i >= 0; i--) rdsBits[bitIndex++] = (b4 >> i) & 1;
}

void buildGroup2A(uint16_t pi, uint8_t pty, String rt, int segment) {
  char c1 = (segment * 4 < rt.length()) ? rt[segment * 4] : ' ';
  char c2 = (segment * 4 + 1 < rt.length()) ? rt[segment * 4 + 1] : ' ';
  char c3 = (segment * 4 + 2 < rt.length()) ? rt[segment * 4 + 2] : ' ';
  char c4 = (segment * 4 + 3 < rt.length()) ? rt[segment * 4 + 3] : ' ';

  uint16_t block1 = pi;
  uint16_t block2 = (0x2 << 11) | (pty << 5) | segment;
  uint16_t block3 = ((uint8_t)c1 << 8) | (uint8_t)c2;
  uint16_t block4 = ((uint8_t)c3 << 8) | (uint8_t)c4;

  uint16_t b1 = (block1 << 10) | rdsCRC(block1);
  uint16_t b2 = (block2 << 10) | rdsCRC(block2 ^ 0x0);
  uint16_t b3 = (block3 << 10) | rdsCRC(block3 ^ 0x0);
  uint16_t b4 = (block4 << 10) | rdsCRC(block4 ^ 0x0);

  int bitIndex = 0;
  for (int i = 15; i >= 0; i--) rdsBits[bitIndex++] = (b1 >> i) & 1;
  for (int i = 15; i >= 0; i--) rdsBits[bitIndex++] = (b2 >> i) & 1;
  for (int i = 15; i >= 0; i--) rdsBits[bitIndex++] = (b3 >> i) & 1;
  for (int i = 15; i >= 0; i--) rdsBits[bitIndex++] = (b4 >> i) & 1;
}

// ---- Generación de muestra ----
int32_t genRDSsample() {
  int bit = rdsBits[rdsBitPos];
  float s = sinf(phase) * (bit ? -rdsLevel : rdsLevel);

  phase += phaseInc;
  if (phase >= 2 * M_PI) phase -= 2 * M_PI;

  bitSampleCounter++;
  if (bitSampleCounter >= samplesPerBit) {
    bitSampleCounter = 0;
    rdsBitPos++;
    if (rdsBitPos >= RDS_GROUP_SIZE) {
      rdsBitPos = 0;
      if (groupCounter < 2) {
        buildGroup0A(rdsPI, rdsPTY, rdsPS.c_str(), psSegment);
        psSegment++;
        if (psSegment >= psSegmentsTotal) psSegment = 0;
        groupCounter++;
      } else {
        buildGroup2A(rdsPI, rdsPTY, rdsRT, rtSegment);
        rtSegment++;
        if (rtSegment >= rtSegmentsTotal) rtSegment = 0;
        groupCounter = 0;
      }
    }
  }
  return (int32_t)(s * 2147483647.0f);
}

// ---- Webserver ----
void handleRoot() {
  String html = "<html><body><h1>Config RDS</h1>"
                "<form method='GET' action='/save'>"
                "PS:<input name='ps' value='" + rdsPS + "'><br>"
                "RT:<input name='rt' value='" + rdsRT + "'><br>"
                "PI (hex):<input name='pi' value='" + String(rdsPI, HEX) + "'><br>"
                "PTY:<input name='pty' value='" + String(rdsPTY) + "'><br>"
                "Nivel (0.0-1.0):<input name='lvl' value='" + String(rdsLevel) + "'><br>"
                "<input type='submit' value='Guardar'>"
                "</form></body></html>";
  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.hasArg("ps"))  rdsPS  = server.arg("ps");
  if (server.hasArg("rt"))  rdsRT  = server.arg("rt");
  if (server.hasArg("pi"))  rdsPI  = strtol(server.arg("pi").c_str(), NULL, 16);
  if (server.hasArg("pty")) rdsPTY = server.arg("pty").toInt();
  if (server.hasArg("lvl")) rdsLevel = constrain(server.arg("lvl").toFloat(), 0.0, 1.0);

  prefs.putString("ps", rdsPS);
  prefs.putString("rt", rdsRT);
  prefs.putUShort("pi", rdsPI);
  prefs.putUChar("pty", rdsPTY);
  prefs.putFloat("lvl", rdsLevel);

  psSegmentsTotal = (rdsPS.length() + 1) / 2;
  rtSegmentsTotal = (rdsRT.length() + 3) / 4;

  server.send(200, "text/html", "<html><body>Guardado. <a href='/'>Volver</a></body></html>");
}

// ---- I2S ----
void setupI2S() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = 0,
    .dma_buf_count = 8,
    .dma_buf_len = 64,
    .use_apll = true
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_BCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_DOUT,
    .data_in_num = I2S_PIN_NO_CHANGE
  };

  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);
}

// ---- Setup ----
void setup() {
  Serial.begin(115200);

  prefs.begin("rds", false);
  rdsPS  = prefs.getString("ps", rdsPS);
  rdsRT  = prefs.getString("rt", rdsRT);
  rdsPI  = prefs.getUShort("pi", rdsPI);
  rdsPTY = prefs.getUChar("pty", rdsPTY);
  rdsLevel = prefs.getFloat("lvl", rdsLevel);

  psSegmentsTotal = (rdsPS.length() + 1) / 2;
  rtSegmentsTotal = (rdsRT.length() + 3) / 4;

  WiFi.mode(WIFI_AP);
  WiFi.softAP("RDS_Config", "12345678");
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/save", handleSave);
  server.begin();

  setupI2S();

  phaseInc = 2.0 * M_PI * 57000.0 / SAMPLE_RATE;
  samplesPerBit = SAMPLE_RATE / 1187;

  buildGroup0A(rdsPI, rdsPTY, rdsPS.c_str(), psSegment);
}

// ---- Loop ----
void loop() {
  dnsServer.processNextRequest();
  server.handleClient();

  int32_t buffer[BUFFER_SIZE];
  for (int i = 0; i < BUFFER_SIZE; i++) {
    buffer[i] = genRDSsample();
  }
  size_t bytes_written;
  i2s_write(I2S_NUM_0, buffer, sizeof(buffer), &bytes_written, portMAX_DELAY);
}
