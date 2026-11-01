/* calib.c: a fixed piece of work (5M xorshift steps), timed on its own thread's CPU clock and on the
 * wall clock. The CPU time does not depend on sharing the vCPU with the server, so if it grows, the
 * vCPU itself got slower (the host moved it to a slower core, or ran something else on its core). */
#include <stdint.h>
#include <stdio.h>
#include <time.h>
static double ms(clockid_t c) {
  struct timespec t;
  clock_gettime(c, &t);
  return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
int main(void) {
  double w0 = ms(CLOCK_MONOTONIC), c0 = ms(CLOCK_THREAD_CPUTIME_ID);
  volatile uint64_t x = 88172645463325252ULL;
  for (long i = 0; i < 5000000; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; }
  printf("%.1f %.1f\n", ms(CLOCK_THREAD_CPUTIME_ID) - c0, ms(CLOCK_MONOTONIC) - w0);
  return 0;
}
