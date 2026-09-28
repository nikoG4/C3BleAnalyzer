#include <stdint.h>

// Compilado solamente por la variante LAB; el build normal no define el símbolo.
#if defined(LAB_ENABLE_RAW_TX)
extern "C" __attribute__((used)) int ieee80211_raw_frame_sanity_check(
    int32_t, int32_t, int32_t) {
  return 0;
}
#endif
