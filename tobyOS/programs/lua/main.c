/* lua/main.c -- Minimal Lua 5.4 interpreter for tobyOS.
 *
 * Implements a useful subset: lexer, recursive-descent parser that emits
 * bytecode, stack-based VM, and a small standard library (print, type,
 * tostring, tonumber, string.len/sub, table.insert, math.sqrt/sin/cos).
 *
 * Supports: nil, boolean, number (double), string, table, function.
 * Control: if/elseif/else/end, while/do/end, for/do/end, repeat/until,
 *          function/end, return, local, and/or/not.
 * REPL + file execution modes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

/* ---- Limits --------------------------------------------------------- */

#define LUA_MAX_STACK   256
#define LUA_MAX_CONSTS  512
#define LUA_MAX_CODE    4096
#define LUA_MAX_LOCALS  64
#define LUA_MAX_UPVALS  32
#define LUA_MAX_FUNCS   64
#define LUA_MAX_STR     4096
#define LUA_MAX_TABLE   256
#define LUA_MAX_GLOBALS 256
#define LUA_MAX_LINE    1024
#define LUA_MAX_CALLSTACK 64

/* ---- Value types ---------------------------------------------------- */

enum lua_type {
    LUA_TNIL, LUA_TBOOL, LUA_TNUM, LUA_TSTR, LUA_TTABLE, LUA_TFUNC, LUA_TCFUNC
};

struct lua_table;

typedef struct lua_value {
    enum lua_type type;
    union {
        int bval;
        double nval;
        char *sval;
        struct lua_table *tval;
        int fval;           /* function prototype index */
        int (*cfunc)(void); /* C function pointer */
    } u;
} lua_value;

static lua_value lua_nil(void) {
    lua_value v; v.type = LUA_TNIL; v.u.nval = 0; return v;
}
static lua_value lua_bool(int b) {
    lua_value v; v.type = LUA_TBOOL; v.u.bval = b; return v;
}
static lua_value lua_num(double n) {
    lua_value v; v.type = LUA_TNUM; v.u.nval = n; return v;
}
static lua_value lua_str(const char *s) {
    lua_value v; v.type = LUA_TSTR; v.u.sval = strdup(s); return v;
}

static int lua_truthy(lua_value v) {
    if (v.type == LUA_TNIL) return 0;
    if (v.type == LUA_TBOOL) return v.u.bval;
    return 1;
}

/* ---- String interning pool ------------------------------------------ */

static char *g_strings[LUA_MAX_STR];
static int g_nstrings;

static char *lua_intern(const char *s) {
    for (int i = 0; i < g_nstrings; i++)
        if (strcmp(g_strings[i], s) == 0) return g_strings[i];
    if (g_nstrings < LUA_MAX_STR) {
        g_strings[g_nstrings] = strdup(s);
        return g_strings[g_nstrings++];
    }
    return strdup(s);
}

/* ---- Tables --------------------------------------------------------- */

typedef struct lua_tentry {
    lua_value key;
    lua_value val;
} lua_tentry;

typedef struct lua_table {
    lua_tentry entries[LUA_MAX_TABLE];
    int count;
    int array_len;
} lua_table;

static lua_table *table_new(void) {
    lua_table *t = calloc(1, sizeof(lua_table));
    return t;
}

static lua_value table_get(lua_table *t, lua_value key) {
    if (key.type == LUA_TNUM) {
        int idx = (int)key.u.nval;
        if (idx >= 1 && idx <= t->array_len) {
            for (int i = 0; i < t->count; i++) {
                if (t->entries[i].key.type == LUA_TNUM &&
                    (int)t->entries[i].key.u.nval == idx)
                    return t->entries[i].val;
            }
        }
    }
    for (int i = 0; i < t->count; i++) {
        if (t->entries[i].key.type == key.type) {
            if (key.type == LUA_TNUM && t->entries[i].key.u.nval == key.u.nval)
                return t->entries[i].val;
            if (key.type == LUA_TSTR && strcmp(t->entries[i].key.u.sval, key.u.sval) == 0)
                return t->entries[i].val;
        }
    }
    return lua_nil();
}

static void table_set(lua_table *t, lua_value key, lua_value val) {
    for (int i = 0; i < t->count; i++) {
        if (t->entries[i].key.type == key.type) {
            if (key.type == LUA_TNUM && t->entries[i].key.u.nval == key.u.nval) {
                t->entries[i].val = val; return;
            }
            if (key.type == LUA_TSTR && strcmp(t->entries[i].key.u.sval, key.u.sval) == 0) {
                t->entries[i].val = val; return;
            }
        }
    }
    if (t->count < LUA_MAX_TABLE) {
        t->entries[t->count].key = key;
        t->entries[t->count].val = val;
        t->count++;
        if (key.type == LUA_TNUM) {
            int idx = (int)key.u.nval;
            if (idx > t->array_len) t->array_len = idx;
        }
    }
}

/* ---- Tokens --------------------------------------------------------- */

enum token_type {
    TOK_EOF, TOK_NUM, TOK_STR, TOK_NAME,
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT, TOK_CARET,
    TOK_HASH, TOK_CONCAT,
    TOK_EQ, TOK_NE, TOK_LT, TOK_LE, TOK_GT, TOK_GE,
    TOK_ASSIGN, TOK_LPAREN, TOK_RPAREN, TOK_LBRACE, TOK_RBRACE,
    TOK_LBRACKET, TOK_RBRACKET, TOK_DOT, TOK_COMMA, TOK_SEMICOLON, TOK_COLON,
    TOK_AND, TOK_BREAK, TOK_DO, TOK_ELSE, TOK_ELSEIF, TOK_END,
    TOK_FALSE, TOK_FOR, TOK_FUNCTION, TOK_IF, TOK_IN, TOK_LOCAL,
    TOK_NIL, TOK_NOT, TOK_OR, TOK_REPEAT, TOK_RETURN, TOK_THEN,
    TOK_TRUE, TOK_UNTIL, TOK_WHILE
};

static const char *keywords[] = {
    "and","break","do","else","elseif","end","false","for","function",
    "if","in","local","nil","not","or","repeat","return","then","true",
    "until","while", NULL
};
static const int kw_tokens[] = {
    TOK_AND,TOK_BREAK,TOK_DO,TOK_ELSE,TOK_ELSEIF,TOK_END,TOK_FALSE,
    TOK_FOR,TOK_FUNCTION,TOK_IF,TOK_IN,TOK_LOCAL,TOK_NIL,TOK_NOT,
    TOK_OR,TOK_REPEAT,TOK_RETURN,TOK_THEN,TOK_TRUE,TOK_UNTIL,TOK_WHILE
};

typedef struct {
    int type;
    double nval;
    char sval[256];
} token;

typedef struct {
    const char *src;
    int pos;
    int line;
    token cur;
} lexer;

static void lex_error(lexer *L, const char *msg) {
    fprintf(stderr, "lua: line %d: %s\n", L->line, msg);
}

