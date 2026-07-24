/* HOST reference build for the MLKEMTEST checksums (NOT staged to the
 * initrd). Runs the identical selftest.c + kyber ref code natively on the
 * build host (x86-64, little-endian -- bit-identical results expected) and
 * prints the reference checksums for seeds 1..3. Compare against the
 * [mlkem] lines from the tobyOS boot:
 *
 *   cd programs/linux-mlkem
 *   gcc -O2 -o host_mlkem.exe host_main.c selftest.c kem.c indcpa.c \
 *       polyvec.c poly.c ntt.c reduce.c cbd.c symmetric-shake.c fips202.c \
 *       verify.c && ./host_mlkem.exe
 */
#include <stdio.h>
#include <stdint.h>

struct mlkem_result { uint64_t checksum; int fail_round; };
int mlkem_selftest(uint64_t seed, int rounds, struct mlkem_result *res);

int main(void) {
    for (uint64_t seed = 1; seed <= 3; seed++) {
        struct mlkem_result r;
        int rc = mlkem_selftest(seed, 40, &r);
        printf("seed%llu rc=%d checksum=0x%016llx fail_round=%d\n",
               (unsigned long long)seed, rc,
               (unsigned long long)r.checksum, r.fail_round);
    }
    return 0;
}
