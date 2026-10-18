/*
 * ds_fmod.c - FMOD Ex 4.44 + FMOD Event, SILENT first pass.
 *
 * Every object the game asks for exists (a small arena block, so it is a
 * unique non-NULL guest pointer) and every call returns FMOD_OK, but nothing
 * makes sound: channels report "not playing", events report "ready". A real
 * backend (SDL audio, FSB banks) replaces this later.
 *
 * The C++ API is exported as stdcall member functions ("QAG" in the mangled
 * name): `this` is pushed first and the callee pops this + every parameter,
 * so argc below = 1 + the parameter count. bool and float travel as 32-bit
 * stack slots. FMOD_EventSystem_Create is a plain F_API (stdcall) function.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ds_hle.h"

#define FMOD_OK 0u
#define FMOD_EVENT_VERSION 0x00044464u     /* what the exe was built against (it logs both) */
#define FMOD_EVENT_STATE_READY 0x1u

enum { FO_EVENTSYSTEM = 0xF0D00001u, FO_SYSTEM, FO_SOUND, FO_CHANNEL, FO_DSP, FO_DSPCONN,
       FO_EVENT, FO_PARAM, FO_PROJECT, FO_CATEGORY, FO_GROUP };

/* ---- virtual interfaces
 * EventProject, EventCategory and EventGroup are abstract classes: the game
 * calls them through their vtable, not through imports. The slot order and
 * each method's stack size come from the game's own fmod_event.dll (the
 * EventProjectI / EventCategoryI / EventGroupI vtables, `ret N` of each
 * implementation). One generic shim serves every slot: it finds the method by
 * the VA it was called through. argc includes `this`. */
static int fmod_trace = -1;
enum { V_OK, V_OBJ, V_ZERO, V_ONE_F, V_SETUD, V_GETUD, V_READY, V_INFO };
typedef struct { const char *name; uint8_t argc, act, idx; uint32_t kind; } vmeth;
static const vmeth k_project[] = {
    { "EventProject::release", 1, V_OK }, { "EventProject::getInfo", 2, V_OK },
    { "EventProject::getGroup", 4, V_OBJ, 3, FO_GROUP }, { "EventProject::getGroupByIndex", 4, V_OBJ, 3, FO_GROUP },
    { "EventProject::getNumGroups", 2, V_ZERO, 1 }, { "EventProject::getEvent", 4, V_OBJ, 3, FO_EVENT },
    { "EventProject::getEventByProjectID", 4, V_OBJ, 3, FO_EVENT }, { "EventProject::getNumEvents", 2, V_ZERO, 1 },
    { "EventProject::loadSampleData", 6, V_OK }, { "EventProject::stopAllEvents", 2, V_OK },
    { "EventProject::cancelAllLoads", 1, V_OK }, { "EventProject::setUserData", 2, V_SETUD },
    { "EventProject::getUserData", 2, V_GETUD }, { "EventProject::getMemoryInfo", 5, V_ZERO, 3 },
    { 0 } };
static const vmeth k_category[] = {
    { "EventCategory::getInfo", 3, V_INFO }, { "EventCategory::getCategory", 3, V_OBJ, 2, FO_CATEGORY },
    { "EventCategory::getCategoryByIndex", 3, V_OBJ, 2, FO_CATEGORY }, { "EventCategory::getNumCategories", 2, V_ZERO, 1 },
    { "EventCategory::getEventByIndex", 4, V_OBJ, 3, FO_EVENT }, { "EventCategory::getNumEvents", 2, V_ZERO, 1 },
    { "EventCategory::getParentCategory", 2, V_ZERO, 1 }, { "EventCategory::stopAllEvents", 1, V_OK },
    { "EventCategory::setVolume", 2, V_OK }, { "EventCategory::getVolume", 2, V_ONE_F, 1 },
    { "EventCategory::setPitch", 3, V_OK }, { "EventCategory::getPitch", 3, V_ZERO, 1 },
    { "EventCategory::setPaused", 2, V_OK }, { "EventCategory::getPaused", 2, V_ZERO, 1 },
    { "EventCategory::setMute", 2, V_OK }, { "EventCategory::getMute", 2, V_ZERO, 1 },
    { "EventCategory::getChannelGroup", 2, V_ZERO, 1 }, { "EventCategory::setUserData", 2, V_SETUD },
    { "EventCategory::getUserData", 2, V_GETUD }, { "EventCategory::getMemoryInfo", 5, V_ZERO, 3 },
    { 0 } };