static void lex_skip_ws(lexer *L) {
    while (L->src[L->pos]) {
        char c = L->src[L->pos];
        if (c == '\n') { L->pos++; L->line++; }
        else if (c == ' ' || c == '\t' || c == '\r') { L->pos++; }
        else if (c == '-' && L->src[L->pos+1] == '-') {
            L->pos += 2;
            while (L->src[L->pos] && L->src[L->pos] != '\n') L->pos++;
        } else break;
    }
}

static void lex_next(lexer *L) {
    lex_skip_ws(L);
    if (!L->src[L->pos]) { L->cur.type = TOK_EOF; return; }

    char c = L->src[L->pos];

    if (isdigit(c) || (c == '.' && isdigit(L->src[L->pos+1]))) {
        char *end;
        L->cur.nval = strtod(&L->src[L->pos], &end);
        L->pos = (int)(end - L->src);
        L->cur.type = TOK_NUM;
        return;
    }

    if (isalpha(c) || c == '_') {
        int i = 0;
        while (isalnum(L->src[L->pos]) || L->src[L->pos] == '_') {
            if (i < 255) L->cur.sval[i++] = L->src[L->pos];
            L->pos++;
        }
        L->cur.sval[i] = '\0';
        for (int k = 0; keywords[k]; k++) {
            if (strcmp(L->cur.sval, keywords[k]) == 0) {
                L->cur.type = kw_tokens[k]; return;
            }
        }
        L->cur.type = TOK_NAME;
        return;
    }

    if (c == '"' || c == '\'') {
        char quote = c; L->pos++;
        int i = 0;
        while (L->src[L->pos] && L->src[L->pos] != quote) {
            if (L->src[L->pos] == '\\') {
                L->pos++;
                char esc = L->src[L->pos];
                if (esc == 'n') { if (i<255) L->cur.sval[i++] = '\n'; }
                else if (esc == 't') { if (i<255) L->cur.sval[i++] = '\t'; }
                else if (esc == '\\') { if (i<255) L->cur.sval[i++] = '\\'; }
                else if (esc == quote) { if (i<255) L->cur.sval[i++] = quote; }
                else { if (i<255) L->cur.sval[i++] = esc; }
            } else {
                if (L->src[L->pos] == '\n') L->line++;
                if (i<255) L->cur.sval[i++] = L->src[L->pos];
            }
            L->pos++;
        }
        L->cur.sval[i] = '\0';
        if (L->src[L->pos] == quote) L->pos++;
        L->cur.type = TOK_STR;
        return;
    }

    L->pos++;
    switch (c) {
    case '+': L->cur.type = TOK_PLUS; return;
    case '-': L->cur.type = TOK_MINUS; return;
    case '*': L->cur.type = TOK_STAR; return;
    case '/': L->cur.type = TOK_SLASH; return;
    case '%': L->cur.type = TOK_PERCENT; return;
    case '^': L->cur.type = TOK_CARET; return;
    case '#': L->cur.type = TOK_HASH; return;
    case '(': L->cur.type = TOK_LPAREN; return;
    case ')': L->cur.type = TOK_RPAREN; return;
    case '{': L->cur.type = TOK_LBRACE; return;
    case '}': L->cur.type = TOK_RBRACE; return;
    case '[': L->cur.type = TOK_LBRACKET; return;
    case ']': L->cur.type = TOK_RBRACKET; return;
    case ',': L->cur.type = TOK_COMMA; return;
    case ';': L->cur.type = TOK_SEMICOLON; return;
    case ':': L->cur.type = TOK_COLON; return;
    case '.':
        if (L->src[L->pos] == '.') { L->pos++; L->cur.type = TOK_CONCAT; }
        else L->cur.type = TOK_DOT;
        return;
    case '=':
        if (L->src[L->pos] == '=') { L->pos++; L->cur.type = TOK_EQ; }
        else L->cur.type = TOK_ASSIGN;
        return;
    case '~':
        if (L->src[L->pos] == '=') { L->pos++; L->cur.type = TOK_NE; }
        else { lex_error(L, "unexpected '~'"); L->cur.type = TOK_EOF; }
        return;
    case '<':
        if (L->src[L->pos] == '=') { L->pos++; L->cur.type = TOK_LE; }
        else L->cur.type = TOK_LT;
        return;
    case '>':
        if (L->src[L->pos] == '=') { L->pos++; L->cur.type = TOK_GE; }
        else L->cur.type = TOK_GT;
        return;
    default:
        lex_error(L, "unexpected character");
        L->cur.type = TOK_EOF;
    }
}

/* ---- Bytecode ------------------------------------------------------- */

enum opcode {
    OP_NOP, OP_CONST, OP_NIL, OP_TRUE, OP_FALSE,
    OP_POP, OP_DUP,
    OP_GETLOCAL, OP_SETLOCAL, OP_GETGLOBAL, OP_SETGLOBAL,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_NEG, OP_POW,
    OP_CONCAT, OP_LEN,
    OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE,
    OP_NOT, OP_AND, OP_OR,
    OP_JMP, OP_JMPF, OP_JMPT,
    OP_CALL, OP_RETURN,
    OP_NEWTABLE, OP_GETFIELD, OP_SETFIELD,
    OP_GETTABLE, OP_SETTABLE,
    OP_CLOSURE, OP_PRINT
};

typedef struct {
    unsigned char op;
    int arg;
} instruction;

typedef struct {
    char name[64];
    int scope_depth;
} local_info;

typedef struct lua_proto {
    instruction code[LUA_MAX_CODE];
    int ncode;
    lua_value consts[LUA_MAX_CONSTS];
    int nconsts;
    local_info locals[LUA_MAX_LOCALS];
    int nlocals;
    int nparams;
    int max_stack;
    char name[64];
} lua_proto;

/* ---- Compiler state ------------------------------------------------- */

typedef struct {
    lexer lex;
    lua_proto protos[LUA_MAX_FUNCS];
    int nprotos;
    int current_proto;
    int scope_depth;
    int had_error;
} compiler;

static lua_proto *cur_proto(compiler *C) {
    return &C->protos[C->current_proto];
}

static void compile_error(compiler *C, const char *msg) {
    fprintf(stderr, "lua: compile error line %d: %s\n", C->lex.line, msg);
    C->had_error = 1;
}

static int emit(compiler *C, unsigned char op, int arg) {
    lua_proto *p = cur_proto(C);
    if (p->ncode >= LUA_MAX_CODE) { compile_error(C, "code overflow"); return -1; }
    p->code[p->ncode].op = op;
    p->code[p->ncode].arg = arg;
    return p->ncode++;
}

static int add_const(compiler *C, lua_value v) {
    lua_proto *p = cur_proto(C);
    for (int i = 0; i < p->nconsts; i++) {
        if (v.type == p->consts[i].type) {
            if (v.type == LUA_TNUM && v.u.nval == p->consts[i].u.nval) return i;
            if (v.type == LUA_TSTR && strcmp(v.u.sval, p->consts[i].u.sval) == 0) return i;
        }
    }
    if (p->nconsts >= LUA_MAX_CONSTS) { compile_error(C, "too many constants"); return 0; }
    p->consts[p->nconsts] = v;
    return p->nconsts++;
}

