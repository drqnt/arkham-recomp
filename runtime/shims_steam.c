// Steam: the game's ownership check and online features run on Valve's own arm64 libsteam_api, found in the user's
// Steam library. The verdict comes from the running Steam client; nothing here answers on its behalf.
#include "shim.h"
#include "shim_reg.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

#define STEAM_MAGIC 0xEC000100u
#define APP_ID "35140"

static void *lib;
static int trace;

static const char *const candidates[] = {
    "steamapps/common/Control/Game.app/Contents/MacOS/libsteam_api.dylib",
    "steamapps/common/BOMBANANA!/BOMBANANA.app/Contents/PlugIns/libsteam_api.dylib",
    "steamapps/common/BOMBANANA! Demo/BOMBANANA.app/Contents/PlugIns/libsteam_api.dylib",
};

static void *steam_lib(void) {
    if (lib) return lib;
    const char *over = getenv("ARKHAM_STEAM_API");
    char path[1200];
    if (over) lib = dlopen(over, RTLD_NOW | RTLD_LOCAL);
    for (unsigned i = 0; !lib && i < sizeof candidates / sizeof candidates[0]; i++) {
        snprintf(path, sizeof path, "%s/Library/Application Support/Steam/%s", getenv("HOME"), candidates[i]);
        lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    }
    if (!lib) fprintf(stderr, "[steam] no arm64 libsteam_api found in the Steam library\n");
    return lib;
}

// Tell the client which app this is, the documented way for a process not launched by Steam itself.
static void identify_app(void) {
    setenv("SteamAppId", APP_ID, 1);
    setenv("SteamGameId", APP_ID, 1);
}

SHIM(api_Init) {
    identify_app();
    int (*init_flat)(char *) = dlsym(steam_lib(), "SteamAPI_InitFlat");
    char err[1024] = "";
    int r = init_flat ? init_flat(err) : -1;
    fprintf(stderr, "[steam] SteamAPI_InitFlat -> %d %s\n", r, err);
    RET(r == 0);
}
SHIM(api_IsSteamRunning) {
    signed char (*f)(void) = dlsym(steam_lib(), "SteamAPI_IsSteamRunning");
    RET(f ? f() : 0);
}
SHIM(api_RestartAppIfNecessary) {
    identify_app();
    signed char (*f)(unsigned) = dlsym(steam_lib(), "SteamAPI_RestartAppIfNecessary");
    int r = f ? f(ARG(0)) : 0;
    fprintf(stderr, "[steam] SteamAPI_RestartAppIfNecessary(%u) -> %d\n", ARG(0), r);
    RET(r);
}
SHIM(api_RunCallbacks) {
    void (*f)(void) = dlsym(steam_lib(), "SteamAPI_RunCallbacks");
    if (f) f();
}
SHIM(api_Shutdown) {
    void (*f)(void) = dlsym(steam_lib(), "SteamAPI_Shutdown");
    if (f) f();
}
SHIM(api_noop) { RET(0); }

// ---------- C++ interfaces: guest objects whose vtables call Valve's flat API ----------
u32 rt_dyn_import(const char *name, void (*fn)(CPU *));
u32 rt_dyn_import_arg(void (*fn)(CPU *), u32 arg);
extern __thread u32 rt_dyn_cur_arg;

#define FLAT(ret, name, ...) ((ret(*)(__VA_ARGS__))dlsym(lib, "SteamAPI_" name))
#define HOSTI(accessor) (((void *(*)(void))dlsym(lib, "SteamAPI_" accessor))())

static const char *const iface_names[] = {"SteamApps", "SteamUser", "SteamUtils", "SteamFriends", "SteamUserStats",
    "SteamRemoteStorage", "SteamNetworking", "SteamMatchmaking", "SteamMatchmakingServers", "SteamClient",
    "SteamController", "SteamHTMLSurface", "SteamUGC", "SteamInventory", "SteamMusic", "SteamVideo"};
#define N_IFACES (sizeof iface_names / sizeof iface_names[0])
#define VT_SLOTS 128u
static u32 proxies[N_IFACES];

SHIM(not_bridged) {
    u32 a = rt_dyn_cur_arg;
    static u8 seen[N_IFACES][VT_SLOTS];
    u32 ifc = a >> 16, slot = a & 0xFFFF;
    if (!seen[ifc][slot]) {
        seen[ifc][slot] = 1;
        fprintf(stderr, "[steam] %s vtable slot %u is not bridged yet (from %08x)\n", iface_names[ifc], slot, G32(c->esp));
    }
    RET(0);
    c->edx = 0;
}