static const vmeth k_group[] = {
    { "EventGroup::getInfo", 3, V_INFO }, { "EventGroup::loadEventData", 3, V_OK },
    { "EventGroup::freeEventData", 3, V_OK }, { "EventGroup::getGroup", 4, V_OBJ, 3, FO_GROUP },
    { "EventGroup::getGroupByIndex", 4, V_OBJ, 3, FO_GROUP }, { "EventGroup::getParentGroup", 2, V_ZERO, 1 },
    { "EventGroup::getParentProject", 2, V_ZERO, 1 }, { "EventGroup::getNumGroups", 2, V_ZERO, 1 },
    { "EventGroup::getEvent", 4, V_OBJ, 3, FO_EVENT }, { "EventGroup::getEventByIndex", 4, V_OBJ, 3, FO_EVENT },
    { "EventGroup::getNumEvents", 2, V_ZERO, 1 }, { "EventGroup::getProperty", 3, V_OK },
    { "EventGroup::getPropertyByIndex", 3, V_OK }, { "EventGroup::getNumProperties", 2, V_ZERO, 1 },
    { "EventGroup::getState", 2, V_READY, 1 }, { "EventGroup::setUserData", 2, V_SETUD },
    { "EventGroup::getUserData", 2, V_GETUD }, { "EventGroup::getMemoryInfo", 5, V_ZERO, 3 },
    { 0 } };
static const struct { uint32_t kind; const vmeth *m; } k_vtables[] = {
    { FO_PROJECT, k_project }, { FO_CATEGORY, k_category }, { FO_GROUP, k_group } };
static uint32_t g_vt[3];                      /* guest vtable per virtual kind, built on first use */
static struct { uint32_t va; const vmeth *m; } g_vmap[64]; static int g_vmap_n;

static uint32_t obj_new(uint32_t kind);
static void sh_fmod_virtual(void) {           /* every EventProject/Category/Group slot */
    uint32_t va = ds_last_import_va();
    const vmeth *m = NULL;
    for (int i = 0; i < g_vmap_n; i++) if (g_vmap[i].va == va) { m = g_vmap[i].m; break; }
    if (!m) ds_fatal("fmod: unknown virtual method VA 0x%08X", va);
    uint32_t self = A32(0), p = m->idx ? A32(m->idx) : 0;
    switch (m->act) {
    case V_OBJ:   if (p) MEM32(p) = obj_new(m->kind); break;
    case V_ZERO:  if (p) MEM32(p) = 0; break;
    case V_ONE_F: if (p) MEM32(p) = 0x3F800000u; break;                  /* 1.0f */
    case V_READY: if (p) MEM32(p) = FMOD_EVENT_STATE_READY; break;
    case V_SETUD: MEM32(self + 4) = A32(1); break;
    case V_GETUD: if (A32(1)) MEM32(A32(1)) = MEM32(self + 4); break;
    case V_INFO: {                                                        /* (this, int *index, char **name) */
        static uint32_t empty; if (!empty) { empty = ds_arena_alloc(4, 4); MEM32(empty) = 0; }
        if (A32(1)) MEM32(A32(1)) = 0;
        if (A32(2)) MEM32(A32(2)) = empty;
        break; }
    }
    if (fmod_trace > 0) fprintf(stderr, "[fmod] %s\n", m->name);
    RET(FMOD_OK, m->argc);
}
static uint32_t vtable_for(uint32_t kind) {
    for (int k = 0; k < 3; k++) {
        if (k_vtables[k].kind != kind) continue;
        if (!g_vt[k]) {
            int n = 0; while (k_vtables[k].m[n].name) n++;
            g_vt[k] = ds_arena_alloc(4u * (uint32_t)n, 4);
            for (int i = 0; i < n; i++) {
                uint32_t va = ds_resolve(k_vtables[k].m[i].name);
                MEM32(g_vt[k] + 4u * (uint32_t)i) = va;
                if (g_vmap_n < 64) { g_vmap[g_vmap_n].va = va; g_vmap[g_vmap_n].m = &k_vtables[k].m[i]; g_vmap_n++; }
            }
        }
        return g_vt[k];
    }
    return 0;                                     /* non-virtual: methods are imports */
}