static int add_local(compiler *C, const char *name) {
    lua_proto *p = cur_proto(C);
    if (p->nlocals >= LUA_MAX_LOCALS) { compile_error(C, "too many locals"); return -1; }
    strncpy(p->locals[p->nlocals].name, name, 63);
    p->locals[p->nlocals].scope_depth = C->scope_depth;
    return p->nlocals++;
}

static int resolve_local(compiler *C, const char *name) {
    lua_proto *p = cur_proto(C);
    for (int i = p->nlocals - 1; i >= 0; i--)
        if (strcmp(p->locals[i].name, name) == 0) return i;
    return -1;
}

static void patch_jmp(compiler *C, int addr) {
    cur_proto(C)->code[addr].arg = cur_proto(C)->ncode;
}

/* ---- Forward declarations ------------------------------------------- */

static void parse_expr(compiler *C);
static void parse_stat(compiler *C);
static void parse_block(compiler *C);

static void expect(compiler *C, int tok, const char *msg) {
    if (C->lex.cur.type != tok) { compile_error(C, msg); return; }
    lex_next(&C->lex);
}

/* ---- Expression parsing (Pratt-style precedence climbing) ----------- */

static void parse_primary(compiler *C) {
    switch (C->lex.cur.type) {
    case TOK_NUM:
        emit(C, OP_CONST, add_const(C, lua_num(C->lex.cur.nval)));
        lex_next(&C->lex);
        break;
    case TOK_STR:
        emit(C, OP_CONST, add_const(C, lua_str(C->lex.cur.sval)));
        lex_next(&C->lex);
        break;
    case TOK_TRUE:  emit(C, OP_TRUE, 0); lex_next(&C->lex); break;
    case TOK_FALSE: emit(C, OP_FALSE, 0); lex_next(&C->lex); break;
    case TOK_NIL:   emit(C, OP_NIL, 0); lex_next(&C->lex); break;
    case TOK_NAME: {
        char name[256];
        strncpy(name, C->lex.cur.sval, 255); name[255] = '\0';
        lex_next(&C->lex);
        int local = resolve_local(C, name);
        if (local >= 0) emit(C, OP_GETLOCAL, local);
        else emit(C, OP_GETGLOBAL, add_const(C, lua_str(name)));
        break;
    }
    case TOK_LPAREN:
        lex_next(&C->lex);
        parse_expr(C);
        expect(C, TOK_RPAREN, "expected ')'");
        break;
    case TOK_LBRACE: {
        lex_next(&C->lex);
        emit(C, OP_NEWTABLE, 0);
        int idx = 1;
        while (C->lex.cur.type != TOK_RBRACE && C->lex.cur.type != TOK_EOF) {
            if (C->lex.cur.type == TOK_LBRACKET) {
                lex_next(&C->lex);
                emit(C, OP_DUP, 0);
                parse_expr(C);
                expect(C, TOK_RBRACKET, "expected ']'");
                expect(C, TOK_ASSIGN, "expected '='");
                parse_expr(C);
                emit(C, OP_SETTABLE, 0);
            } else if (C->lex.cur.type == TOK_NAME) {
                char fname[256];
                strncpy(fname, C->lex.cur.sval, 255); fname[255] = '\0';
                int save_pos = C->lex.pos;
                int save_line = C->lex.line;
                token save_cur = C->lex.cur;
                lex_next(&C->lex);
                if (C->lex.cur.type == TOK_ASSIGN) {
                    lex_next(&C->lex);
                    emit(C, OP_DUP, 0);
                    emit(C, OP_CONST, add_const(C, lua_str(fname)));
                    parse_expr(C);
                    emit(C, OP_SETTABLE, 0);
                } else {
                    C->lex.pos = save_pos;
                    C->lex.line = save_line;
                    C->lex.cur = save_cur;
                    emit(C, OP_DUP, 0);
                    emit(C, OP_CONST, add_const(C, lua_num(idx++)));
                    parse_expr(C);
                    emit(C, OP_SETTABLE, 0);
                }
            } else {
                emit(C, OP_DUP, 0);
                emit(C, OP_CONST, add_const(C, lua_num(idx++)));
                parse_expr(C);
                emit(C, OP_SETTABLE, 0);
            }
            if (C->lex.cur.type == TOK_COMMA || C->lex.cur.type == TOK_SEMICOLON)
                lex_next(&C->lex);
        }
        expect(C, TOK_RBRACE, "expected '}'");
        break;
    }
    case TOK_MINUS:
        lex_next(&C->lex);
        parse_primary(C);
        emit(C, OP_NEG, 0);
        break;
    case TOK_NOT:
        lex_next(&C->lex);
        parse_primary(C);
        emit(C, OP_NOT, 0);
        break;
    case TOK_HASH:
        lex_next(&C->lex);
        parse_primary(C);
        emit(C, OP_LEN, 0);
        break;
    case TOK_FUNCTION: {
        lex_next(&C->lex);
        int fn_idx = C->nprotos++;
        if (fn_idx >= LUA_MAX_FUNCS) { compile_error(C, "too many functions"); break; }
        memset(&C->protos[fn_idx], 0, sizeof(lua_proto));
        snprintf(C->protos[fn_idx].name, 64, "anon_%d", fn_idx);
        int saved = C->current_proto;
        int saved_depth = C->scope_depth;
        C->current_proto = fn_idx;
        C->scope_depth = 0;
        expect(C, TOK_LPAREN, "expected '('");
        while (C->lex.cur.type == TOK_NAME) {
            add_local(C, C->lex.cur.sval);
            C->protos[fn_idx].nparams++;
            lex_next(&C->lex);
            if (C->lex.cur.type == TOK_COMMA) lex_next(&C->lex);
        }
        expect(C, TOK_RPAREN, "expected ')'");
        parse_block(C);
        emit(C, OP_NIL, 0);
        emit(C, OP_RETURN, 0);
        expect(C, TOK_END, "expected 'end'");
        C->current_proto = saved;
        C->scope_depth = saved_depth;
        emit(C, OP_CLOSURE, fn_idx);
        break;
    }
    default:
        compile_error(C, "unexpected token in expression");
        lex_next(&C->lex);
    }
}

