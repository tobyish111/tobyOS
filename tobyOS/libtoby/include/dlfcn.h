/* dlfcn.h -- Dynamic loading API (POSIX dlfcn).
 *
 * Provides dlopen/dlsym/dlclose/dlerror for runtime loading of
 * shared libraries on tobyOS.
 */

#ifndef _DLFCN_H
#define _DLFCN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Flags for dlopen */
#define RTLD_LAZY    0x0001  /* Lazy binding (resolve symbols on use) */
#define RTLD_NOW     0x0002  /* Immediate binding (resolve all now) */
#define RTLD_GLOBAL  0x0100  /* Symbols available for other libraries */
#define RTLD_LOCAL   0x0000  /* Symbols not available to other libs */

/* Special handles */
#define RTLD_DEFAULT ((void *)0)  /* Search default symbol scope */
#define RTLD_NEXT    ((void *)-1) /* Search next library in scope */

/* Load a shared library. Returns opaque handle, or NULL on failure. */
void *dlopen(const char *filename, int flags);

/* Look up a symbol in a loaded library. Returns address or NULL. */
void *dlsym(void *handle, const char *symbol);

/* Decrease refcount on library; unload when zero. Returns 0 on success. */
int dlclose(void *handle);

/* Return a string describing the last dlopen/dlsym/dlclose error.
 * Returns NULL if no error occurred since the last call. */
char *dlerror(void);

#ifdef __cplusplus
}
#endif

#endif /* _DLFCN_H */
