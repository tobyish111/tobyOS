/* types.h -- common scalar types for the tobyOS kernel.
 *
 * Freestanding clang gives us <stdint.h> and <stddef.h>, so we only need
 * to centralise the imports plus a couple of project-wide aliases.
 */

#ifndef TOBYOS_TYPES_H
#define TOBYOS_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uintptr_t uptr;
typedef intptr_t  iptr;

#endif /* TOBYOS_TYPES_H */
