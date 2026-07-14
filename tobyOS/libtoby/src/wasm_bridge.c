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

int toby_wasm_mem_pages(IM3Runtime rt)
{
    return rt ? (int)rt->memory.numPages : 0;
}

int toby_wasm_mem_maxpages(IM3Runtime rt)
{
    return rt ? (int)rt->memory.maxPages : 0;
}

int toby_wasm_mem_grow(IM3Runtime rt, int delta_pages)
{
    if (!rt || delta_pages < 0) return -1;
    u32 old = rt->memory.numPages;
    /* ResizeMemory reallocs (moving the base but preserving contents) up
     * to the module's declared max; the memory.grow opcode uses the same
     * path, so JS- and wasm-initiated growth are identical. */
    M3Result r = ResizeMemory(rt, old + (u32)delta_pages);
    if (r) return -1;
    return (int)old;
}

int toby_wasm_memory_imported(IM3Module m)
{
    return (m && m->memoryImported) ? 1 : 0;
}

const char *toby_wasm_memory_import_module(IM3Module m)
{
    return (m && m->memoryImported) ? m->memoryImport.moduleUtf8 : 0;
}

const char *toby_wasm_memory_import_field(IM3Module m)
{
    return (m && m->memoryImported) ? m->memoryImport.fieldUtf8 : 0;
}

int toby_wasm_memory_init_pages(IM3Module m)
{
    return m ? (int)m->memoryInfo.initPages : 0;
}

int toby_wasm_memory_max_pages(IM3Module m)
{
    return m ? (int)m->memoryInfo.maxPages : 0;
}

int toby_wasm_mem_setup(IM3Runtime rt, int init_pages, int max_pages)
{
    if (!rt) return -1;
    rt->memory.maxPages = (max_pages > 0) ? (u32)max_pages : 65536;
    rt->memory.pageSize = 65536;
    /* Even 0 pages allocates the M3MemoryHeader, which is what
     * InitDataSegments checks for. */
    M3Result r = ResizeMemory(rt, (u32)(init_pages > 0 ? init_pages : 0));
    return r ? -1 : 0;
}

int toby_wasm_num_globals(IM3Module m)
{
    return m ? (int)m->numGlobals : 0;
}

const char *toby_wasm_global_export_name(IM3Module m, int i)
{
    if (!m || i < 0 || (u32)i >= m->numGlobals) return 0;
    return m->globals[i].name;      /* NULL unless exported */
}

const char *toby_wasm_table_export_name(IM3Module m)
{
    return m ? m->table0ExportName : 0;
}

int toby_wasm_table_size(IM3Module m)
{
    return m ? (int)m->table0Size : 0;
}
