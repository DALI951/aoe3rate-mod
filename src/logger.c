/* logger.c — d3d9mod.log handling + debug logging (R14) */
#include "state.h"

/* Called from DllMain; currently a no-op (dlog auto-creates the log file on
 * first use). Kept as an explicit init hook per the modular plan. */
void logger_init(void) {
    /* nothing to do — dlog opens/closes d3d9mod.log lazily */
}

/* Debug diagnostics — emitted only when [Debug] Enabled=1. */
void logger_debug(const char *fmt, ...) {
    if (!g_settings.debug_enabled) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    dlog("DBG %s", buf);
}
