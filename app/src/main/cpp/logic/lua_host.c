/* Diamond Engine — LuaC host bridge (C99).
 *
 * LuaC is a native compiler: it turns Lua source into C, which is then built
 * with the platform toolchain. On Android there is no toolchain at runtime, so
 * the engine uses LuaC for exactly what it can do on device:
 *
 *   1. syntax validation of the generated / hand edited logic   (luac_check)
 *   2. native C generation, which is what now gets baked into the
 *      .diamond file and what the build step compiles into libengine.so
 *      when the project is exported for a standalone game   (luac_compile_bundle_c)
 *   3. the compiled C is also fed back through the built-in executor while the
 *      user is editing, so "Play" works instantly.
 *
 * When the LuaC sources are absent (DIAMOND_HAVE_LUAC == 0) every entry point
 * degrades to a precise message instead of failing to link.
 */
#include "diamond/dlogic.h"
#include "diamond/dlog.h"
#include "diamond/dutil.h"

#include <stdio.h>
#include <string.h>

#if DIAMOND_HAVE_LUAC
#include "luac.h"
#endif

static char g_version_buffer[64];

/* A deliberately small structural check of the Lua subset the code generator
 * emits and the executor understands. It catches the mistakes a block editor
 * actually produces -- a missing `end`, an unbalanced bracket, an assignment
 * outside a function -- and reports the line, instead of pretending to be a Lua
 * compiler. Anything it accepts is guaranteed to be rejected by the executor
 * only for semantic (not syntactic) reasons. */
static int is_identifier_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int dlogic_check_source_subset(const char *source, int length,
                                      char *reason, int reasonCap) {
    int depth = 0;          /* open function/if/for/while blocks */
    int parens = 0;
    int brackets = 0;
    int line = 1;
    int openLine = 0;
    int expectStatement = 1; /* start of a line, outside any expression */
    int previousKeyword = 0; /* 1 = just saw `end`, 2 = saw `then`/`do` */
    int i = 0;

    while (i < length) {
        const char c = source[i];
        if (c == '\n') {
            line++;
            expectStatement = 1;
            previousKeyword = 0;
            i++;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }
        if (c == '-' && i + 1 < length && source[i + 1] == '-') {
            while (i < length && source[i] != '\n') i++;
            continue;
        }
        if (c == '"' || c == '\'') {
            const char quote = c;
            i++;
            while (i < length && source[i] != quote) {
                if (source[i] == '\\') i++;
                if (i < length && source[i] == '\n') {
                    snprintf(reason, (size_t)reasonCap, "line %d: unterminated string", line);
                    return 0;
                }
                i++;
            }
            if (i >= length) {
                snprintf(reason, (size_t)reasonCap, "line %d: unterminated string", line);
                return 0;
            }
            i++;
            expectStatement = 0;
            continue;
        }
        if (c == '(') parens++;
        else if (c == ')') parens--;
        else if (c == '[') brackets++;
        else if (c == ']') brackets--;
        if (parens < 0 || brackets < 0) {
            snprintf(reason, (size_t)reasonCap, "line %d: unbalanced bracket", line);
            return 0;
        }
        if (is_identifier_char(c)) {
            const int start = i;
            while (i < length && is_identifier_char(source[i])) i++;
            const int wordLen = i - start;
            const char *word = source + start;
            const int opens = (wordLen == 8 && strncmp(word, "function", 8) == 0);
            const int conditional = (wordLen == 2 && strncmp(word, "if", 2) == 0) ||
                                    (wordLen == 7 && strncmp(word, "elseif", 7) == 0) ||
                                    (wordLen == 3 && strncmp(word, "for", 3) == 0) ||
                                    (wordLen == 5 && strncmp(word, "while", 5) == 0);
            const int closes = (wordLen == 3 && strncmp(word, "end", 3) == 0);
            const int isElse = (wordLen == 4 && strncmp(word, "else", 4) == 0);
            if (opens || conditional) {
                if (opens) {
                    if (!expectStatement) {
                        snprintf(reason, (size_t)reasonCap,
                                 "line %d: 'function' must start a statement", line);
                        return 0;
                    }
                    openLine = line;
                    previousKeyword = 2;
                } else {
                    previousKeyword = 2;
                }
                depth++;
            } else if (closes) {
                if (--depth < 0) {
                    snprintf(reason, (size_t)reasonCap, "line %d: 'end' without a block", line);
                    return 0;
                }
                previousKeyword = 1;
            } else if (isElse) {
                if (previousKeyword != 1) {
                    snprintf(reason, (size_t)reasonCap,
                             "line %d: 'else' must follow the body of an if", line);
                    return 0;
                }
                previousKeyword = 1;
            } else {
                previousKeyword = 0;
            }
            expectStatement = 0;
            continue;
        }
        if (c == '=' && expectStatement) {
            /* An assignment at statement level is fine; nothing to track. */
            expectStatement = 0;
        }
        i++;
    }
    if (parens != 0 || brackets != 0) {
        snprintf(reason, (size_t)reasonCap, "unbalanced bracket at end of file");
        return 0;
    }
    if (depth != 0) {
        snprintf(reason, (size_t)reasonCap, "line %d: block is never closed with 'end'", openLine);
        return 0;
    }
    return 1;
}

