/* settings.c — ResourceRateMod.ini parse/write (atomic) + DefaultProfile XML poll (R14)
 *
 * Robust parser: missing/partial/garbage file => defaults. Unknown keys ignored.
 * Atomic write: write temp file then MoveFileExA(REPLACE_EXISTING).
 * Also polls the active Users\DefaultProfile*.xml for any mod <Setting> keys
 * (mtime + parse, cheap) and merges them over the INI defaults.
 */
#include "state.h"

static char g_ini_path[MAX_PATH];        /* ResourceRateMod.ini next to the DLL */
static char g_profile_path[MAX_PATH];    /* active Users\DefaultProfile*.xml */
static __int64 g_profile_mtime = 0;
static int s_clock_noted = 0;            /* one-time "realtime clock" note (R16 A1) */
static DWORD s_poll_last = 0;            /* 1s throttle for settings_poll_profile */
static int   s_poll_have = 0;

static void settings_defaults(void) {
    ModSettings *s = &g_settings;
    memset(s, 0, sizeof(*s));
    s->enabled             = 1;
    s->hotkey              = 0x78;          /* F9 */
    s->start_hidden        = 0;
    lstrcpyA(s->font_name, "");
    s->font_size           = 14;
    s->opacity             = 0.85f;
    s->pos_x               = 12;
    s->pos_y               = 12;
    s->show_header         = 1;
    s->show_slots_567      = 0;
    s->sample_ms           = (int)SAMPLE_MS_DEFAULT;
    lstrcpyA(s->smoothing, "med");
    s->use_unit_min        = 1;
    s->use_game_time       = 1;
    s->discontinuity_ratio = 3.0f;
    s->show_gains          = 1;
    s->debug_enabled       = 0;
}

static int ini_get_bool(const char *v, int dflt) {
    if (v == NULL) return dflt;
    if (!strcmp(v, "1")) return 1;
    if (!strcmp(v, "true")) return 1;
    if (!strcmp(v, "on")) return 1;
    if (!strcmp(v, "yes")) return 1;
    if (!strcmp(v, "0")) return 0;
    if (!strcmp(v, "false")) return 0;
    if (!strcmp(v, "off")) return 0;
    if (!strcmp(v, "no")) return 0;
    return dflt;
}

static int ini_get_int(const char *v, int dflt) {
    if (v == NULL) return dflt;
    if (v[0] == '\0') return dflt;
    int n = (int)strtol(v, NULL, 0);
    if (n == 0 && v[0] != '0') return dflt;
    return n;
}

static float ini_get_float(const char *v, float dflt) {
    if (v == NULL) return dflt;
    if (v[0] == '\0') return dflt;
    char *end = NULL;
    float f = (float)strtod(v, &end);
    if (end == v) return dflt;
    return f;
}

/* resolve module dir + path building */
static const char *module_dir(const char *filename, char *out, size_t outsz) {
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
    if (n == 0) {
        _snprintf(out, outsz, "%s", filename);
        return out;
    }
    char *slash = strrchr(exe, '\\');
    if (slash != NULL) slash[1] = '\0';
    else exe[0] = '\0';
    _snprintf(out, outsz, "%s%s", exe, filename);
    return out;
}

void settings_init(void) {
    module_dir("ResourceRateMod.ini", g_ini_path, sizeof(g_ini_path));
    module_dir("Users\\DefaultProfile.xml", g_profile_path, sizeof(g_profile_path));
    settings_defaults();
}

static void profile_path_find(void) {
    /* active profile: newest Users\DefaultProfile*.xml under the exe dir */
    char users_dir[MAX_PATH];
    module_dir("Users", users_dir, sizeof(users_dir));
    g_profile_path[0] = '\0';
    WIN32_FIND_DATAA fd;
    char pat[MAX_PATH];
    _snprintf(pat, sizeof(pat), "%s\\DefaultProfile*.xml", users_dir);
    HANDLE hf = FindFirstFileA(pat, &fd);
    if (hf == INVALID_HANDLE_VALUE) return;
    __int64 best = -1;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        __int64 ft = ((__int64)fd.ftLastWriteTime.dwHighDateTime << 32)
                   | (DWORD)fd.ftLastWriteTime.dwLowDateTime;
        if (ft > best) {
            best = ft;
            _snprintf(g_profile_path, sizeof(g_profile_path), "%s\\%s",
                      users_dir, fd.cFileName);
        }
    } while (FindNextFileA(hf, &fd));
    FindClose(hf);
}

