/* Force "cross" behaviour: no -run, no dlopen on .so in tcc_add_file for
 * the same-arch case. All output is still x86_64 ELF for tobyOS. */
#ifndef TOBYOS_TCC_H
#define TOBYOS_TCC_H
#undef TCC_IS_NATIVE
#undef CONFIG_TCC_BACKTRACE
#undef CONFIG_TCC_BCHECK
#endif