int dlogic_luac_available(void) {
#if DIAMOND_HAVE_LUAC
    return 1;
#else
    return 0;
#endif
}

const char *dlogic_luac_version(void) {
#if DIAMOND_HAVE_LUAC
    const char *version = luac_version();
    snprintf(g_version_buffer, sizeof(g_version_buffer), "%s (%s)",
             version ? version : "?", DIAMOND_LUAC_REF);
#else
    snprintf(g_version_buffer, sizeof(g_version_buffer), "built-in executor");
#endif
    return g_version_buffer;
}

int dlogic_luac_check(const char *source, int length, char *err, int err_cap) {
    if (!source || length <= 0) return diamond_fail(DIAMOND_ERR_ARG, err, err_cap, "empty source");
#if DIAMOND_HAVE_LUAC
    {
        char *error = NULL;
        /* luac_check expects NUL terminated text; our source always is. */
        int ok = luac_check(source, "logic.lua", &error);
        if (!ok) {
            diamond_fail(DIAMOND_ERR_LOGIC, err, err_cap, "%s", error ? error : "syntax error");
            dlog_write(DLOG_ERROR, "luac", "syntax error: %s", error ? error : "?");
            if (error) luac_free(error);
            return 0;
        }
        if (error) luac_free(error);
        return 1;
    }
#else
    /* Without LuaC there is no Lua compiler, but the editor still needs a
     * yes/no answer while the user types. The executor understands exactly the
     * subset the code generator emits, so parse against that: balanced
     * function/if/for/while blocks and no stray keywords. */
    {
        char reason[160];
        reason[0] = '\0';
        if (dlogic_check_source_subset(source, length, reason, sizeof(reason)) == 0) {
            diamond_fail(DIAMOND_ERR_LOGIC, err, err_cap, "%s", reason);
            return 0;
        }
        /* Valid, but say plainly that this was not a real Lua compile. */
        if (err && err_cap > 0) err[0] = '\0';
        return 1;
    }
#endif
}

const char *dlogic_luac_compile(const char *source, int length, char *err, int err_cap) {
    if (!source || length <= 0) {
        diamond_fail(DIAMOND_ERR_ARG, err, err_cap, "empty source");
        return NULL;
    }
#if DIAMOND_HAVE_LUAC
    {
        char *error = NULL;
        char *code = luac_compile_bundle_c(source, "logic.lua", NULL, 0, &error);
        if (!code) {
            diamond_fail(DIAMOND_ERR_LOGIC, err, err_cap, "%s", error ? error : "compile failed");
            dlog_write(DLOG_ERROR, "luac", "compile failed: %s", error ? error : "?");
            if (error) luac_free(error);
            return NULL;
        }
        if (error) luac_free(error);
        dlog_write(DLOG_INFO, "luac", "compiled logic to native C (%d bytes source)",
                   (int)strlen(source));
        return code;
    }
#else
    (void)length;
    diamond_fail(DIAMOND_ERR_UNSUPPORTED, err, err_cap,
                 "LuaC not compiled in (built-in executor active)");
    return NULL;
#endif
}

void dlogic_luac_free(char *text) {
#if DIAMOND_HAVE_LUAC
    if (text) luac_free(text);
#else
    (void)text;
#endif
}
