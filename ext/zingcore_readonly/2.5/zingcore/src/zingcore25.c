#include "zingcore25.h"

const char *zingcore25_version(void) {
  return "zingcore25/2.5 (WIP)";
}

uint32_t zingcore25_zabi_version(void) {
  return (uint32_t)ZINGCORE25_ZABI_VERSION;
}

int zingcore25_init(void) {
  if (!zi_caps_init()) return 0;
  if (!zi_async_init()) return 0;
  return 1;
}

const zi_cap_registry_v1 *zingcore25_cap_registry(void) {
  return zi_cap_registry();
}

const zi_async_registry_v1 *zingcore25_async_registry(void) {
  return zi_async_registry();
}

void zingcore25_reset_for_test(void) {
  zi_caps_reset_for_test();
  zi_async_reset_for_test();
}