// ISteamApps, in the slot order of the SDK this game was built against. Slot 6 is the ownership check itself.
#define APPS HOSTI("SteamApps_v008")
SHIM(apps_BIsSubscribed) { RET(FLAT(signed char, "ISteamApps_BIsSubscribed", void *)(APPS)); }
SHIM(apps_BIsLowViolence) { RET(FLAT(signed char, "ISteamApps_BIsLowViolence", void *)(APPS)); }
SHIM(apps_BIsCybercafe) { RET(FLAT(signed char, "ISteamApps_BIsCybercafe", void *)(APPS)); }
SHIM(apps_BIsVACBanned) { RET(FLAT(signed char, "ISteamApps_BIsVACBanned", void *)(APPS)); }
SHIM(apps_GetCurrentGameLanguage) {
    const char *s = FLAT(const char *, "ISteamApps_GetCurrentGameLanguage", void *)(APPS);
    RET(s ? rt_guest_strdup(s) : 0);
}
SHIM(apps_GetAvailableGameLanguages) {
    const char *s = FLAT(const char *, "ISteamApps_GetAvailableGameLanguages", void *)(APPS);
    RET(s ? rt_guest_strdup(s) : 0);
}
SHIM(apps_BIsSubscribedApp) {
    u32 app = ARG(1);
    int r = FLAT(signed char, "ISteamApps_BIsSubscribedApp", void *, unsigned)(APPS, app);
    fprintf(stderr, "[steam] Steam says app %u owned: %d\n", app, r);
    RET(r);
}
SHIM(apps_BIsDlcInstalled) { RET(FLAT(signed char, "ISteamApps_BIsDlcInstalled", void *, unsigned)(APPS, ARG(1))); }
SHIM(apps_GetDLCCount) { RET(FLAT(int, "ISteamApps_GetDLCCount", void *)(APPS)); }

#define USER HOSTI("SteamUser_v023")
#define UTILS HOSTI("SteamUtils_v010")
SHIM(user_GetHSteamUser) { RET(FLAT(int, "ISteamUser_GetHSteamUser", void *)(USER)); }
SHIM(user_BLoggedOn) { RET(FLAT(signed char, "ISteamUser_BLoggedOn", void *)(USER)); }
SHIM(user_GetSteamID) { u64 v = FLAT(u64, "ISteamUser_GetSteamID", void *)(USER); RET64(v); }
SHIM(utils_GetSecondsSinceAppActive) { RET(FLAT(unsigned, "ISteamUtils_GetSecondsSinceAppActive", void *)(UTILS)); }
SHIM(utils_GetSecondsSinceComputerActive) { RET(FLAT(unsigned, "ISteamUtils_GetSecondsSinceComputerActive", void *)(UTILS)); }
SHIM(utils_GetConnectedUniverse) { RET(FLAT(int, "ISteamUtils_GetConnectedUniverse", void *)(UTILS)); }
SHIM(utils_GetServerRealTime) { RET(FLAT(unsigned, "ISteamUtils_GetServerRealTime", void *)(UTILS)); }
SHIM(utils_GetIPCountry) {
    const char *s = FLAT(const char *, "ISteamUtils_GetIPCountry", void *)(UTILS);
    RET(s ? rt_guest_strdup(s) : 0);
}
SHIM(utils_GetAppID) { RET(FLAT(unsigned, "ISteamUtils_GetAppID", void *)(UTILS)); }
SHIM(utils_IsOverlayEnabled) { RET(0); }
SHIM(utils_BOverlayNeedsPresent) { RET(0); }
SHIM(utils_noop) { RET(0); }

