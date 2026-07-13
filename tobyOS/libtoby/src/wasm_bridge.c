/* wasm_bridge.c -- see toby/wasm_bridge.h. Reaches into wasm3's private
 * M3Module/M3Function/M3FuncType layout to expose function-import
 * metadata through a small stable C API. Compiled with -I third_party/
 * wasm3 so it can see the internal headers; nothing else in the tree
 * needs them. */

#include "m3_env.h"          /* M3Module, M3Function, M3FuncType (private) */
#include "toby/wasm_bridge.h"

/* A module's function array holds imports first (indices 0..numFuncImports-1),
 * each carrying its import record and funcType. M3FuncType.types[] lists
 * the return types first, then the argument types. */

static IM3Function import_func(IM3Module m, int i)
{
    if (!m || i < 0 || (u32)i >= m->numFuncImports)
        return 0;
    return &m->functions[i];
}

int toby_wasm_num_func_imports(IM3Module m)
{
    return m ? (int)m->numFuncImports : 0;
}

const char *toby_wasm_import_module(IM3Module m, int i)
{
    IM3Function f = import_func(m, i);
    return f ? f->import.moduleUtf8 : 0;
}

const char *toby_wasm_import_field(IM3Module m, int i)
{
    IM3Function f = import_func(m, i);
    return f ? f->import.fieldUtf8 : 0;
}

int toby_wasm_import_num_args(IM3Module m, int i)
{
    IM3Function f = import_func(m, i);
    return (f && f->funcType) ? (int)f->funcType->numArgs : 0;
}

int toby_wasm_import_num_rets(IM3Module m, int i)
{
    IM3Function f = import_func(m, i);
    return (f && f->funcType) ? (int)f->funcType->numRets : 0;
}

int toby_wasm_import_arg_type(IM3Module m, int i, int arg)
{
    IM3Function f = import_func(m, i);
    if (!f || !f->funcType) return 0;
    if (arg < 0 || (u32)arg >= f->funcType->numArgs) return 0;
    /* args follow the returns in types[] */
    return (int)f->funcType->types[f->funcType->numRets + arg];
}

int toby_wasm_import_ret_type(IM3Module m, int i, int ret)
{
    IM3Function f = import_func(m, i);
    if (!f || !f->funcType) return 0;
    if (ret < 0 || (u32)ret >= f->funcType->numRets) return 0;
    return (int)f->funcType->types[ret];
}

int toby_wasm_num_functions(IM3Module m)
{
    return m ? (int)m->numFunctions : 0;
}

const char *toby_wasm_func_export_name(IM3Module m, int idx)
{
    if (!m || idx < 0 || (u32)idx >= m->numFunctions)
        return 0;
    return m->functions[idx].export_name;
}

const char *toby_wasm_memory_export_name(IM3Module m)
{
    return m ? m->memoryExportName : 0;
}