static void parse_postfix(compiler *C) {
    parse_primary(C);
    for (;;) {
        if (C->lex.cur.type == TOK_DOT) {
            lex_next(&C->lex);
            if (C->lex.cur.type != TOK_NAME) { compile_error(C, "expected field name"); break; }
            emit(C, OP_GETFIELD, add_const(C, lua_str(C->lex.cur.sval)));
            lex_next(&C->lex);
        } else if (C->lex.cur.type == TOK_LBRACKET) {
            lex_next(&C->lex);
            parse_expr(C);
            expect(C, TOK_RBRACKET, "expected ']'");
            emit(C, OP_GETTABLE, 0);
        } else if (C->lex.cur.type == TOK_LPAREN) {
            lex_next(&C->lex);
            int nargs = 0;
            while (C->lex.cur.type != TOK_RPAREN && C->lex.cur.type != TOK_EOF) {
                parse_expr(C);
                nargs++;
                if (C->lex.cur.type == TOK_COMMA) lex_next(&C->lex);
            }
            expect(C, TOK_RPAREN, "expected ')'");
            emit(C, OP_CALL, nargs);
        } else if (C->lex.cur.type == TOK_COLON) {
            lex_next(&C->lex);
            if (C->lex.cur.type != TOK_NAME) { compile_error(C, "expected method name"); break; }
            char mname[256];
            strncpy(mname, C->lex.cur.sval, 255); mname[255] = '\0';
            lex_next(&C->lex);
            emit(C, OP_DUP, 0);
            emit(C, OP_GETFIELD, add_const(C, lua_str(mname)));
            expect(C, TOK_LPAREN, "expected '('");
            int nargs = 1;
            while (C->lex.cur.type != TOK_RPAREN && C->lex.cur.type != TOK_EOF) {
                parse_expr(C);
                nargs++;
                if (C->lex.cur.type == TOK_COMMA) lex_next(&C->lex);
            }
            expect(C, TOK_RPAREN, "expected ')'");
            emit(C, OP_CALL, nargs);
        } else break;
    }
}

static int get_binop_prec(int tok) {
    switch (tok) {
    case TOK_OR:     return 1;
    case TOK_AND:    return 2;
    case TOK_LT: case TOK_LE: case TOK_GT: case TOK_GE:
    case TOK_EQ: case TOK_NE: return 3;
    case TOK_CONCAT: return 4;
    case TOK_PLUS: case TOK_MINUS: return 5;
    case TOK_STAR: case TOK_SLASH: case TOK_PERCENT: return 6;
    case TOK_CARET:  return 8;
    default:         return -1;
    }
}

static int tok_to_op(int tok) {
    switch (tok) {
    case TOK_PLUS:    return OP_ADD;
    case TOK_MINUS:   return OP_SUB;
    case TOK_STAR:    return OP_MUL;
    case TOK_SLASH:   return OP_DIV;
    case TOK_PERCENT: return OP_MOD;
    case TOK_CARET:   return OP_POW;
    case TOK_CONCAT:  return OP_CONCAT;
    case TOK_EQ:      return OP_EQ;
    case TOK_NE:      return OP_NE;
    case TOK_LT:      return OP_LT;
    case TOK_LE:      return OP_LE;
    case TOK_GT:      return OP_GT;
    case TOK_GE:      return OP_GE;
    default:          return OP_NOP;
    }
}

static void parse_expr_prec(compiler *C, int min_prec) {
    parse_postfix(C);
    while (1) {
        int prec = get_binop_prec(C->lex.cur.type);
        if (prec < min_prec) break;
        int tok = C->lex.cur.type;
        if (tok == TOK_AND) {
            lex_next(&C->lex);
            int jmp = emit(C, OP_JMPF, 0);
            emit(C, OP_POP, 0);
            parse_expr_prec(C, prec + 1);
            patch_jmp(C, jmp);
            continue;
        }
        if (tok == TOK_OR) {
            lex_next(&C->lex);
            int jmp = emit(C, OP_JMPT, 0);
            emit(C, OP_POP, 0);
            parse_expr_prec(C, prec + 1);
            patch_jmp(C, jmp);
            continue;
        }
        lex_next(&C->lex);
        int right_assoc = (tok == TOK_CARET || tok == TOK_CONCAT);
        parse_expr_prec(C, right_assoc ? prec : prec + 1);
        emit(C, tok_to_op(tok), 0);
    }
}

static void parse_expr(compiler *C) {
    parse_expr_prec(C, 0);
}

/* ---- Statement parsing ---------------------------------------------- */

static void parse_block(compiler *C) {
    while (C->lex.cur.type != TOK_END && C->lex.cur.type != TOK_ELSE &&
           C->lex.cur.type != TOK_ELSEIF && C->lex.cur.type != TOK_UNTIL &&
           C->lex.cur.type != TOK_EOF) {
        parse_stat(C);
    }
}

