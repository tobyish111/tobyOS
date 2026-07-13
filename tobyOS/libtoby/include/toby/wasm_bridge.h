/* toby/wasm_bridge.h -- thin accessors over wasm3's internal M3Module so
 * the browser can enumerate a module's function imports (module/field
 * name + signature) WITHOUT pulling wasm3's private headers -- and their
 * u8/u32 typedefs -- into the large browser translation unit.
 *
 * The JS WebAssembly bridge in the browser uses these to resolve each
 * import against a JS import object, then links a trampoline via the
 * public m3_LinkRawFunctionEx. Value-type ints match wasm3's M3ValueType
 * (1=i32, 2=i64, 3=f32, 4=f64). */

#ifndef TOBY_WASM_BRIDGE_H
#define TOBY_WASM_BRIDGE_H

#include <wasm3.h>          /* IM3Module */

#ifdef __cplusplus
extern "C" {
#endif

/* Number of imported functions (they occupy function indices 0..N-1). */
int         toby_wasm_num_func_imports(IM3Module m);

/* Import i's "module" and "field" UTF-8 names (NULL if out of range). */
const char *toby_wasm_import_module(IM3Module m, int i);
const char *toby_wasm_import_field(IM3Module m, int i);

/* Signature of import i: argument count, result count (0 or 1 in the MVP),
 * and the M3ValueType of a given arg/result. */
int         toby_wasm_import_num_args(IM3Module m, int i);
int         toby_wasm_import_num_rets(IM3Module m, int i);
int         toby_wasm_import_arg_type(IM3Module m, int i, int arg);
int         toby_wasm_import_ret_type(IM3Module m, int i, int ret);

/* Enumerate ALL functions (imports + defined) to discover exports.
 * toby_wasm_func_export_name returns the export name of function idx, or
 * NULL if that function isn't exported. */
int         toby_wasm_num_functions(IM3Module m);
const char *toby_wasm_func_export_name(IM3Module m, int idx);

/* The export name of the module's linear memory, or NULL if the memory
 * is not exported (or the module has none). */
const char *toby_wasm_memory_export_name(IM3Module m);

#ifdef __cplusplus
}
#endif

#endif /* TOBY_WASM_BRIDGE_H */
