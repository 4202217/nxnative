// What the tests that link hle_nvn.c and hle_bigstack.c without the host need of it: weak definitions, so a test that defines its own keeps it
#include <stddef.h>
#include <stdint.h>
__attribute__((weak)) uint64_t g_t0_ns;   // process start time (host main.c), read by hle_nvn's per-frame timing
__attribute__((weak)) int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }   // no guest address space (hle_bigstack.c)
// No guest heap either (hle_svc.c is not linked): the pool page renew sees no renewable memory and never runs
__attribute__((weak)) int dm_guest_renewable(uint64_t a, size_t n) { (void)a; (void)n; return 0; }
