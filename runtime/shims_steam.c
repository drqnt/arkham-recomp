// Steam: the game's ownership check and online features run on Valve's own arm64 libsteam_api, found in the user's
// Steam library. The verdict comes from the running Steam client; nothing here answers on its behalf.
// The phone has no Steam client. There the answers are the ones the Mac's Steam gave when the game was put on the phone
// (arkham --steam-check, which writes them only when Steam says the account owns the game; docs/IOS_PORT.md), and saves stay on the phone.
#include "shim.h"
#include "shim_reg.h"

#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define STEAM_MAGIC 0xEC000100u
#define APP_ID "35140"

static void *lib;
static int trace;

#if !TARGET_OS_IPHONE
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
#endif
#if TARGET_OS_IPHONE
// ---------- the phone: Steam's stored answers ----------
// Documents/Steam/answer.txt, one "key value" per line, and the cloud saves in Documents/Steam/cloud.
static struct {
    int loaded, owned, subscribed, low_violence, logged_on, dlc_n;
    char language[64], languages[512], persona[128];
    u64 steam_id;
    u32 dlc_id[64];
    u8 dlc_installed[64];
} answer;
static time_t started;
static const char *steam_dir(void) {
    static char d[1024];
    if (!d[0]) snprintf(d, sizeof d, "%s/Documents/Steam", getenv("HOME"));
    return d;
}
static int load_answer(void) {
    if (answer.loaded) return answer.owned;
    char path[1100];
    snprintf(path, sizeof path, "%s/answer.txt", steam_dir());
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[steam] no answer from Steam yet: %s is missing. tools/ios_install.sh asks the Mac's Steam and puts it there.\n", path);
        return 0;
    }
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *v = strchr(line, ' ');
        if (line[0] == '#' || !v) continue;
        *v++ = 0;
        if (!strcmp(line, "app") && strcmp(v, APP_ID)) break;  // an answer about another game
        else if (!strcmp(line, "owned")) answer.owned = atoi(v);
        else if (!strcmp(line, "subscribed")) answer.subscribed = atoi(v);
        else if (!strcmp(line, "low_violence")) answer.low_violence = atoi(v);
        else if (!strcmp(line, "logged_on")) answer.logged_on = atoi(v);
        else if (!strcmp(line, "language")) snprintf(answer.language, sizeof answer.language, "%s", v);
        else if (!strcmp(line, "languages")) snprintf(answer.languages, sizeof answer.languages, "%s", v);
        else if (!strcmp(line, "persona")) snprintf(answer.persona, sizeof answer.persona, "%s", v);
        else if (!strcmp(line, "steam_id")) answer.steam_id = strtoull(v, NULL, 10);
        else if (!strcmp(line, "dlc") && answer.dlc_n < 64) {
            unsigned id = 0, inst = 0;
            if (sscanf(v, "%u %u", &id, &inst) == 2) answer.dlc_id[answer.dlc_n] = id, answer.dlc_installed[answer.dlc_n++] = (u8)inst;
        }
    }
    fclose(f);
    answer.loaded = 1;
    fprintf(stderr, "[steam] Steam's stored answer: app %s owned %d\n", APP_ID, answer.owned);
    return answer.owned;
}
SHIM(api_Init) {
    started = time(NULL);
    RET(load_answer() == 1);
}
SHIM(api_IsSteamRunning) { RET(load_answer() == 1); }
SHIM(api_RestartAppIfNecessary) { RET(0); }
SHIM(api_RunCallbacks) {}
SHIM(api_Shutdown) {}
#endif
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