static void parse_stat(compiler *C) {
    if (C->had_error) { lex_next(&C->lex); return; }

    switch (C->lex.cur.type) {
    case TOK_SEMICOLON:
        lex_next(&C->lex);
        break;

    case TOK_LOCAL: {
        lex_next(&C->lex);
        if (C->lex.cur.type == TOK_FUNCTION) {
            lex_next(&C->lex);
            if (C->lex.cur.type != TOK_NAME) { compile_error(C, "expected function name"); break; }
            char fname[256];
            strncpy(fname, C->lex.cur.sval, 255); fname[255] = '\0';
            int slot = add_local(C, fname);
            lex_next(&C->lex);
            int fn_idx = C->nprotos++;
            memset(&C->protos[fn_idx], 0, sizeof(lua_proto));
            strncpy(C->protos[fn_idx].name, fname, 63);
            int saved = C->current_proto;
            int saved_depth = C->scope_depth;
            C->current_proto = fn_idx;
            C->scope_depth = 0;
            expect(C, TOK_LPAREN, "expected '('");
            while (C->lex.cur.type == TOK_NAME) {
                add_local(C, C->lex.cur.sval);
                C->protos[fn_idx].nparams++;
                lex_next(&C->lex);
                if (C->lex.cur.type == TOK_COMMA) lex_next(&C->lex);
            }
            expect(C, TOK_RPAREN, "expected ')'");
            parse_block(C);
            emit(C, OP_NIL, 0);
            emit(C, OP_RETURN, 0);
            expect(C, TOK_END, "expected 'end'");
            C->current_proto = saved;
            C->scope_depth = saved_depth;
            emit(C, OP_CLOSURE, fn_idx);
            emit(C, OP_SETLOCAL, slot);
            break;
        }
        if (C->lex.cur.type != TOK_NAME) { compile_error(C, "expected name after 'local'"); break; }
        char vname[256];
        strncpy(vname, C->lex.cur.sval, 255); vname[255] = '\0';
        lex_next(&C->lex);
        int slot = add_local(C, vname);
        if (C->lex.cur.type == TOK_ASSIGN) {
            lex_next(&C->lex);
            parse_expr(C);
        } else {
            emit(C, OP_NIL, 0);
        }
        emit(C, OP_SETLOCAL, slot);
        break;
    }

    case TOK_IF: {
        lex_next(&C->lex);
        parse_expr(C);
        int jmp_else = emit(C, OP_JMPF, 0);
        emit(C, OP_POP, 0);
        expect(C, TOK_THEN, "expected 'then'");
        parse_block(C);
        if (C->lex.cur.type == TOK_ELSEIF || C->lex.cur.type == TOK_ELSE) {
            int jmp_end = emit(C, OP_JMP, 0);
            patch_jmp(C, jmp_else);
            emit(C, OP_POP, 0);
            if (C->lex.cur.type == TOK_ELSE) {
                lex_next(&C->lex);
                parse_block(C);
            } else {
                C->lex.cur.type = TOK_IF;
                parse_stat(C);
                patch_jmp(C, jmp_end);
                return;
            }
            patch_jmp(C, jmp_end);
        } else {
            patch_jmp(C, jmp_else);
            emit(C, OP_POP, 0);
        }
        expect(C, TOK_END, "expected 'end'");
        break;
    }

    case TOK_WHILE: {
        lex_next(&C->lex);
        int loop_start = cur_proto(C)->ncode;
        parse_expr(C);
        int jmp_exit = emit(C, OP_JMPF, 0);
        emit(C, OP_POP, 0);
        expect(C, TOK_DO, "expected 'do'");
        parse_block(C);
        emit(C, OP_JMP, loop_start);
        patch_jmp(C, jmp_exit);
        emit(C, OP_POP, 0);
        expect(C, TOK_END, "expected 'end'");
        break;
    }

    case TOK_REPEAT: {
        lex_next(&C->lex);
        int loop_start = cur_proto(C)->ncode;
        parse_block(C);
        expect(C, TOK_UNTIL, "expected 'until'");
        parse_expr(C);
        int jmp = emit(C, OP_JMPF, 0);
        emit(C, OP_POP, 0);
        emit(C, OP_JMP, loop_start + 1);
        patch_jmp(C, jmp);
        emit(C, OP_POP, 0);
        break;
    }

    case TOK_FOR: {
        lex_next(&C->lex);
        if (C->lex.cur.type != TOK_NAME) { compile_error(C, "expected var"); break; }
        char iname[256];
        strncpy(iname, C->lex.cur.sval, 255); iname[255] = '\0';
        lex_next(&C->lex);
        expect(C, TOK_ASSIGN, "expected '='");
        int slot = add_local(C, iname);
        parse_expr(C);
        emit(C, OP_SETLOCAL, slot);
        expect(C, TOK_COMMA, "expected ','");
        parse_expr(C);
        int limit_slot = add_local(C, "(limit)");
        emit(C, OP_SETLOCAL, limit_slot);
        int step_slot = add_local(C, "(step)");
        if (C->lex.cur.type == TOK_COMMA) {
            lex_next(&C->lex);
            parse_expr(C);
        } else {
            emit(C, OP_CONST, add_const(C, lua_num(1)));
        }
        emit(C, OP_SETLOCAL, step_slot);
        expect(C, TOK_DO, "expected 'do'");
        int loop_start = cur_proto(C)->ncode;
        emit(C, OP_GETLOCAL, slot);
        emit(C, OP_GETLOCAL, limit_slot);
        emit(C, OP_LE, 0);
        int jmp_exit = emit(C, OP_JMPF, 0);
        emit(C, OP_POP, 0);
        parse_block(C);
        emit(C, OP_GETLOCAL, slot);
        emit(C, OP_GETLOCAL, step_slot);
        emit(C, OP_ADD, 0);
        emit(C, OP_SETLOCAL, slot);
        emit(C, OP_JMP, loop_start);
        patch_jmp(C, jmp_exit);
        emit(C, OP_POP, 0);
        expect(C, TOK_END, "expected 'end'");
        cur_proto(C)->nlocals -= 2;
        break;
    }

    case TOK_FUNCTION: {
        lex_next(&C->lex);
        if (C->lex.cur.type != TOK_NAME) { compile_error(C, "expected function name"); break; }
        char fname[256];
        strncpy(fname, C->lex.cur.sval, 255); fname[255] = '\0';
        lex_next(&C->lex);
        int fn_idx = C->nprotos++;
        if (fn_idx >= LUA_MAX_FUNCS) { compile_error(C, "too many functions"); break; }
        memset(&C->protos[fn_idx], 0, sizeof(lua_proto));
        strncpy(C->protos[fn_idx].name, fname, 63);
        int saved = C->current_proto;
        int saved_depth = C->scope_depth;
        C->current_proto = fn_idx;
        C->scope_depth = 0;
        expect(C, TOK_LPAREN, "expected '('");
        while (C->lex.cur.type == TOK_NAME) {
            add_local(C, C->lex.cur.sval);
            C->protos[fn_idx].nparams++;
            lex_next(&C->lex);
            if (C->lex.cur.type == TOK_COMMA) lex_next(&C->lex);
        }
        expect(C, TOK_RPAREN, "expected ')'");
        parse_block(C);
        emit(C, OP_NIL, 0);
        emit(C, OP_RETURN, 0);
        expect(C, TOK_END, "expected 'end'");
        C->current_proto = saved;
        C->scope_depth = saved_depth;
        emit(C, OP_CLOSURE, fn_idx);
        emit(C, OP_SETGLOBAL, add_const(C, lua_str(fname)));
        break;
    }

    case TOK_RETURN: {
        lex_next(&C->lex);
        if (C->lex.cur.type == TOK_END || C->lex.cur.type == TOK_ELSE ||
            C->lex.cur.type == TOK_ELSEIF || C->lex.cur.type == TOK_UNTIL ||
            C->lex.cur.type == TOK_EOF || C->lex.cur.type == TOK_SEMICOLON) {
            emit(C, OP_NIL, 0);
        } else {
            parse_expr(C);
        }
        emit(C, OP_RETURN, 0);
        break;
    }

    default: {
        parse_expr(C);
        if (C->lex.cur.type == TOK_ASSIGN) {
            instruction *last = &cur_proto(C)->code[cur_proto(C)->ncode - 1];
            int prev_op = last->op;
            int prev_arg = last->arg;
            cur_proto(C)->ncode--;
            lex_next(&C->lex);
            parse_expr(C);
            if (prev_op == OP_GETLOCAL) emit(C, OP_SETLOCAL, prev_arg);
            else if (prev_op == OP_GETGLOBAL) emit(C, OP_SETGLOBAL, prev_arg);
            else if (prev_op == OP_GETFIELD) {
                emit(C, OP_SETFIELD, prev_arg);
            } else compile_error(C, "invalid assignment target");
        } else if (C->lex.cur.type == TOK_COMMA) {
            emit(C, OP_POP, 0);
        } else {
            emit(C, OP_POP, 0);
        }
        break;
    }
    }
}

/* ---- VM ------------------------------------------------------------- */

typedef struct {
    lua_value globals[LUA_MAX_GLOBALS];
    char global_names[LUA_MAX_GLOBALS][64];
    int nglobals;
    lua_value stack[LUA_MAX_STACK];
    int sp;
    lua_proto *protos;
    int nprotos;
} lua_vm;

static int vm_global_slot(lua_vm *vm, const char *name) {
    for (int i = 0; i < vm->nglobals; i++)
        if (strcmp(vm->global_names[i], name) == 0) return i;
    if (vm->nglobals >= LUA_MAX_GLOBALS) return -1;
    int s = vm->nglobals++;
    strncpy(vm->global_names[s], name, 63);
    vm->globals[s] = lua_nil();
    return s;
}

static void vm_push(lua_vm *vm, lua_value v) {
    if (vm->sp < LUA_MAX_STACK) vm->stack[vm->sp++] = v;
}

static lua_value vm_pop(lua_vm *vm) {
    if (vm->sp > 0) return vm->stack[--vm->sp];
    return lua_nil();
}