/* object layout (guest): +0 vtable (virtual kinds) or 0, +4 user data, +8 extra, +12 kind tag */
static uint32_t obj_new(uint32_t kind) {
    uint32_t o = ds_arena_alloc(16, 8);
    MEM32(o) = vtable_for(kind); MEM32(o + 4) = 0; MEM32(o + 8) = 0; MEM32(o + 12) = kind;
    return o;
}
static void out_obj(uint32_t pp, uint32_t kind) { if (pp) MEM32(pp) = obj_new(kind); }

static uint32_t g_system, g_projects[64], g_nprojects;
static void trace(const char *what, const char *arg) {
    if (fmod_trace < 0) { const char *e = getenv("DS_FMOD_TRACE"); fmod_trace = e && *e == '1'; }
    if (fmod_trace) fprintf(stderr, "[fmod] %s%s%s\n", what, arg ? " " : "", arg ? arg : "");
}

/* ===================== FMOD Event */
static void sh_FMOD_EventSystem_Create(void) {           /* (EventSystem **) */
    out_obj(A32(0), FO_EVENTSYSTEM);
    fprintf(stderr, "[fmod] FMOD_EventSystem_Create -> silent event system (no audio backend yet)\n");
    RET(FMOD_OK, 1);
}
static void sh_ES_getSystemObject(void) {                /* (this, System **) */
    if (!g_system) g_system = obj_new(FO_SYSTEM);
    if (A32(1)) MEM32(A32(1)) = g_system;
    RET(FMOD_OK, 2);
}
static void sh_ES_init(void)        { RET(FMOD_OK, 5); }  /* (this, maxchannels, flags, extradriverdata, eventflags) */
static void sh_ES_getVersion(void)  { if (A32(1)) MEM32(A32(1)) = FMOD_EVENT_VERSION; RET(FMOD_OK, 2); }
static void sh_ES_setMediaPath(void){ trace("setMediaPath", ASTR(1)); RET(FMOD_OK, 2); }
static void sh_ES_load(void) {                           /* (this, name, FMOD_EVENT_LOADINFO *, EventProject **) */
    trace("load", (MEM32(A32(1)) == 0x46464952u) ? "(bank from memory)" : ASTR(1));   /* "RIFF" */
    uint32_t p = obj_new(FO_PROJECT);
    if (g_nprojects < 64) g_projects[g_nprojects++] = p;
    if (A32(3)) MEM32(A32(3)) = p;
    RET(FMOD_OK, 4);
}
static void sh_ES_unload(void)      { g_nprojects = 0; RET(FMOD_OK, 1); }
static void sh_ES_getNumProjects(void) { if (A32(1)) MEM32(A32(1)) = g_nprojects; RET(FMOD_OK, 2); }
static void sh_ES_getProjectByIndex(void) {              /* (this, index, EventProject **) */
    uint32_t i = A32(1);
    if (A32(2)) MEM32(A32(2)) = i < g_nprojects ? g_projects[i] : 0;
    RET(i < g_nprojects ? FMOD_OK : 37u /*FMOD_ERR_INVALID_PARAM*/, 3);
}
static void sh_ES_getEvent(void) {                       /* (this, name, mode, Event **) */
    trace("getEvent", ASTR(1));
    out_obj(A32(3), FO_EVENT);
    RET(FMOD_OK, 4);
}
static void sh_ES_getCategory(void) { out_obj(A32(2), FO_CATEGORY); RET(FMOD_OK, 3); }   /* (this, name, EventCategory **) */
static void sh_ES_setReverbProperties(void) { RET(FMOD_OK, 2); }
static void sh_ES_getReverbPreset(void) {                /* (this, name, FMOD_REVERB_PROPERTIES *, int *index) */
    if (A32(2)) memset(ds_hostptr(A32(2)), 0, 0x58);     /* sizeof(FMOD_REVERB_PROPERTIES) in 4.44 */
    if (A32(3)) MEM32(A32(3)) = 0;
    RET(FMOD_OK, 4);
}
static void sh_ES_set3DListenerAttributes(void) { RET(FMOD_OK, 6); }  /* (this, listener, pos, vel, forward, up) */
static void sh_ES_update(void)      { RET(FMOD_OK, 1); }
static void sh_ES_release(void)     { RET(FMOD_OK, 1); }