#if TARGET_OS_IPHONE
SHIM(apps_BIsSubscribed) { RET(answer.subscribed); }
SHIM(apps_BIsLowViolence) { RET(answer.low_violence); }
SHIM(apps_BIsCybercafe) { RET(0); }
SHIM(apps_BIsVACBanned) { RET(0); }
SHIM(apps_GetCurrentGameLanguage) { RET(rt_guest_strdup(answer.language[0] ? answer.language : "english")); }
SHIM(apps_GetAvailableGameLanguages) { RET(rt_guest_strdup(answer.languages[0] ? answer.languages : "english")); }
static int dlc_index(u32 id) {
    for (int i = 0; i < answer.dlc_n; i++)
        if (answer.dlc_id[i] == id) return i;
    return -1;
}
SHIM(apps_BIsSubscribedApp) {
    u32 app = ARG(1);
    int i = dlc_index(app);
    RET(app == (u32)atoi(APP_ID) ? answer.owned : i >= 0);
}
SHIM(apps_BIsDlcInstalled) {
    int i = dlc_index(ARG(1));
    RET(i >= 0 && answer.dlc_installed[i]);
}
SHIM(apps_GetDLCCount) { RET((u32)answer.dlc_n); }
SHIM(user_GetHSteamUser) { RET(1); }
SHIM(user_BLoggedOn) { RET(answer.logged_on); }
SHIM(user_GetSteamID) { RET64(answer.steam_id); }
SHIM(utils_GetSecondsSinceAppActive) { RET((u32)(time(NULL) - started)); }
SHIM(utils_GetSecondsSinceComputerActive) { RET(0); }
SHIM(utils_GetConnectedUniverse) { RET(1); }  // the public universe
SHIM(utils_GetServerRealTime) { RET((u32)time(NULL)); }
SHIM(utils_GetIPCountry) { RET(rt_guest_strdup("UA")); }
SHIM(utils_GetAppID) { RET((u32)atoi(APP_ID)); }
SHIM(utils_IsOverlayEnabled) { RET(0); }
SHIM(utils_BOverlayNeedsPresent) { RET(0); }
SHIM(utils_noop) { RET(0); }