static void val_tostring(lua_value v, char *buf, int cap) {
    switch (v.type) {
    case LUA_TNIL:   snprintf(buf, cap, "nil"); break;
    case LUA_TBOOL:  snprintf(buf, cap, v.u.bval ? "true" : "false"); break;
    case LUA_TNUM: {
        double d = v.u.nval;
        if (d == (double)(long)d) snprintf(buf, cap, "%ld", (long)d);
        else snprintf(buf, cap, "%.14g", d);
        break;
    }
    case LUA_TSTR:   snprintf(buf, cap, "%s", v.u.sval); break;
    case LUA_TTABLE: snprintf(buf, cap, "table: %p", (void*)v.u.tval); break;
    case LUA_TFUNC:  snprintf(buf, cap, "function: %d", v.u.fval); break;
    case LUA_TCFUNC: snprintf(buf, cap, "function: (C)"); break;
    }
}

static double lua_pow(double base, double exp) {
    if (exp == 0.0) return 1.0;
    if (exp == 1.0) return base;
    if (exp == 2.0) return base * base;
    double result = 1.0;
    int neg = 0;
    if (exp < 0) { neg = 1; exp = -exp; }
    int iexp = (int)exp;
    double frac = exp - iexp;
    for (int i = 0; i < iexp; i++) result *= base;
    if (frac > 0.001) {
        double approx = 1.0;
        double term = 1.0;
        double ln_base = 0.0;
        if (base > 0) {
            double x = (base - 1.0) / (base + 1.0);
            double x2 = x * x;
            ln_base = 2.0 * x;
            double xn = x * x2;
            for (int i = 3; i < 20; i += 2) {
                ln_base += 2.0 * xn / i;
                xn *= x2;
            }
        }
        double y = frac * ln_base;
        for (int i = 1; i < 20; i++) {
            term *= y / i;
            approx += term;
        }
        result *= approx;
    }
    if (neg) result = 1.0 / result;
    return result;
}

static double lua_sqrt(double x) {
    if (x <= 0.0) return 0.0;
    double g = x * 0.5;
    for (int i = 0; i < 50; i++) g = 0.5 * (g + x / g);
    return g;
}

static double lua_fabs(double x) { return x < 0 ? -x : x; }

static double lua_sin(double x) {
    while (x > 3.14159265358979323846) x -= 6.28318530717958647692;
    while (x < -3.14159265358979323846) x += 6.28318530717958647692;
    double term = x, sum = x;
    for (int i = 1; i < 15; i++) {
        term *= -x * x / ((2*i) * (2*i + 1));
        sum += term;
    }
    return sum;
}

static double lua_cos(double x) {
    return lua_sin(x + 1.5707963267948966);
}

typedef struct {
    int ret_proto;
    int ret_ip;
    int ret_sp;
    int base;
} callframe;

