#include "rand_xor.h"

/* Super fast random number generator.
 *
 * This rand_xorshift128plus function by Sebastiano Vigna belongs
 * to the public domain.
 */
uint64_t
rand_xorshift128plus(uint64_t *seed)
{
   uint64_t *s = seed;

   uint64_t s1 = s[0];
   const uint64_t s0 = s[1];
   s[0] = s0;
   s1 ^= s1 << 23;
   s[1] = s1 ^ s0 ^ (s1 >> 18) ^ (s0 >> 5);

   return s[1] + s0;
}
