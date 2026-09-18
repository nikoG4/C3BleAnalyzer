#include "labguard.h"

int lab_frame_sanity_check(uint8_t type) {
  if (type == 0xC0U) {
    return 0x102;
  }
  return 0;
}