// ISteamRemoteStorage: the game's saves go through Steam Cloud, so these are the real client's calls.
#define STORE HOSTI("SteamRemoteStorage_v016")
// Scripted test runs read the player's real saves but must never change them.
static int cloud_read_only(void) { return getenv("ARKHAM_INPUT") != NULL; }
SHIM(store_FileWrite) {
    if (cloud_read_only()) {
        fprintf(stderr, "[steam] test run: not writing %s, %d bytes\n", GS(ARG(1)), (int)ARG(3));
        RET(1);
        return;
    }
    int r = FLAT(signed char, "ISteamRemoteStorage_FileWrite", void *, const char *, const void *, int)(STORE, GS(ARG(1)), GP(ARG(2)), (int)ARG(3));
    fprintf(stderr, "[steam] cloud write %s, %d bytes -> %d\n", GS(ARG(1)), (int)ARG(3), r);
    RET(r);
}
SHIM(store_FileRead) {
    int r = FLAT(int, "ISteamRemoteStorage_FileRead", void *, const char *, void *, int)(STORE, GS(ARG(1)), GP(ARG(2)), (int)ARG(3));
    fprintf(stderr, "[steam] cloud read %s -> %d bytes\n", GS(ARG(1)), r);
    RET(r);
}
SHIM(store_FileForget) { if (cloud_read_only()) { RET(1); return; } RET(FLAT(signed char, "ISteamRemoteStorage_FileForget", void *, const char *)(STORE, GS(ARG(1)))); }
SHIM(store_FileDelete) {
    if (cloud_read_only()) {
        fprintf(stderr, "[steam] test run: not deleting %s\n", GS(ARG(1)));
        RET(1);
        return;
    }
    int r = FLAT(signed char, "ISteamRemoteStorage_FileDelete", void *, const char *)(STORE, GS(ARG(1)));
    fprintf(stderr, "[steam] cloud delete %s -> %d\n", GS(ARG(1)), r);
    RET(r);
}
SHIM(store_SetSyncPlatforms) { RET(FLAT(signed char, "ISteamRemoteStorage_SetSyncPlatforms", void *, const char *, int)(STORE, GS(ARG(1)), (int)ARG(2))); }
SHIM(store_FileExists) { RET(FLAT(signed char, "ISteamRemoteStorage_FileExists", void *, const char *)(STORE, GS(ARG(1)))); }
SHIM(store_FilePersisted) { RET(FLAT(signed char, "ISteamRemoteStorage_FilePersisted", void *, const char *)(STORE, GS(ARG(1)))); }
SHIM(store_GetFileSize) { RET(FLAT(int, "ISteamRemoteStorage_GetFileSize", void *, const char *)(STORE, GS(ARG(1)))); }
SHIM(store_GetFileTimestamp) { i64 v = FLAT(i64, "ISteamRemoteStorage_GetFileTimestamp", void *, const char *)(STORE, GS(ARG(1))); RET64(v); }
SHIM(store_GetFileCount) { RET(FLAT(int, "ISteamRemoteStorage_GetFileCount", void *)(STORE)); }
SHIM(store_GetFileNameAndSize) {
    int size = 0;
    const char *s = FLAT(const char *, "ISteamRemoteStorage_GetFileNameAndSize", void *, int, int *)(STORE, (int)ARG(1), &size);
    if (ARG(2)) G32(ARG(2)) = (u32)size;
    RET(s ? rt_guest_strdup(s) : 0);
}
// This game's version of the call takes 32-bit sizes.
SHIM(store_GetQuota) {
    u64 total = 0, avail = 0;
    int r = FLAT(signed char, "ISteamRemoteStorage_GetQuota", void *, u64 *, u64 *)(STORE, &total, &avail);
    if (ARG(1)) G32(ARG(1)) = total > 0x7FFFFFFFu ? 0x7FFFFFFFu : (u32)total;
    if (ARG(2)) G32(ARG(2)) = avail > 0x7FFFFFFFu ? 0x7FFFFFFFu : (u32)avail;
    fprintf(stderr, "[steam] cloud quota %llu of %llu free -> %d\n", (unsigned long long)avail, (unsigned long long)total, r);
    RET(r);
}
SHIM(store_IsCloudEnabledForAccount) { RET(FLAT(signed char, "ISteamRemoteStorage_IsCloudEnabledForAccount", void *)(STORE)); }
SHIM(store_IsCloudEnabledForApp) { RET(FLAT(signed char, "ISteamRemoteStorage_IsCloudEnabledForApp", void *)(STORE)); }
#define FRIENDS HOSTI("SteamFriends_v018")
SHIM(friends_GetPersonaName) {
    const char *s = FLAT(const char *, "ISteamFriends_GetPersonaName", void *)(FRIENDS);
    RET(s ? rt_guest_strdup(s) : 0);
}

