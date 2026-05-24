/* quickjs.h -- Minimal JavaScript engine interface for tobyOS.
 *
 * This provides a QuickJS-compatible API for evaluating JavaScript
 * in the tobyOS browser. The implementation is a minimal interpreter
 * sufficient for basic DOM manipulation and event handling.
 */

#ifndef QUICKJS_H
#define QUICKJS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Core types ---- */

typedef struct JSRuntime JSRuntime;
typedef struct JSContext JSContext;
typedef uint64_t JSValue;
typedef uint32_t JSAtom;

/* Value tags (NaN-boxed) */
#define JS_TAG_INT       0
#define JS_TAG_BOOL      1
#define JS_TAG_NULL      2
#define JS_TAG_UNDEFINED 3
#define JS_TAG_STRING    4
#define JS_TAG_OBJECT    5
#define JS_TAG_FUNCTION  6
#define JS_TAG_FLOAT64   7
#define JS_TAG_EXCEPTION 8

/* Predefined values */
#define JS_UNDEFINED   ((JSValue)(((uint64_t)JS_TAG_UNDEFINED << 48)))
#define JS_NULL        ((JSValue)(((uint64_t)JS_TAG_NULL << 48)))
#define JS_TRUE        ((JSValue)(((uint64_t)JS_TAG_BOOL << 48) | 1))
#define JS_FALSE       ((JSValue)(((uint64_t)JS_TAG_BOOL << 48) | 0))
#define JS_EXCEPTION   ((JSValue)(((uint64_t)JS_TAG_EXCEPTION << 48)))

/* Property flags */
#define JS_PROP_WRITABLE    (1 << 0)
#define JS_PROP_ENUMERABLE  (1 << 1)
#define JS_PROP_CONFIGURABLE (1 << 2)
#define JS_PROP_C_W_E       (JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE | JS_PROP_ENUMERABLE)

/* Eval flags */
#define JS_EVAL_TYPE_GLOBAL   0
#define JS_EVAL_TYPE_MODULE   1
#define JS_EVAL_FLAG_STRICT   (1 << 3)

/* ---- C function callback type ---- */

typedef JSValue (*JSCFunction)(JSContext *ctx, JSValue this_val,
                               int argc, JSValue *argv);

/* ---- Runtime management ---- */

JSRuntime *JS_NewRuntime(void);
void JS_FreeRuntime(JSRuntime *rt);
void JS_SetMemoryLimit(JSRuntime *rt, size_t limit);

/* ---- Context management ---- */

JSContext *JS_NewContext(JSRuntime *rt);
void JS_FreeContext(JSContext *ctx);
JSRuntime *JS_GetRuntime(JSContext *ctx);

/* ---- Value creation ---- */

JSValue JS_NewInt32(JSContext *ctx, int32_t val);
JSValue JS_NewFloat64(JSContext *ctx, double val);
JSValue JS_NewString(JSContext *ctx, const char *str);
JSValue JS_NewStringLen(JSContext *ctx, const char *str, size_t len);
JSValue JS_NewObject(JSContext *ctx);
JSValue JS_NewArray(JSContext *ctx);
JSValue JS_NewCFunction(JSContext *ctx, JSCFunction func, const char *name, int length);

/* ---- Value extraction ---- */

int JS_ToInt32(JSContext *ctx, int32_t *pval, JSValue val);
int JS_ToFloat64(JSContext *ctx, double *pval, JSValue val);
const char *JS_ToCString(JSContext *ctx, JSValue val);
void JS_FreeCString(JSContext *ctx, const char *ptr);
int JS_ToBool(JSContext *ctx, JSValue val);

/* ---- Value type checking ---- */

int JS_IsNull(JSValue val);
int JS_IsUndefined(JSValue val);
int JS_IsString(JSValue val);
int JS_IsNumber(JSValue val);
int JS_IsObject(JSValue val);
int JS_IsFunction(JSContext *ctx, JSValue val);
int JS_IsArray(JSContext *ctx, JSValue val);
int JS_IsException(JSValue val);

/* ---- Object property access ---- */

JSValue JS_GetPropertyStr(JSContext *ctx, JSValue obj, const char *prop);
int JS_SetPropertyStr(JSContext *ctx, JSValue obj, const char *prop, JSValue val);
int JS_SetPropertyUint32(JSContext *ctx, JSValue obj, uint32_t idx, JSValue val);
JSValue JS_GetPropertyUint32(JSContext *ctx, JSValue obj, uint32_t idx);
int JS_DeletePropertyStr(JSContext *ctx, JSValue obj, const char *prop);

/* ---- Global object ---- */

JSValue JS_GetGlobalObject(JSContext *ctx);

/* ---- Function calls ---- */

JSValue JS_Call(JSContext *ctx, JSValue func, JSValue this_val,
                int argc, JSValue *argv);

/* ---- Evaluation ---- */

JSValue JS_Eval(JSContext *ctx, const char *input, size_t input_len,
                const char *filename, int eval_flags);

/* ---- Reference counting ---- */

JSValue JS_DupValue(JSContext *ctx, JSValue val);
void JS_FreeValue(JSContext *ctx, JSValue val);

/* ---- Error handling ---- */

JSValue JS_GetException(JSContext *ctx);
JSValue JS_Throw(JSContext *ctx, JSValue val);
JSValue JS_ThrowTypeError(JSContext *ctx, const char *fmt, ...);
JSValue JS_ThrowReferenceError(JSContext *ctx, const char *fmt, ...);

/* ---- Array operations ---- */

int64_t JS_GetArrayLength(JSContext *ctx, JSValue obj);

/* ---- JSON ---- */

JSValue JS_ParseJSON(JSContext *ctx, const char *buf, size_t len, const char *filename);
JSValue JS_JSONStringify(JSContext *ctx, JSValue val, JSValue replacer, JSValue space);

/* ---- Timer support (browser extension) ---- */

typedef int (*JS_TimerCallback)(JSContext *ctx, void *user_data);
int JS_SetTimeout(JSContext *ctx, JSValue func, int delay_ms);
int JS_SetInterval(JSContext *ctx, JSValue func, int interval_ms);
void JS_ClearTimer(JSContext *ctx, int timer_id);
void JS_ProcessTimers(JSContext *ctx);

#ifdef __cplusplus
}
#endif

#endif /* QUICKJS_H */