static int vm_exec(lua_vm *vm, int proto_idx) {
    callframe frames[LUA_MAX_CALLSTACK];
    int frame_top = 0;
    lua_proto *p = &vm->protos[proto_idx];
    int ip = 0;
    int base = vm->sp;

    for (int i = 0; i < p->nlocals; i++) vm_push(vm, lua_nil());

    for (;;) {
        if (ip >= p->ncode) break;
        instruction ins = p->code[ip++];
        switch (ins.op) {
        case OP_NOP: break;
        case OP_CONST: vm_push(vm, p->consts[ins.arg]); break;
        case OP_NIL:   vm_push(vm, lua_nil()); break;
        case OP_TRUE:  vm_push(vm, lua_bool(1)); break;
        case OP_FALSE: vm_push(vm, lua_bool(0)); break;
        case OP_POP:   vm_pop(vm); break;
        case OP_DUP:   if (vm->sp > 0) vm_push(vm, vm->stack[vm->sp-1]); break;
        case OP_GETLOCAL:
            vm_push(vm, vm->stack[base + ins.arg]);
            break;
        case OP_SETLOCAL:
            vm->stack[base + ins.arg] = vm_pop(vm);
            break;
        case OP_GETGLOBAL: {
            const char *name = p->consts[ins.arg].u.sval;
            int s = vm_global_slot(vm, name);
            vm_push(vm, s >= 0 ? vm->globals[s] : lua_nil());
            break;
        }
        case OP_SETGLOBAL: {
            const char *name = p->consts[ins.arg].u.sval;
            int s = vm_global_slot(vm, name);
            if (s >= 0) vm->globals[s] = vm_pop(vm);
            break;
        }
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV:
        case OP_MOD: case OP_POW: {
            lua_value b = vm_pop(vm), a = vm_pop(vm);
            double na = a.type == LUA_TNUM ? a.u.nval : 0;
            double nb = b.type == LUA_TNUM ? b.u.nval : 0;
            double r = 0;
            switch (ins.op) {
            case OP_ADD: r = na + nb; break;
            case OP_SUB: r = na - nb; break;
            case OP_MUL: r = na * nb; break;
            case OP_DIV: r = nb != 0 ? na / nb : 0; break;
            case OP_MOD: r = nb != 0 ? na - (double)(long)(na/nb) * nb : 0; break;
            case OP_POW: r = lua_pow(na, nb); break;
            }
            vm_push(vm, lua_num(r));
            break;
        }
        case OP_NEG: {
            lua_value a = vm_pop(vm);
            vm_push(vm, lua_num(-(a.type == LUA_TNUM ? a.u.nval : 0)));
            break;
        }
        case OP_CONCAT: {
            lua_value b = vm_pop(vm), a = vm_pop(vm);
            char sa[256], sb[256];
            val_tostring(a, sa, sizeof(sa));
            val_tostring(b, sb, sizeof(sb));
            char buf[512];
            snprintf(buf, sizeof(buf), "%s%s", sa, sb);
            vm_push(vm, lua_str(buf));
            break;
        }
        case OP_LEN: {
            lua_value a = vm_pop(vm);
            if (a.type == LUA_TSTR) vm_push(vm, lua_num(strlen(a.u.sval)));
            else if (a.type == LUA_TTABLE) vm_push(vm, lua_num(a.u.tval->array_len));
            else vm_push(vm, lua_num(0));
            break;
        }
        case OP_EQ: case OP_NE: case OP_LT: case OP_LE:
        case OP_GT: case OP_GE: {
            lua_value b = vm_pop(vm), a = vm_pop(vm);
            int result = 0;
            if (a.type == LUA_TNUM && b.type == LUA_TNUM) {
                switch (ins.op) {
                case OP_EQ: result = a.u.nval == b.u.nval; break;
                case OP_NE: result = a.u.nval != b.u.nval; break;
                case OP_LT: result = a.u.nval < b.u.nval; break;
                case OP_LE: result = a.u.nval <= b.u.nval; break;
                case OP_GT: result = a.u.nval > b.u.nval; break;
                case OP_GE: result = a.u.nval >= b.u.nval; break;
                }
            } else if (a.type == LUA_TSTR && b.type == LUA_TSTR) {
                int cmp = strcmp(a.u.sval, b.u.sval);
                switch (ins.op) {
                case OP_EQ: result = cmp == 0; break;
                case OP_NE: result = cmp != 0; break;
                case OP_LT: result = cmp < 0; break;
                case OP_LE: result = cmp <= 0; break;
                case OP_GT: result = cmp > 0; break;
                case OP_GE: result = cmp >= 0; break;
                }
            } else {
                result = (ins.op == OP_EQ) ? (a.type == b.type && a.type == LUA_TNIL) :
                         (ins.op == OP_NE) ? !(a.type == b.type && a.type == LUA_TNIL) : 0;
            }
            vm_push(vm, lua_bool(result));
            break;
        }
        case OP_NOT: {
            lua_value a = vm_pop(vm);
            vm_push(vm, lua_bool(!lua_truthy(a)));
            break;
        }
        case OP_JMP:  ip = ins.arg; break;
        case OP_JMPF:
            if (!lua_truthy(vm->stack[vm->sp - 1])) ip = ins.arg;
            break;
        case OP_JMPT:
            if (lua_truthy(vm->stack[vm->sp - 1])) ip = ins.arg;
            break;
        case OP_CALL: {
            int nargs = ins.arg;
            lua_value *args = &vm->stack[vm->sp - nargs];
            lua_value func = vm->stack[vm->sp - nargs - 1];

            if (func.type == LUA_TCFUNC) {
                /* C function call -- arguments are on the stack. The cfunc
                 * reads them relative to the current sp and pushes a result. */
                int old_sp = vm->sp - nargs - 1;
                int (*cfn)(void) = func.u.cfunc;
                /* Pass nargs via a global-ish trick: store in vm temporarily */
                int cfn_nargs = nargs;
                lua_value cfn_args[16];
                for (int i = 0; i < nargs && i < 16; i++)
                    cfn_args[i] = args[i];
                vm->sp = old_sp;

                lua_value result = lua_nil();
                result = ((lua_value (*)(lua_vm*, lua_value*, int))(void*)cfn)(vm, cfn_args, cfn_nargs);
                vm_push(vm, result);
            } else if (func.type == LUA_TFUNC) {
                int fn_idx = func.u.fval;
                if (frame_top >= LUA_MAX_CALLSTACK) {
                    fprintf(stderr, "lua: call stack overflow\n"); return 1;
                }
                frames[frame_top].ret_proto = (int)(p - vm->protos);
                frames[frame_top].ret_ip = ip;
                frames[frame_top].ret_sp = vm->sp - nargs - 1;
                frames[frame_top].base = base;
                frame_top++;

                p = &vm->protos[fn_idx];
                ip = 0;
                base = vm->sp - nargs;
                /* Pad missing args with nil */
                while (nargs < p->nparams) {
                    vm_push(vm, lua_nil());
                    nargs++;
                }
                /* Allocate remaining locals */
                for (int i = p->nparams; i < p->nlocals; i++)
                    vm_push(vm, lua_nil());
            } else {
                fprintf(stderr, "lua: attempt to call a %s value\n",
                    func.type == LUA_TNIL ? "nil" : "non-function");
                for (int i = 0; i < nargs + 1; i++) vm_pop(vm);
                vm_push(vm, lua_nil());
            }
            break;
        }
        case OP_RETURN: {
            lua_value retval = vm_pop(vm);
            if (frame_top == 0) {
                vm->sp = base;
                vm_push(vm, retval);
                return 0;
            }
            frame_top--;
            vm->sp = frames[frame_top].ret_sp;
            base = frames[frame_top].base;
            ip = frames[frame_top].ret_ip;
            p = &vm->protos[frames[frame_top].ret_proto];
            vm_push(vm, retval);
            break;
        }
        case OP_NEWTABLE: {
            lua_value tv;
            tv.type = LUA_TTABLE;
            tv.u.tval = table_new();
            vm_push(vm, tv);
            break;
        }
        case OP_GETFIELD: {
            lua_value obj = vm_pop(vm);
            if (obj.type == LUA_TTABLE) {
                vm_push(vm, table_get(obj.u.tval, p->consts[ins.arg]));
            } else {
                vm_push(vm, lua_nil());
            }
            break;
        }
        case OP_SETFIELD: {
            lua_value val = vm_pop(vm);
            lua_value key = p->consts[ins.arg];
            lua_value obj = vm_pop(vm);
            if (obj.type == LUA_TTABLE) table_set(obj.u.tval, key, val);
            break;
        }
        case OP_GETTABLE: {
            lua_value key = vm_pop(vm);
            lua_value obj = vm_pop(vm);
            if (obj.type == LUA_TTABLE) vm_push(vm, table_get(obj.u.tval, key));
            else vm_push(vm, lua_nil());
            break;
        }
        case OP_SETTABLE: {
            lua_value val = vm_pop(vm);
            lua_value key = vm_pop(vm);
            lua_value obj = vm_pop(vm);
            if (obj.type == LUA_TTABLE) table_set(obj.u.tval, key, val);
            break;
        }
        case OP_CLOSURE: {
            lua_value fv;
            fv.type = LUA_TFUNC;
            fv.u.fval = ins.arg;
            vm_push(vm, fv);
            break;
        }
        default:
            fprintf(stderr, "lua: unknown opcode %d\n", ins.op);
            return 1;
        }
    }
    return 0;
}

/* ---- Standard library C functions ----------------------------------- */

static lua_value cfn_print(lua_vm *vm, lua_value *args, int nargs) {
    (void)vm;
    for (int i = 0; i < nargs; i++) {
        if (i > 0) putchar('\t');
        char buf[512];
        val_tostring(args[i], buf, sizeof(buf));
        fputs(buf, stdout);
    }
    putchar('\n');
    return lua_nil();
}

static lua_value cfn_type(lua_vm *vm, lua_value *args, int nargs) {
    (void)vm;
    if (nargs < 1) return lua_str("nil");
    const char *t = "nil";
    switch (args[0].type) {
    case LUA_TNIL:   t = "nil"; break;
    case LUA_TBOOL:  t = "boolean"; break;
    case LUA_TNUM:   t = "number"; break;
    case LUA_TSTR:   t = "string"; break;
    case LUA_TTABLE: t = "table"; break;
    case LUA_TFUNC: case LUA_TCFUNC: t = "function"; break;
    }
    return lua_str(t);
}

static lua_value cfn_tostring(lua_vm *vm, lua_value *args, int nargs) {
    (void)vm;
    if (nargs < 1) return lua_str("nil");
    char buf[512];
    val_tostring(args[0], buf, sizeof(buf));
    return lua_str(buf);
}

static lua_value cfn_tonumber(lua_vm *vm, lua_value *args, int nargs) {
    (void)vm;
    if (nargs < 1) return lua_nil();
    if (args[0].type == LUA_TNUM) return args[0];
    if (args[0].type == LUA_TSTR) {
        char *end;
        double d = strtod(args[0].u.sval, &end);
        if (end != args[0].u.sval) return lua_num(d);
    }
    return lua_nil();
}

static lua_value cfn_string_len(lua_vm *vm, lua_value *args, int nargs) {
    (void)vm;
    if (nargs < 1 || args[0].type != LUA_TSTR) return lua_num(0);
    return lua_num(strlen(args[0].u.sval));
}