typedef struct { const char *iface; u32 slot; void (*fn)(CPU *); } SlotDef;
static const SlotDef slots[] = {
    {"SteamRemoteStorage", 0, shim_store_FileWrite}, {"SteamRemoteStorage", 1, shim_store_FileRead}, {"SteamRemoteStorage", 2, shim_store_FileForget},
    {"SteamRemoteStorage", 3, shim_store_FileDelete}, {"SteamRemoteStorage", 5, shim_store_SetSyncPlatforms}, {"SteamRemoteStorage", 10, shim_store_FileExists},
    {"SteamRemoteStorage", 11, shim_store_FilePersisted}, {"SteamRemoteStorage", 12, shim_store_GetFileSize}, {"SteamRemoteStorage", 13, shim_store_GetFileTimestamp},
    {"SteamRemoteStorage", 15, shim_store_GetFileCount}, {"SteamRemoteStorage", 16, shim_store_GetFileNameAndSize}, {"SteamRemoteStorage", 17, shim_store_GetQuota},
    {"SteamRemoteStorage", 18, shim_store_IsCloudEnabledForAccount}, {"SteamRemoteStorage", 19, shim_store_IsCloudEnabledForApp},
    {"SteamFriends", 0, shim_friends_GetPersonaName},
    {"SteamApps", 0, shim_apps_BIsSubscribed}, {"SteamApps", 1, shim_apps_BIsLowViolence}, {"SteamApps", 2, shim_apps_BIsCybercafe},
    {"SteamApps", 3, shim_apps_BIsVACBanned}, {"SteamApps", 4, shim_apps_GetCurrentGameLanguage},
    {"SteamApps", 5, shim_apps_GetAvailableGameLanguages}, {"SteamApps", 6, shim_apps_BIsSubscribedApp},
    {"SteamApps", 7, shim_apps_BIsDlcInstalled}, {"SteamApps", 10, shim_apps_GetDLCCount},
    {"SteamUser", 0, shim_user_GetHSteamUser}, {"SteamUser", 1, shim_user_BLoggedOn}, {"SteamUser", 2, shim_user_GetSteamID},
    {"SteamUtils", 0, shim_utils_GetSecondsSinceAppActive}, {"SteamUtils", 1, shim_utils_GetSecondsSinceComputerActive},
    {"SteamUtils", 2, shim_utils_GetConnectedUniverse}, {"SteamUtils", 3, shim_utils_GetServerRealTime},
    {"SteamUtils", 4, shim_utils_GetIPCountry}, {"SteamUtils", 9, shim_utils_GetAppID}, {"SteamUtils", 10, shim_utils_noop},
    {"SteamUtils", 16, shim_utils_noop}, {"SteamUtils", 17, shim_utils_IsOverlayEnabled}, {"SteamUtils", 18, shim_utils_BOverlayNeedsPresent},
};

static u32 proxy_for(u32 ifc) {
    if (proxies[ifc]) return proxies[ifc];
    u32 vt = rt_guest_calloc(VT_SLOTS, 4);
    for (u32 i = 0; i < VT_SLOTS; i++) {
        void (*fn)(CPU *) = shim_not_bridged;
        for (unsigned k = 0; k < sizeof slots / sizeof slots[0]; k++) {
            if (slots[k].slot == i && !strcmp(slots[k].iface, iface_names[ifc])) fn = slots[k].fn;
        }
        G32(vt + i * 4) = rt_dyn_import_arg(fn, (ifc << 16) | i);
    }
    u32 obj = rt_guest_calloc(1, 16);
    G32(obj) = vt;
    proxies[ifc] = obj;
    return obj;
}
SHIM(iface_accessor) { RET(proxy_for(rt_dyn_cur_arg)); }

// Callback objects are accepted; delivering Steam's callbacks into guest objects comes when a feature needs it.
SHIM(api_RegisterCallback) { if (trace) fprintf(stderr, "[steam] callback %d registered, delivery not connected yet\n", (int)ARG(1)); }

static const ShimReg steam_table[] = {
    R2("SteamAPI_RegisterCallback", api_RegisterCallback) R2("SteamAPI_UnregisterCallback", api_noop)
    R2("SteamAPI_RegisterCallResult", api_noop) R2("SteamAPI_UnregisterCallResult", api_noop)
    R2("SteamAPI_Init", api_Init) R2("SteamAPI_IsSteamRunning", api_IsSteamRunning)
    R2("SteamAPI_RestartAppIfNecessary", api_RestartAppIfNecessary) R2("SteamAPI_RunCallbacks", api_RunCallbacks)
    R2("SteamAPI_Shutdown", api_Shutdown) R2("SteamAPI_SetMiniDumpComment", api_noop) R2("SteamAPI_WriteMiniDump", api_noop)
    {NULL, NULL},
};

u32 rt_steam_dlopen(const char *name) {
    trace = getenv("ARKHAM_TRACE_DL") != NULL;
    if (name && strstr(name, "libsteam_api") && steam_lib()) return STEAM_MAGIC;
    return 0;
}
u32 rt_steam_dlsym(u32 handle, const char *sym) {
    if (handle != STEAM_MAGIC) return 0;
    for (const ShimReg *r = steam_table; r->name; r++) {
        if (!strcmp(r->name, sym)) return rt_dyn_import(r->name, r->fn);
    }
    for (u32 i = 0; i < N_IFACES; i++) {
        if (!strcmp(iface_names[i], sym)) return rt_dyn_import_arg(shim_iface_accessor, i);
    }
    if (trace) fprintf(stderr, "[steam] game asked for %s, not bridged yet\n", sym);
    return 0;
}
