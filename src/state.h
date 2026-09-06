/* state.h — shared declarations for all modules (d3d9 proxy DLL, R14) */
#ifndef STATE_H
#define STATE_H

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define MAX_SLOTS 8u
#define RATE_WINDOW 60.0f
#define MAX_HIST 1024u

/* ---- addressing (VERIFIED) ---- */
#define RVA_GAME_PTR           0x00866234u
#define OFF_GAME_CTX           0x13c
#define OFF_GAME_ACTIVE_PLAYER 0x14c
#define OFF_CTX_PLAYERS        0x58
#define OFF_CTX_PLAYERCNT      0x5c
#define OFF_PLAYER_RES         0x230
#define OFF_PLAYER_RES_CAP     0x238
#define OFF_PLAYER_RES_ADD     0x240
#define OFF_PLAYER_INCOME      0x80
#define RVA_KEY_TABLE          0x0086DF14u
#define RVA_SLOT_COUNT         0x0086DF38u
#define OFF_INC_CUR            0x160
#define OFF_INC_PREV           0x164
#define OFF_INC_HIST           0x168
#define OFF_INC_COUNT          0x16c
#define OFF_CTX_SNAP           0x108
#define RVA_TIME_STEP          0x0079A5C8u
#define RVA_NEG_FUDGE          0x007A12FCu
#define RVA_WIN_THRESH         0x007A12E8u
#define RVA_DIV_CONST          0x007856BCu

/* ring buffer */
#define SAMPLE_RING_SIZE 1024u
#define SAMPLE_MS_DEFAULT 500u

/* version gate */
#define EXPECTED_EXE_SIZE  11598648u
#define EXPECTED_PE_VER_HI 6u
#define EXPECTED_PE_VER_LO 108u
#define EXPECTED_PE_VER_R  321u
#define EXPECTED_PE_VER_B  137u
#define EXPECTED_IMAGE_BASE 0x400000u

/* ---- globals (defined in d3d9.c) ---- */
extern HMODULE g_real;
extern void   *g_base;
extern void   *g_ctx;
extern void   *g_res;
extern void   *g_inc;
extern void   *g_device;
extern void  **g_devvt;
extern int     g_player_idx;
extern int     s_snap_have;
extern DWORD   g_obs_player;
extern DWORD   s_last_ctx;
extern DWORD   s_last_player;
extern int     s_last_n;
extern int     s_tick;
extern volatile char g_step[64];
extern FILE   *g_log;
extern LPTOP_LEVEL_EXCEPTION_FILTER s_prev_filter;

/* ---- logger ---- */
void dlog(const char *fmt, ...);
void logger_init(void);
void logger_debug(const char *fmt, ...);

/* ---- crash / fault ---- */
void set_step(const char *s);
int  fault_code_benign(DWORD code);
long WINAPI vectored_fault_filter(struct _EXCEPTION_POINTERS *ep);  /* log-only */
long WINAPI fault_filter(struct _EXCEPTION_POINTERS *ep);           /* log + exit */

/* ---- guarded reads ---- */
BOOL page_readable(DWORD addr);
BOOL read32(DWORD addr, DWORD *out);
DWORD safe_r32(DWORD addr);
#ifdef SWARM_TEST
float safe_rf(DWORD addr);
void *safe_rptr(DWORD addr);
#endif

/* ---- decrypt ---- */
float decrypt_slot_at(DWORD rbase, int slot);
#ifdef SWARM_TEST
float decrypt_slot(int slot);
#endif

/* ---- chain log ---- */
void dlog_chain(DWORD game, DWORD ctx, int n, DWORD player, DWORD res, DWORD inc, const char *tag);

/* ---- chain walk ---- */
void locate_resources_impl(void);
void locate_resources(void);
#ifdef SWARM_TEST
void locate_resources_at(void *base);
#endif

/* ---- rate_for (FUN_0086da39 reproduction, SWARM_TEST only) ---- */
#ifdef SWARM_TEST
float rate_for(int slot);
#endif

/* ---- observer ---- */
void observer_sample(void);

/* ---- tracker ---- */
typedef struct {
    DWORD tick;
    float value;
} Sample;
typedef struct {
    Sample ring[SAMPLE_RING_SIZE];
    unsigned int head;
    unsigned int count;
} SlotTracker;
extern SlotTracker g_tracker[MAX_SLOTS];
extern DWORD g_last_sample_tick;
extern int   g_paused;
extern float g_last_values[MAX_SLOTS];
extern int   g_values_valid;
extern float g_last_ema[MAX_SLOTS];
extern int   g_slot_frozen[MAX_SLOTS];

void tracker_init(void);
void tracker_reset(void);
void tracker_sample(DWORD current_tick, const float *values, int num_slots);

/* ---- rate helpers ---- */
float rate_get_ema(int slot);
float rate_get_raw(int slot);
float rate_display(int slot);
const char *rate_unit_label(void);

/* ---- settings state ---- */
typedef struct {
    int    enabled;
    int    hotkey;
    int    start_hidden;
    char   font_name[64];
    int    font_size;
    float  opacity;
    int    pos_x;
    int    pos_y;
    int    show_header;
    int    show_slots_567;
    int    sample_ms;
    char   smoothing[16];
    int    use_unit_min;
    int    use_game_time;
    float  discontinuity_ratio;
    int    show_gains;
    int    debug_enabled;
} ModSettings;
extern ModSettings g_settings;
extern int g_panel_visible;

/* version gate */
extern int  g_version_ok;
extern char g_version_reason[128];
void version_gate_check(void);

void settings_init(void);
void settings_load(void);
void settings_save(void);

/* ---- crash safety (re-arm + direct fault write) ---- */
void ensure_fault_filter(void);

/* ---- UI ---- */
void ui_init(void);
void ui_create_font(void *dev);   /* device-create + Reset only, never mid-frame */
void ui_draw(void);
void ui_on_reset(void);
void ui_toggle_panel(void);
void ui_check_hotkey(void);

#endif /* STATE_H */
