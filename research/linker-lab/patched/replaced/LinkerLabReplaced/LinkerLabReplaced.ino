#include <Arduino.h>
#include <labguard.h>

void setup() {
  Serial.begin(115200);
  delay(500);

  const uint8_t type = 0xC0;
  const int result = lab_frame_sanity_check(type);

  Serial.println("PATCHED (object replacement):");
  Serial.printf("type=0x%02X\n", type);
  Serial.printf("result=%d\n", result);
  Serial.printf("archive_marker=0x%X\n", labguard_archive_marker());
}

void loop() {
  delay(1000);
}
