#include "zi_hostlib25.h"

#include <stdint.h>

// Provided by the object produced by `lower` from examples/echo_zabi25_native.jsonl
extern int64_t zir_main(void);

int main(int argc, char **argv, char **envp) {
  if (!zi_hostlib25_init_all(argc, (const char *const *)argv, (const char *const *)envp)) {
    return 111;
  }
  (void)zir_main();
  return 0;
}