static void sh_Ev_start(void)       { RET(FMOD_OK, 1); }
static void sh_Ev_stop(void)        { RET(FMOD_OK, 2); }  /* (this, bool immediate) */
static void sh_Ev_release(void)     { RET(FMOD_OK, 3); }  /* (this, bool freeeventdata, bool waituntilready) */
static void sh_Ev_setVolume(void)   { RET(FMOD_OK, 2); }  /* (this, float) */
static void sh_Ev_set3DAttributes(void) { RET(FMOD_OK, 4); }  /* (this, pos, vel, orientation) */
static void sh_Ev_getState(void)    { if (A32(1)) MEM32(A32(1)) = FMOD_EVENT_STATE_READY; RET(FMOD_OK, 2); }
static void sh_Ev_getParameter(void){ out_obj(A32(2), FO_PARAM); RET(FMOD_OK, 3); }   /* (this, name, EventParameter **) */
static void sh_EvP_setValue(void)   { RET(FMOD_OK, 2); }  /* (this, float) */

/* ===================== FMOD Ex low level */
static void sh_Sys_setFileSystem(void)      { RET(FMOD_OK, 8); }  /* (this, open, close, read, seek, asyncread, asynccancel, blockalign) */
static void sh_Sys_setStreamBufferSize(void){ RET(FMOD_OK, 3); }
static void sh_Sys_setSoftwareChannels(void){ RET(FMOD_OK, 2); }
static void sh_Sys_setSpeakerMode(void)     { RET(FMOD_OK, 2); }
static void sh_Sys_setSoftwareFormat(void)  { RET(FMOD_OK, 6); }  /* (this, rate, format, outch, maxinch, resampler) */
static void sh_Sys_createSound(void) {       /* (this, name_or_data, mode, FMOD_CREATESOUNDEXINFO *, Sound **) */
    uint32_t ex = A32(3);
    uint32_t s = obj_new(FO_SOUND);
    if (ex && MEM32(ex) >= 0x48) MEM32(s + 4) = MEM32(ex + 0x44);  /* exinfo.userdata: 18th field in 4.44 */
    if (A32(4)) MEM32(A32(4)) = s;
    RET(FMOD_OK, 5);
}
static void sh_Sys_playSound(void) {         /* (this, channelid, Sound *, bool paused, Channel **) */
    uint32_t c = obj_new(FO_CHANNEL);
    MEM32(c + 8) = A32(2);                   /* the channel's current sound */
    if (A32(4)) MEM32(A32(4)) = c;
    RET(FMOD_OK, 5);
}
static void sh_Sys_createDSPByType(void)    { out_obj(A32(2), FO_DSP); RET(FMOD_OK, 3); }  /* (this, type, DSP **) */
static void sh_Snd_release(void)            { RET(FMOD_OK, 1); }
static void sh_Snd_getUserData(void)        { if (A32(1)) MEM32(A32(1)) = A32(0) ? MEM32(A32(0) + 4) : 0; RET(FMOD_OK, 2); }
static void sh_Ch_isPlaying(void)           { if (A32(1)) MEM32(A32(1)) = 0; RET(FMOD_OK, 2); }   /* (this, bool *): silent = done */
static void sh_Ch_getCurrentSound(void)     { if (A32(1)) MEM32(A32(1)) = A32(0) ? MEM32(A32(0) + 8) : 0; RET(FMOD_OK, 2); }
static void sh_Ch_getPosition(void)         { if (A32(1)) MEM32(A32(1)) = 0; RET(FMOD_OK, 3); }   /* (this, uint *, unit) */
static void sh_Ch_setVolume(void)           { RET(FMOD_OK, 2); }
static void sh_Ch_setPriority(void)         { RET(FMOD_OK, 2); }
static void sh_Ch_setPaused(void)           { RET(FMOD_OK, 2); }
static void sh_Ch_stop(void)                { RET(FMOD_OK, 1); }
static void sh_DSP_release(void)            { RET(FMOD_OK, 1); }
static void sh_DSP_remove(void)             { RET(FMOD_OK, 1); }
static void sh_DSP_setParameter(void)       { RET(FMOD_OK, 3); }  /* (this, index, float) */
static void sh_CG_addDSP(void)              { out_obj(A32(2), FO_DSPCONN); RET(FMOD_OK, 3); }  /* (this, DSP *, DSPConnection **) */

