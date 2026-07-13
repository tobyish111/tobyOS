/* user_wasmtest/main.c -- /bin/wasmtest: runs a WebAssembly module on the
 * vendored wasm3 0.5.2 interpreter to prove the engine works freestanding
 * on tobyOS. Exercises i32 arithmetic, f32 arithmetic, linear-memory
 * load/store, and a HOST IMPORT (env.host_mul supplied from C) -- the same
 * pieces the browser's JS WebAssembly bridge relies on.
 *
 * Embedded module (programs/user_wasmtest/wasm_clip.h) exports:
 *   add(i32,i32)->i32          i32.add          -> add(7,35)=42
 *   fmul(f32,f32)->f32         f32.mul          -> fmul(1.5,2.0)=3.0
 *   mem_sum(i32,i32)->i32      i32.store/load   -> mem_sum(20,22)=42
 *   call_host(i32,i32)->i32    call import *2   -> call_host(6,7)=84
 *   memory "mem"               1 page
 *
 * "[wasm] ..." markers on serial; exit 0 iff all checks pass. */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "wasm3.h"
#include "m3_env.h"
#include "wasm_clip.h"

/* Host function imported by the module as env.host_mul: returns a*b. */
static const void *host_mul(IM3Runtime rt, IM3ImportContext ctx,
                            uint64_t *sp, void *mem) {
    (void)rt; (void)ctx; (void)mem;
    int32_t a = (int32_t)sp[1];
    int32_t b = (int32_t)sp[2];
    sp[0] = (uint64_t)(uint32_t)(a * b);
    return m3Err_none;
}

int main(void) {
    printf("[wasm] wasm3 %s: %d-byte module\n", M3_VERSION, (int)g_wasm_len);

    IM3Environment env = m3_NewEnvironment();
    IM3Runtime rt = m3_NewRuntime(env, 64 * 1024, NULL);
    if (!env || !rt) { printf("[wasm] env/runtime alloc FAIL\n"); return 1; }

    IM3Module mod;
    M3Result r = m3_ParseModule(env, &mod, g_wasm, (uint32_t)g_wasm_len);
    if (r) { printf("[wasm] parse FAIL: %s\n", r); return 1; }
    r = m3_LoadModule(rt, mod);
    if (r) { printf("[wasm] load FAIL: %s\n", r); return 1; }
    r = m3_LinkRawFunction(mod, "env", "host_mul", "i(ii)", &host_mul);
    if (r) { printf("[wasm] link import FAIL: %s\n", r); return 1; }

    int ok = 1;
    IM3Function f;
    int32_t res;

    r = m3_FindFunction(&f, rt, "add");
    if (r || m3_CallV(f, (int32_t)7, (int32_t)35) ||
        m3_GetResultsV(f, &res)) { printf("[wasm] add call FAIL\n"); return 1; }
    printf("[wasm] add(7,35) = %d\n", res);
    if (res != 42) { printf("[wasm] FAIL add != 42\n"); ok = 0; }

    r = m3_FindFunction(&f, rt, "mem_sum");
    if (r || m3_CallV(f, (int32_t)20, (int32_t)22) ||
        m3_GetResultsV(f, &res)) { printf("[wasm] mem_sum call FAIL\n"); return 1; }
    printf("[wasm] mem_sum(20,22) = %d\n", res);
    if (res != 42) { printf("[wasm] FAIL mem_sum != 42\n"); ok = 0; }

    r = m3_FindFunction(&f, rt, "call_host");
    if (r || m3_CallV(f, (int32_t)6, (int32_t)7) ||
        m3_GetResultsV(f, &res)) { printf("[wasm] call_host call FAIL\n"); return 1; }
    printf("[wasm] call_host(6,7) = %d\n", res);
    if (res != 84) { printf("[wasm] FAIL call_host != 84\n"); ok = 0; }

    float fr;
    r = m3_FindFunction(&f, rt, "fmul");
    if (r || m3_CallV(f, 1.5f, 2.0f) ||
        m3_GetResultsV(f, &fr)) { printf("[wasm] fmul call FAIL\n"); return 1; }
    printf("[wasm] fmul(1.5,2.0) = %d/1000\n", (int)(fr * 1000.0f));
    if ((int)(fr * 1000.0f) != 3000) { printf("[wasm] FAIL fmul != 3.0\n"); ok = 0; }

    /* linear memory: mem_sum stored 20 at mem[0]; read it back from C */
    uint32_t msize = 0;
    uint8_t *lm = m3_GetMemory(rt, &msize, 0);
    if (!lm || msize < 65536) { printf("[wasm] FAIL memory %u\n", msize); ok = 0; }
    else {
        int32_t at0; memcpy(&at0, lm, 4);
        printf("[wasm] mem[0] = %d, memsize = %u\n", at0, msize);
        if (at0 != 20) { printf("[wasm] FAIL mem[0] != 20\n"); ok = 0; }
    }

    m3_FreeRuntime(rt);
    m3_FreeEnvironment(env);

    printf("[wasm] engine self-test: %s\n", ok ? "ALL PASS" : "FAIL");
    return ok ? 0 : 1;
}
