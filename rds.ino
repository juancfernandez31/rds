 (cd "$(git rev-parse --show-toplevel)" && git apply --3way <<'EOF' 
diff --git a/rds.ino b/rds.ino
index 075763ca546c4ef87a1ab70427c585e9999e39d1..80a250f6d884add100670eff358a4db4cd99d673 100644
--- a/rds.ino
+++ b/rds.ino
@@ -2,53 +2,56 @@
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
-WebServer server(80);
+WebServer server(80); // TODO: evaluar migrar a ESPAsyncWebServer para evitar bloqueos
 Preferences prefs;
 
+// Tarea FreeRTOS para manejar el audio I2S
+void i2sTask(void *pvParameters);
+
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
diff --git a/rds.ino b/rds.ino
index 075763ca546c4ef87a1ab70427c585e9999e39d1..80a250f6d884add100670eff358a4db4cd99d673 100644
--- a/rds.ino
+++ b/rds.ino
@@ -167,67 +170,75 @@ void handleSave() {
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
 
+// Genera audio de manera continua en una tarea dedicada
+void i2sTask(void *pvParameters) {
+  int32_t buffer[BUFFER_SIZE];
+  while (true) {
+    for (int i = 0; i < BUFFER_SIZE; i++) {
+      buffer[i] = genRDSsample();
+    }
+    size_t bytes_written;
+    i2s_write(I2S_NUM_0, buffer, sizeof(buffer), &bytes_written, portMAX_DELAY);
+  }
+}
+
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
+
+  // Inicia la tarea encargada de enviar datos por I2S
+  xTaskCreatePinnedToCore(i2sTask, "i2sTask", 4096, NULL, 1, NULL, 0);
 }
 
 // ---- Loop ----
 void loop() {
   dnsServer.processNextRequest();
   server.handleClient();
-
-  int32_t buffer[BUFFER_SIZE];
-  for (int i = 0; i < BUFFER_SIZE; i++) {
-    buffer[i] = genRDSsample();
-  }
-  size_t bytes_written;
-  i2s_write(I2S_NUM_0, buffer, sizeof(buffer), &bytes_written, portMAX_DELAY);
 }
 
EOF
)
