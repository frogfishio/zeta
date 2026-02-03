#include <zi_hostlib25.h>

#include <stdint.h>

// This symbol must be provided by the object produced by `lower`.
// In your IR, export it via: {"k":"dir","d":"PUBLIC",...}
extern int64_t zir_main(void);

int main(int argc, char **argv, char **envp) {
  if (!zi_hostlib25_init_all(argc, (const char *const *)argv, (const char *const *)envp)) {
    return 111;
  }
  return (int)zir_main();
}
