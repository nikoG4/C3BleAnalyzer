#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int lab_frame_sanity_check(uint8_t type);
int labguard_archive_marker(void);

#ifdef __cplusplus
}
#endif