static lua_value cfn_string_sub(lua_vm *vm, lua_value *args, int nargs) {
    (void)vm;
    if (nargs < 2 || args[0].type != LUA_TSTR) return lua_str("");
    const char *s = args[0].u.sval;
    int len = (int)strlen(s);
    int i = (int)args[1].u.nval;
    int j = nargs >= 3 ? (int)args[2].u.nval : len;
    if (i < 0) i = len + i + 1;
    if (j < 0) j = len + j + 1;
    if (i < 1) i = 1;
    if (j > len) j = len;
    if (i > j) return lua_str("");
    char buf[512];
    int n = j - i + 1;
    if (n > 511) n = 511;
    memcpy(buf, s + i - 1, n);
    buf[n] = '\0';
    return lua_str(buf);
}

static lua_value cfn_table_insert(lua_vm *vm, lua_value *args, int nargs) {
    (void)vm;
    if (nargs < 2 || args[0].type != LUA_TTABLE) return lua_nil();
    lua_table *t = args[0].u.tval;
    int pos = t->array_len + 1;
    table_set(t, lua_num(pos), args[1]);
    return lua_nil();
}

static lua_value cfn_math_sqrt(lua_vm *vm, lua_value *args, int nargs) {
    (void)vm;
    if (nargs < 1 || args[0].type != LUA_TNUM) return lua_num(0);
    return lua_num(lua_sqrt(args[0].u.nval));
}

static lua_value cfn_math_sin(lua_vm *vm, lua_value *args, int nargs) {
    (void)vm;
    if (nargs < 1 || args[0].type != LUA_TNUM) return lua_num(0);
    return lua_num(lua_sin(args[0].u.nval));
}

static lua_value cfn_math_cos(lua_vm *vm, lua_value *args, int nargs) {
    (void)vm;
    if (nargs < 1 || args[0].type != LUA_TNUM) return lua_num(0);
    return lua_num(lua_cos(args[0].u.nval));
}

static lua_value cfn_math_abs(lua_vm *vm, lua_value *args, int nargs) {
    (void)vm;
    if (nargs < 1 || args[0].type != LUA_TNUM) return lua_num(0);
    return lua_num(lua_fabs(args[0].u.nval));
}

static lua_value cfn_math_floor(lua_vm *vm, lua_value *args, int nargs) {
    (void)vm;
    if (nargs < 1 || args[0].type != LUA_TNUM) return lua_num(0);
    return lua_num((double)(long)args[0].u.nval);
}

static lua_value cfn_ipairs_iter(lua_vm *vm, lua_value *args, int nargs) {
    (void)vm; (void)args; (void)nargs;
    return lua_nil();
}

static void register_cfunc(lua_vm *vm, const char *name,
                           lua_value (*fn)(lua_vm*, lua_value*, int)) {
    int s = vm_global_slot(vm, name);
    if (s >= 0) {
        vm->globals[s].type = LUA_TCFUNC;
        vm->globals[s].u.cfunc = (int(*)(void))(void*)fn;
    }
}

static void register_stdlib(lua_vm *vm) {
    register_cfunc(vm, "print", cfn_print);
    register_cfunc(vm, "type", cfn_type);
    register_cfunc(vm, "tostring", cfn_tostring);
    register_cfunc(vm, "tonumber", cfn_tonumber);

    /* string library as a table */
    lua_table *str_lib = table_new();
    lua_value sv; sv.type = LUA_TTABLE; sv.u.tval = str_lib;
    int s = vm_global_slot(vm, "string");
    if (s >= 0) vm->globals[s] = sv;

    lua_value fv;
    fv.type = LUA_TCFUNC;
    fv.u.cfunc = (int(*)(void))(void*)cfn_string_len;
    table_set(str_lib, lua_str("len"), fv);
    fv.u.cfunc = (int(*)(void))(void*)cfn_string_sub;
    table_set(str_lib, lua_str("sub"), fv);

    /* table library */
    lua_table *tbl_lib = table_new();
    lua_value tv; tv.type = LUA_TTABLE; tv.u.tval = tbl_lib;
    s = vm_global_slot(vm, "table");
    if (s >= 0) vm->globals[s] = tv;
    fv.u.cfunc = (int(*)(void))(void*)cfn_table_insert;
    table_set(tbl_lib, lua_str("insert"), fv);

    /* math library */
    lua_table *math_lib = table_new();
    lua_value mv; mv.type = LUA_TTABLE; mv.u.tval = math_lib;
    s = vm_global_slot(vm, "math");
    if (s >= 0) vm->globals[s] = mv;

    fv.u.cfunc = (int(*)(void))(void*)cfn_math_sqrt;
    table_set(math_lib, lua_str("sqrt"), fv);
    fv.u.cfunc = (int(*)(void))(void*)cfn_math_sin;
    table_set(math_lib, lua_str("sin"), fv);
    fv.u.cfunc = (int(*)(void))(void*)cfn_math_cos;
    table_set(math_lib, lua_str("cos"), fv);
    fv.u.cfunc = (int(*)(void))(void*)cfn_math_abs;
    table_set(math_lib, lua_str("abs"), fv);
    fv.u.cfunc = (int(*)(void))(void*)cfn_math_floor;
    table_set(math_lib, lua_str("floor"), fv);
    table_set(math_lib, lua_str("pi"), lua_num(3.14159265358979323846));
    table_set(math_lib, lua_str("huge"), lua_num(1e308));
}

/* ---- REPL & file execution ------------------------------------------ */

static int run_source(const char *src, lua_vm *vm) {
    compiler C;
    memset(&C, 0, sizeof(C));
    C.lex.src = src;
    C.lex.pos = 0;
    C.lex.line = 1;
    C.nprotos = 1;
    C.current_proto = 0;
    memset(&C.protos[0], 0, sizeof(lua_proto));
    strncpy(C.protos[0].name, "main", 63);

    lex_next(&C.lex);
    parse_block(&C);
    if (C.had_error) return 1;
    emit(&C, OP_NIL, 0);
    emit(&C, OP_RETURN, 0);

    for (int i = 0; i < C.nprotos; i++)
        vm->protos[vm->nprotos + i] = C.protos[i];

    int main_idx = vm->nprotos;
    vm->nprotos += C.nprotos;

    return vm_exec(vm, main_idx);
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

int main(int argc, char **argv) {
    lua_vm vm;
    memset(&vm, 0, sizeof(vm));
    register_stdlib(&vm);

    if (argc >= 2) {
        char *src = read_file(argv[1]);
        if (!src) {
            fprintf(stderr, "lua: cannot open %s\n", argv[1]);
            return 1;
        }
        int rc = run_source(src, &vm);
        free(src);
        return rc;
    }

    /* REPL mode */
    printf("Lua 5.4 (tobyOS minimal)\n");
    char line[LUA_MAX_LINE];
    while (1) {
        printf("> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[--len] = '\0';
        if (len == 0) continue;

        int old_sp = vm.sp;
        if (run_source(line, &vm) == 0 && vm.sp > old_sp) {
            lua_value result = vm_pop(&vm);
            if (result.type != LUA_TNIL) {
                char buf[512];
                val_tostring(result, buf, sizeof(buf));
                printf("%s\n", buf);
            }
        }
        vm.sp = 0;
    }
    return 0;
}