// The cloud is a folder on the phone. The game names its files plainly (profile.bin, Save1...), and nothing else is let in.
static int cloud_path(const char *name, char *out, size_t cap) {
    if (!name || !*name || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..")) return 0;
    snprintf(out, cap, "%s/cloud", steam_dir());
    mkdir(steam_dir(), 0755);
    mkdir(out, 0755);
    snprintf(out, cap, "%s/cloud/%s", steam_dir(), name);
    return 1;
}
SHIM(store_FileWrite) {
    char path[1300], tmp[1310];
    if (!cloud_path(GS(ARG(1)), path, sizeof path)) {
        RET(0);
        return;
    }
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "wb");
    int ok = f && fwrite(GP(ARG(2)), 1, (size_t)ARG(3), f) == (size_t)ARG(3);
    if (f) ok = !fclose(f) && ok;
    ok = ok && !rename(tmp, path);  // a save cut short by the battery leaves the old one whole
    fprintf(stderr, "[steam] save written on the phone: %s, %d bytes -> %d\n", GS(ARG(1)), (int)ARG(3), ok);
    RET(ok);
}
SHIM(store_FileRead) {
    char path[1300];
    int n = 0;
    if (cloud_path(GS(ARG(1)), path, sizeof path)) {
        FILE *f = fopen(path, "rb");
        if (f) {
            n = (int)fread(GP(ARG(2)), 1, (size_t)ARG(3), f);
            fclose(f);
        }
    }
    fprintf(stderr, "[steam] save read on the phone: %s -> %d bytes\n", GS(ARG(1)), n);
    RET(n);
}
SHIM(store_FileForget) { RET(1); }
SHIM(store_FileDelete) {
    char path[1300];
    RET(cloud_path(GS(ARG(1)), path, sizeof path) && !unlink(path));
}
SHIM(store_SetSyncPlatforms) { RET(1); }
static int cloud_stat(CPU *c, struct stat *st) {
    char path[1300];
    return cloud_path(GS(ARG(1)), path, sizeof path) && !stat(path, st);
}
SHIM(store_FileExists) { struct stat st; RET(cloud_stat(c, &st)); }
SHIM(store_FilePersisted) { struct stat st; RET(cloud_stat(c, &st)); }
SHIM(store_GetFileSize) { struct stat st; RET(cloud_stat(c, &st) ? (u32)st.st_size : 0); }
SHIM(store_GetFileTimestamp) { struct stat st; i64 v = cloud_stat(c, &st) ? (i64)st.st_mtime : 0; RET64(v); }
// The listing is taken when the game asks how many files there are, and the names come from it by index.
static char cloud_names[64][256];
static int cloud_sizes[64], cloud_n;
SHIM(store_GetFileCount) {
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/cloud", steam_dir());
    cloud_n = 0;
    DIR *d = opendir(dir);
    for (struct dirent *e; d && (e = readdir(d)) && cloud_n < 64;) {
        size_t len = strlen(e->d_name);
        if (e->d_name[0] == '.' || (len > 4 && !strcmp(e->d_name + len - 4, ".new"))) continue;
        char path[1400];
        struct stat st;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (stat(path, &st) || !S_ISREG(st.st_mode)) continue;
        snprintf(cloud_names[cloud_n], sizeof cloud_names[0], "%s", e->d_name);
        cloud_sizes[cloud_n++] = (int)st.st_size;
    }
    if (d) closedir(d);
    RET((u32)cloud_n);
}
SHIM(store_GetFileNameAndSize) {
    int i = (int)ARG(1);
    if (i < 0 || i >= cloud_n) {
        if (ARG(2)) G32(ARG(2)) = 0;
        RET(0);
        return;
    }
    if (ARG(2)) G32(ARG(2)) = (u32)cloud_sizes[i];
    RET(rt_guest_strdup(cloud_names[i]));
}
SHIM(store_GetQuota) {
    if (ARG(1)) G32(ARG(1)) = 1000000000u;
    if (ARG(2)) G32(ARG(2)) = 999000000u;
    RET(1);
}
SHIM(store_IsCloudEnabledForAccount) { RET(1); }
SHIM(store_IsCloudEnabledForApp) { RET(1); }
SHIM(friends_GetPersonaName) { RET(rt_guest_strdup(answer.persona[0] ? answer.persona : "Player")); }
#else
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
    // Every save of a played game is kept in logs/saves/history, to go back to and to pick one for logs/saves/frozen.
    if (strncmp(GS(ARG(1)), "Save", 4) == 0) {
        char path[256];
        snprintf(path, sizeof path, "logs/saves/history/%s.%ld", GS(ARG(1)), (long)time(NULL));
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(GP(ARG(2)), 1, (size_t)ARG(3), f);
            fclose(f);
        }
    }
    RET(r);
}
SHIM(store_FileRead) {
    // A test run starts from a frozen copy if there is one, so that the place it starts from doesn't move as the game is played.
    if (cloud_read_only()) {
        char path[1024];  // ARKHAM_FROZEN_DIR may be a long path: 256 cut one short, and the game read its directory as an empty save
        const char *dir = getenv("ARKHAM_FROZEN_DIR");  // another set of frozen saves, for a test that needs another place
        snprintf(path, sizeof path, "%s/%s", dir ? dir : "logs/saves/frozen", GS(ARG(1)));
        FILE *f = fopen(path, "rb");
        if (f) {
            int n = (int)fread(GP(ARG(2)), 1, (size_t)ARG(3), f);
            fclose(f);
            fprintf(stderr, "[steam] test run: read frozen %s -> %d bytes\n", GS(ARG(1)), n);
            RET(n);
            return;
        }
    }
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


// ---------- arkham --steam-check <folder>: the answer the phone keeps ----------
// Asks this Mac's Steam (running and signed in; offline mode is fine) whether the account owns the game. Only if it does, writes the
// answers the game asks for to <folder>/answer.txt and copies the account's cloud saves for the game to <folder>/cloud, read only.
// tools/ios_install.sh puts the folder on the phone as Documents/Steam.
#include <sys/stat.h>
int rt_steam_check(const char *dir) {
    identify_app();
    if (!steam_lib()) return 1;
    int (*init_flat)(char *) = dlsym(lib, "SteamAPI_InitFlat");
    char err[1024] = "";
    if (!init_flat || init_flat(err) != 0) {
        fprintf(stderr, "Steam isn't answering (%s). Start Steam on this Mac and sign in; offline mode is fine.\n", err);
        return 1;
    }
    void *apps = APPS, *user = USER, *store = STORE, *friends = FRIENDS;
    int owned = FLAT(signed char, "ISteamApps_BIsSubscribedApp", void *, unsigned)(apps, (unsigned)atoi(APP_ID));
    if (!owned) {
        fprintf(stderr, "Steam says this account doesn't own Batman: Arkham Asylum GOTY (app %s), so there's nothing to put on the phone.\n", APP_ID);
        FLAT(void, "Shutdown", void)();
        return 1;
    }
    char path[1200];
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/cloud", dir);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/answer.txt", dir);
    FILE *f = fopen(path, "w");
    if (!f) {
        perror(path);
        return 1;
    }
    time_t now = time(NULL);
    char when[64];
    strftime(when, sizeof when, "%Y-%m-%d %H:%M", localtime(&now));
    fprintf(f, "# What this Mac's Steam answered for Batman: Arkham Asylum GOTY on %s. Written only when Steam says the account owns the game.\n", when);
    fprintf(f, "app %s\nowned 1\n", APP_ID);
    fprintf(f, "subscribed %d\n", FLAT(signed char, "ISteamApps_BIsSubscribed", void *)(apps));
    fprintf(f, "low_violence %d\n", FLAT(signed char, "ISteamApps_BIsLowViolence", void *)(apps));
    fprintf(f, "logged_on %d\n", FLAT(signed char, "ISteamUser_BLoggedOn", void *)(user));
    const char *s = FLAT(const char *, "ISteamApps_GetCurrentGameLanguage", void *)(apps);
    fprintf(f, "language %s\n", s ? s : "");
    s = FLAT(const char *, "ISteamApps_GetAvailableGameLanguages", void *)(apps);
    fprintf(f, "languages %s\n", s ? s : "");
    fprintf(f, "steam_id %llu\n", (unsigned long long)FLAT(u64, "ISteamUser_GetSteamID", void *)(user));
    s = FLAT(const char *, "ISteamFriends_GetPersonaName", void *)(friends);
    fprintf(f, "persona %s\n", s ? s : "");
    int ndlc = FLAT(int, "ISteamApps_GetDLCCount", void *)(apps);
    for (int i = 0; i < ndlc; i++) {
        unsigned id = 0;
        signed char avail = 0;
        char name[256] = "";
        if (FLAT(signed char, "ISteamApps_BGetDLCDataByIndex", void *, int, unsigned *, signed char *, char *, int)(apps, i, &id, &avail, name, sizeof name))
            fprintf(f, "dlc %u %d\n", id, FLAT(signed char, "ISteamApps_BIsDlcInstalled", void *, unsigned)(apps, id));
    }
    fclose(f);
    int nfiles = FLAT(int, "ISteamRemoteStorage_GetFileCount", void *)(store), copied = 0;
    for (int i = 0; i < nfiles; i++) {
        int size = 0;
        const char *name = FLAT(const char *, "ISteamRemoteStorage_GetFileNameAndSize", void *, int, int *)(store, i, &size);
        if (!name || strchr(name, '/') || size < 0 || size > (64 << 20)) continue;
        void *buf = malloc((size_t)size + 1);
        int n = FLAT(int, "ISteamRemoteStorage_FileRead", void *, const char *, void *, int)(store, name, buf, size);
        snprintf(path, sizeof path, "%s/cloud/%s", dir, name);
        FILE *o = n == size ? fopen(path, "wb") : NULL;
        if (o) {
            fwrite(buf, 1, (size_t)n, o);
            fclose(o);
            copied++;
        }
        free(buf);
    }
    FLAT(void, "Shutdown", void)();
    printf("Steam says the account owns the game. Answer and %d of %d cloud saves are in %s.\n", copied, nfiles, dir);
    return 0;
}
#endif

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
#if TARGET_OS_IPHONE
    if (name && strstr(name, "libsteam_api")) return STEAM_MAGIC;  // whether the game may run is SteamAPI_Init's answer
#else
    if (name && strstr(name, "libsteam_api") && steam_lib()) return STEAM_MAGIC;
#endif
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