const ds_shim ds_fmod[] = {
    { "FMOD_EventSystem_Create", sh_FMOD_EventSystem_Create },
    { "?getSystemObject@EventSystem@FMOD@@QAG?AW4FMOD_RESULT@@PAPAVSystem@2@@Z", sh_ES_getSystemObject },
    { "?init@EventSystem@FMOD@@QAG?AW4FMOD_RESULT@@HIPAXI@Z", sh_ES_init },
    { "?getVersion@EventSystem@FMOD@@QAG?AW4FMOD_RESULT@@PAI@Z", sh_ES_getVersion },
    { "?setMediaPath@EventSystem@FMOD@@QAG?AW4FMOD_RESULT@@PBD@Z", sh_ES_setMediaPath },
    { "?load@EventSystem@FMOD@@QAG?AW4FMOD_RESULT@@PBDPAUFMOD_EVENT_LOADINFO@@PAPAVEventProject@2@@Z", sh_ES_load },
    { "?unload@EventSystem@FMOD@@QAG?AW4FMOD_RESULT@@XZ", sh_ES_unload },
    { "?getNumProjects@EventSystem@FMOD@@QAG?AW4FMOD_RESULT@@PAH@Z", sh_ES_getNumProjects },
    { "?getProjectByIndex@EventSystem@FMOD@@QAG?AW4FMOD_RESULT@@HPAPAVEventProject@2@@Z", sh_ES_getProjectByIndex },
    { "?getEvent@EventSystem@FMOD@@QAG?AW4FMOD_RESULT@@PBDIPAPAVEvent@2@@Z", sh_ES_getEvent },
    { "?getCategory@EventSystem@FMOD@@QAG?AW4FMOD_RESULT@@PBDPAPAVEventCategory@2@@Z", sh_ES_getCategory },
    { "?setReverbProperties@EventSystem@FMOD@@QAG?AW4FMOD_RESULT@@PBUFMOD_REVERB_PROPERTIES@@@Z", sh_ES_setReverbProperties },
    { "?getReverbPreset@EventSystem@FMOD@@QAG?AW4FMOD_RESULT@@PBDPAUFMOD_REVERB_PROPERTIES@@PAH@Z", sh_ES_getReverbPreset },
    { "?set3DListenerAttributes@EventSystem@FMOD@@QAG?AW4FMOD_RESULT@@HPBUFMOD_VECTOR@@000@Z", sh_ES_set3DListenerAttributes },
    { "?update@EventSystem@FMOD@@QAG?AW4FMOD_RESULT@@XZ", sh_ES_update },
    { "?release@EventSystem@FMOD@@QAG?AW4FMOD_RESULT@@XZ", sh_ES_release },
    { "?start@Event@FMOD@@QAG?AW4FMOD_RESULT@@XZ", sh_Ev_start },
    { "?stop@Event@FMOD@@QAG?AW4FMOD_RESULT@@_N@Z", sh_Ev_stop },
    { "?release@Event@FMOD@@QAG?AW4FMOD_RESULT@@_N0@Z", sh_Ev_release },
    { "?setVolume@Event@FMOD@@QAG?AW4FMOD_RESULT@@M@Z", sh_Ev_setVolume },
    { "?set3DAttributes@Event@FMOD@@QAG?AW4FMOD_RESULT@@PBUFMOD_VECTOR@@00@Z", sh_Ev_set3DAttributes },
    { "?getState@Event@FMOD@@QAG?AW4FMOD_RESULT@@PAI@Z", sh_Ev_getState },
    { "?getParameter@Event@FMOD@@QAG?AW4FMOD_RESULT@@PBDPAPAVEventParameter@2@@Z", sh_Ev_getParameter },
    { "?setValue@EventParameter@FMOD@@QAG?AW4FMOD_RESULT@@M@Z", sh_EvP_setValue },
    { "?setFileSystem@System@FMOD@@QAG?AW4FMOD_RESULT@@P6G?AW43@PBDHPAIPAPAX2@ZP6G?AW43@PAX4@ZP6G?AW43@44I14@ZP6G?AW43@4I4@ZP6G?AW43@PAUFMOD_ASYNCREADINFO@@4@Z5H@Z", sh_Sys_setFileSystem },
    { "?setStreamBufferSize@System@FMOD@@QAG?AW4FMOD_RESULT@@II@Z", sh_Sys_setStreamBufferSize },
    { "?setSoftwareChannels@System@FMOD@@QAG?AW4FMOD_RESULT@@H@Z", sh_Sys_setSoftwareChannels },
    { "?setSpeakerMode@System@FMOD@@QAG?AW4FMOD_RESULT@@W4FMOD_SPEAKERMODE@@@Z", sh_Sys_setSpeakerMode },
    { "?setSoftwareFormat@System@FMOD@@QAG?AW4FMOD_RESULT@@HW4FMOD_SOUND_FORMAT@@HHW4FMOD_DSP_RESAMPLER@@@Z", sh_Sys_setSoftwareFormat },
    { "?createSound@System@FMOD@@QAG?AW4FMOD_RESULT@@PBDIPAUFMOD_CREATESOUNDEXINFO@@PAPAVSound@2@@Z", sh_Sys_createSound },
    { "?playSound@System@FMOD@@QAG?AW4FMOD_RESULT@@W4FMOD_CHANNELINDEX@@PAVSound@2@_NPAPAVChannel@2@@Z", sh_Sys_playSound },
    { "?createDSPByType@System@FMOD@@QAG?AW4FMOD_RESULT@@W4FMOD_DSP_TYPE@@PAPAVDSP@2@@Z", sh_Sys_createDSPByType },
    { "?release@Sound@FMOD@@QAG?AW4FMOD_RESULT@@XZ", sh_Snd_release },
    { "?getUserData@Sound@FMOD@@QAG?AW4FMOD_RESULT@@PAPAX@Z", sh_Snd_getUserData },
    { "?isPlaying@Channel@FMOD@@QAG?AW4FMOD_RESULT@@PA_N@Z", sh_Ch_isPlaying },
    { "?getCurrentSound@Channel@FMOD@@QAG?AW4FMOD_RESULT@@PAPAVSound@2@@Z", sh_Ch_getCurrentSound },
    { "?getPosition@Channel@FMOD@@QAG?AW4FMOD_RESULT@@PAII@Z", sh_Ch_getPosition },
    { "?setVolume@Channel@FMOD@@QAG?AW4FMOD_RESULT@@M@Z", sh_Ch_setVolume },
    { "?setPriority@Channel@FMOD@@QAG?AW4FMOD_RESULT@@H@Z", sh_Ch_setPriority },
    { "?setPaused@Channel@FMOD@@QAG?AW4FMOD_RESULT@@_N@Z", sh_Ch_setPaused },
    { "?stop@Channel@FMOD@@QAG?AW4FMOD_RESULT@@XZ", sh_Ch_stop },
    { "?release@DSP@FMOD@@QAG?AW4FMOD_RESULT@@XZ", sh_DSP_release },
    { "?remove@DSP@FMOD@@QAG?AW4FMOD_RESULT@@XZ", sh_DSP_remove },
    { "?setParameter@DSP@FMOD@@QAG?AW4FMOD_RESULT@@HM@Z", sh_DSP_setParameter },
    { "?addDSP@ChannelGroup@FMOD@@QAG?AW4FMOD_RESULT@@PAVDSP@2@PAPAVDSPConnection@2@@Z", sh_CG_addDSP },
    { "EventProject::release", sh_fmod_virtual },
    { "EventProject::getInfo", sh_fmod_virtual },
    { "EventProject::getGroup", sh_fmod_virtual },
    { "EventProject::getGroupByIndex", sh_fmod_virtual },
    { "EventProject::getNumGroups", sh_fmod_virtual },
    { "EventProject::getEvent", sh_fmod_virtual },
    { "EventProject::getEventByProjectID", sh_fmod_virtual },
    { "EventProject::getNumEvents", sh_fmod_virtual },
    { "EventProject::loadSampleData", sh_fmod_virtual },
    { "EventProject::stopAllEvents", sh_fmod_virtual },
    { "EventProject::cancelAllLoads", sh_fmod_virtual },
    { "EventProject::setUserData", sh_fmod_virtual },
    { "EventProject::getUserData", sh_fmod_virtual },
    { "EventProject::getMemoryInfo", sh_fmod_virtual },
    { "EventCategory::getInfo", sh_fmod_virtual },
    { "EventCategory::getCategory", sh_fmod_virtual },
    { "EventCategory::getCategoryByIndex", sh_fmod_virtual },
    { "EventCategory::getNumCategories", sh_fmod_virtual },
    { "EventCategory::getEventByIndex", sh_fmod_virtual },
    { "EventCategory::getNumEvents", sh_fmod_virtual },
    { "EventCategory::getParentCategory", sh_fmod_virtual },
    { "EventCategory::stopAllEvents", sh_fmod_virtual },
    { "EventCategory::setVolume", sh_fmod_virtual },
    { "EventCategory::getVolume", sh_fmod_virtual },
    { "EventCategory::setPitch", sh_fmod_virtual },
    { "EventCategory::getPitch", sh_fmod_virtual },
    { "EventCategory::setPaused", sh_fmod_virtual },
    { "EventCategory::getPaused", sh_fmod_virtual },
    { "EventCategory::setMute", sh_fmod_virtual },
    { "EventCategory::getMute", sh_fmod_virtual },
    { "EventCategory::getChannelGroup", sh_fmod_virtual },
    { "EventCategory::setUserData", sh_fmod_virtual },
    { "EventCategory::getUserData", sh_fmod_virtual },
    { "EventCategory::getMemoryInfo", sh_fmod_virtual },
    { "EventGroup::getInfo", sh_fmod_virtual },
    { "EventGroup::loadEventData", sh_fmod_virtual },
    { "EventGroup::freeEventData", sh_fmod_virtual },
    { "EventGroup::getGroup", sh_fmod_virtual },
    { "EventGroup::getGroupByIndex", sh_fmod_virtual },
    { "EventGroup::getParentGroup", sh_fmod_virtual },
    { "EventGroup::getParentProject", sh_fmod_virtual },
    { "EventGroup::getNumGroups", sh_fmod_virtual },
    { "EventGroup::getEvent", sh_fmod_virtual },
    { "EventGroup::getEventByIndex", sh_fmod_virtual },
    { "EventGroup::getNumEvents", sh_fmod_virtual },
    { "EventGroup::getProperty", sh_fmod_virtual },
    { "EventGroup::getPropertyByIndex", sh_fmod_virtual },
    { "EventGroup::getNumProperties", sh_fmod_virtual },
    { "EventGroup::getState", sh_fmod_virtual },
    { "EventGroup::setUserData", sh_fmod_virtual },
    { "EventGroup::getUserData", sh_fmod_virtual },
    { "EventGroup::getMemoryInfo", sh_fmod_virtual },
    { 0, 0 }
};