static void profile_apply(void) {
    /* mtime check => cheap; re-parse only when changed */
    WIN32_FIND_DATAA fd;
    HANDLE hf = FindFirstFileA(g_profile_path, &fd);
    if (hf == INVALID_HANDLE_VALUE) return;
    FindClose(hf);
    __int64 mt = ((__int64)fd.ftLastWriteTime.dwHighDateTime << 32)
               | (DWORD)fd.ftLastWriteTime.dwLowDateTime;
    if (mt == g_profile_mtime) return;
    g_profile_mtime = mt;

    FILE *f = fopen(g_profile_path, "rb");
    if (f == NULL) return;
    size_t cap = 8192, len = 0;
    char *buf = (char *)malloc(cap);
    if (buf == NULL) { fclose(f); return; }
    size_t got;
    while ((got = fread(buf + len, 1, cap - len - 1, f)) > 0) {
        len += got;
        if (len >= cap - 1) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (nb == NULL) break;
            buf = nb;
        }
    }
    fclose(f);
    buf[len] = '\0';

    /* find <Setting Name="ResourceRateMod.">value</Setting> */
    const char *p = buf;
    char name[160];
    char value[64];
    while ((p = strstr(p, "<Setting")) != NULL) {
        const char *an = strstr(p, "Name=");
        if (an == NULL || an > p + 256) { p += 9; continue; }
        an += 5;
        if (*an == '\"') an++;
        const char *end = strchr(an, '\"');
        int name_len;
        if (end == NULL) { p += 9; continue; }
        name_len = (int)(end - an);
        if (name_len <= 0 || name_len >= (int)sizeof(name)) { p += 9; continue; }
        memcpy(name, an, (size_t)name_len);
        name[name_len] = '\0';
        if (strncmp(name, "ResourceRateMod.", 16) != 0) {
            p = end + 1;
            continue;
        }
        const char *avl = strstr(end, ">");
        if (avl == NULL) { p = end + 1; continue; }
        avl++;
        const char *vle = strchr(avl, '<');
        if (vle == NULL) { p = end + 1; continue; }
        int vlen = (int)(vle - avl);
        if (vlen <= 0 || vlen >= (int)sizeof(value)) { p = end + 1; continue; }
        memcpy(value, avl, (size_t)vlen);
        value[vlen] = '\0';
        p = vle + 1;

        const char *key = name + 16;
        ModSettings *s = &g_settings;
        if (!strcmp(key, "Enabled"))            s->enabled = ini_get_bool(value, s->enabled);
        else if (!strcmp(key, "Hotkey"))        s->hotkey = ini_get_int(value, s->hotkey);
        else if (!strcmp(key, "StartHidden"))   s->start_hidden = ini_get_bool(value, s->start_hidden);
        else if (!strcmp(key, "FontSize"))      s->font_size = ini_get_int(value, s->font_size);
        else if (!strcmp(key, "Opacity"))       s->opacity = ini_get_float(value, s->opacity);
        else if (!strcmp(key, "PosX"))          s->pos_x = ini_get_int(value, s->pos_x);
        else if (!strcmp(key, "PosY"))          s->pos_y = ini_get_int(value, s->pos_y);
        else if (!strcmp(key, "ShowHeader"))    s->show_header = ini_get_bool(value, s->show_header);
        else if (!strcmp(key, "ShowSlots567"))  s->show_slots_567 = ini_get_bool(value, s->show_slots_567);
        else if (!strcmp(key, "SampleMs"))      s->sample_ms = ini_get_int(value, s->sample_ms);
        else if (!strcmp(key, "Smoothing"))     { lstrcpynA(s->smoothing, value, sizeof(s->smoothing)); }
        else if (!strcmp(key, "Unit"))          s->use_unit_min = (!strcmp(value, "min"));
        else if (!strcmp(key, "UseGameTime"))   s->use_game_time = ini_get_bool(value, s->use_game_time);
        else if (!strcmp(key, "DiscontinuityRatio")) s->discontinuity_ratio = ini_get_float(value, s->discontinuity_ratio);
        else if (!strcmp(key, "ShowGains"))     s->show_gains = ini_get_bool(value, s->show_gains);
        else if (!strcmp(key, "DebugEnabled"))  s->debug_enabled = ini_get_bool(value, s->debug_enabled);
    }
    free(buf);
}

/* Re-poll the live DefaultProfile*.xml (hardware profile edits / match-start
 * preferences) — throttled to once per second from present_hook so a running
 * game picks up in-game <Setting> merges without hammering the disk. First
 * call runs immediately (it is just a mtime-check + parse when changed). */
void settings_poll_profile(void) {
    DWORD now = GetTickCount();
    if (s_poll_have && (DWORD)(now - s_poll_last) < 1000) return;
    s_poll_last = now;
    s_poll_have = 1;
    profile_path_find();
    profile_apply();
}

