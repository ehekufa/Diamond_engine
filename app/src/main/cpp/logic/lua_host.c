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
        }
        if (error) luac_free(error);
        return ok ? 1 : 0;
    }
#else
    (void)length;
    diamond_fail(DIAMOND_ERR_UNSUPPORTED, err, err_cap,
                 "LuaC not compiled in (built-in executor active)");
    return 0;
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