void settings_load(void) {
    if (!s_clock_noted) {
        s_clock_noted = 1;
        dlog("clock: using realtime QPC (s_tick is a frame counter, not game time)");
    }
    FILE *f = fopen(g_ini_path, "rb");
    if (f == NULL) {
        profile_path_find();
        profile_apply();
        return;
    }
    char line[256];
    char section[32];
    section[0] = '\0';
    ModSettings *s = &g_settings;
    while (fgets(line, sizeof(line), f) != NULL) {
        /* strip CR/LF */
        size_t L = strlen(line);
        while (L > 0 && (line[L-1] == '\n' || line[L-1] == '\r')) line[--L] = '\0';
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == ';' || *p == '#') continue;
        if (*p == '[') {
            char *rb = strchr(p, ']');
            if (rb == NULL) continue;
            *rb = '\0';
            lstrcpynA(section, p + 1, sizeof(section));
            continue;
        }
        char *eq = strchr(p, '=');
        if (eq == NULL) continue;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        size_t klen = strlen(key);
        while (klen > 0 && (key[klen-1] == ' ' || key[klen-1] == '\t')) key[--klen] = '\0';
        while (*val == ' ' || *val == '\t') val++;
        size_t vlen = strlen(val);
        while (vlen > 0 && (val[vlen-1] == ' ' || val[vlen-1] == '\t')) val[--vlen] = '\0';

        if (!strcmp(section, "General")) {
            if (!strcmp(key, "Enabled"))        s->enabled = ini_get_bool(val, s->enabled);
            else if (!strcmp(key, "Hotkey"))    s->hotkey = ini_get_int(val, s->hotkey);
            else if (!strcmp(key, "StartHidden")) s->start_hidden = ini_get_bool(val, s->start_hidden);
        } else if (!strcmp(section, "Display")) {
            if (!strcmp(key, "FontName"))       lstrcpynA(s->font_name, val, sizeof(s->font_name));
            else if (!strcmp(key, "FontSize"))  s->font_size = ini_get_int(val, s->font_size);
            else if (!strcmp(key, "Opacity"))   s->opacity = ini_get_float(val, s->opacity);
            else if (!strcmp(key, "PosX"))      s->pos_x = ini_get_int(val, s->pos_x);
            else if (!strcmp(key, "PosY"))      s->pos_y = ini_get_int(val, s->pos_y);
            else if (!strcmp(key, "ShowHeader")) s->show_header = ini_get_bool(val, s->show_header);
            else if (!strcmp(key, "ShowSlots567")) s->show_slots_567 = ini_get_bool(val, s->show_slots_567);
        } else if (!strcmp(section, "Rate")) {
            if (!strcmp(key, "SampleMs"))       s->sample_ms = ini_get_int(val, s->sample_ms);
            else if (!strcmp(key, "Smoothing")) lstrcpynA(s->smoothing, val, sizeof(s->smoothing));
            else if (!strcmp(key, "Unit"))      s->use_unit_min = (!strcmp(val, "min"));
            else if (!strcmp(key, "UseGameTime")) s->use_game_time = ini_get_bool(val, s->use_game_time);
            else if (!strcmp(key, "DiscontinuityRatio")) s->discontinuity_ratio = ini_get_float(val, s->discontinuity_ratio);
            else if (!strcmp(key, "ShowGains")) s->show_gains = ini_get_bool(val, s->show_gains);
        } else if (!strcmp(section, "Debug")) {
            if (!strcmp(key, "Enabled"))        s->debug_enabled = ini_get_bool(val, s->debug_enabled);
        }
    }
    fclose(f);

    profile_path_find();
    profile_apply();
}

void settings_save(void) {
    char tmp[MAX_PATH];
    _snprintf(tmp, sizeof(tmp), "%s.tmp", g_ini_path);
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) return;
    const ModSettings *s = &g_settings;
    fprintf(f,
            "; AoE3 Resource Rate Mod configuration\r\n"
            "; Edit while the game is NOT running, or via the F9 overlay panel.\r\n"
            "\r\n"
            "[General]\r\n"
            "Enabled=%d\r\n"
            "Hotkey=0x%X\r\n"
            "StartHidden=%d\r\n"
            "\r\n"
            "[Display]\r\n"
            "FontName=%s\r\n"
            "FontSize=%d\r\n"
            "Opacity=%.2f\r\n"
            "PosX=%d\r\n"
            "PosY=%d\r\n"
            "ShowHeader=%d\r\n"
            "ShowSlots567=%d\r\n"
            "\r\n"
            "[Rate]\r\n"
            "SampleMs=%d\r\n"
            "Smoothing=%s\r\n"
            "Unit=%s\r\n"
            "UseGameTime=%d\r\n"
            "DiscontinuityRatio=%.1f\r\n"
            "ShowGains=%d\r\n"
            "\r\n"
            "[Debug]\r\n"
            "Enabled=%d\r\n"
            "\r\n",
            s->enabled, (unsigned)s->hotkey, s->start_hidden,
            s->font_name, s->font_size, s->opacity,
            s->pos_x, s->pos_y, s->show_header, s->show_slots_567,
            s->sample_ms, s->smoothing,
            s->use_unit_min ? "min" : "sec",
            s->use_game_time, s->discontinuity_ratio, s->show_gains,
            s->debug_enabled);
    fclose(f);
    MoveFileExA(tmp, g_ini_path, MOVEFILE_REPLACE_EXISTING);
}