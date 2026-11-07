// CoreFoundation: real host objects behind 32-bit guest handles. Guest constant strings from the
// __cfstring section are converted to host strings on first use.
#include "shim.h"
#include "shim_reg.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>
#include <dirent.h>
#include <mach/mach_time.h>
#include <dlfcn.h>
#include <mach-o/loader.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

// ---------- handle <-> object ----------
static CFMutableDictionaryRef obj_to_handle;
static CFMutableDictionaryRef literal_cache;
static u32 cfstring_lo, cfstring_hi;

static void *call_host(const char *name) { return dlsym(RTLD_DEFAULT, name); }

// Tagged pointers (dates, small numbers, short strings) never get freed and each value is a new "object",
// so they use a small ring of handles instead of the permanent table.
#define TAGGED_RING 4096
static u32 tagged_ring[TAGGED_RING];
static u32 tagged_next;

u32 cf_wrap(CFTypeRef o) {
    if (!o) return 0;
    if ((intptr_t)o < 0) {
        u32 k = tagged_next++ % TAGGED_RING;
        if (tagged_ring[k]) rt_handle_free(tagged_ring[k]);
        tagged_ring[k] = rt_handle_new((void *)o);
        return tagged_ring[k];
    }
    const void *v = CFDictionaryGetValue(obj_to_handle, o);
    if (v) return (u32)(uintptr_t)v;
    u32 h = rt_handle_new((void *)o);
    CFDictionarySetValue(obj_to_handle, o, (const void *)(uintptr_t)h);
    return h;
}

static CFStringRef literal(u32 g) {
    const void *key = (const void *)(uintptr_t)g;
    CFStringRef s = CFDictionaryGetValue(literal_cache, key);
    if (s) return s;
    u32 flags = G32(g + 4), data = G32(g + 8), len = G32(g + 12);
    if (flags & 0x10)  // UTF-16 literal
        s = CFStringCreateWithCharacters(NULL, GP(data), (CFIndex)len);
    else
        s = CFStringCreateWithBytes(NULL, GP(data), (CFIndex)len, kCFStringEncodingMacRoman, false);
    CFDictionarySetValue(literal_cache, key, s);
    return s;
}

CFTypeRef cf_unwrap(u32 g) {
    if (!g) return NULL;
    if (rt_is_handle(g)) return rt_handle_get(g);
    if (g >= cfstring_lo && g < cfstring_hi) return literal(g);
    return (CFTypeRef)(uintptr_t)g;  // a raw value stored in a CF container
}

static u32 val_to_guest(const void *p) {
    if ((uintptr_t)p <= 0xFFFFFFFFu) return (u32)(uintptr_t)p;
    return cf_wrap(p);
}

#define O(n) cf_unwrap(ARG(n))
#define S(n) ((CFStringRef)cf_unwrap(ARG(n)))
#define U(n) ((CFURLRef)cf_unwrap(ARG(n)))
#define D(n) ((CFMutableDictionaryRef)cf_unwrap(ARG(n)))
#define A(n) ((CFMutableArrayRef)cf_unwrap(ARG(n)))

SHIM(CFRetain) {
    if (ARG(0) >= cfstring_lo && ARG(0) < cfstring_hi) {
        RET(ARG(0));
        return;
    }
    CFRetain(O(0));
    RET(ARG(0));
}
SHIM(CFRelease) {
    u32 g = ARG(0);
    if (g >= cfstring_lo && g < cfstring_hi) return;
    CFTypeRef o = cf_unwrap(g);
    if (!o) return;
    if (CFGetRetainCount(o) == 1) {
        CFDictionaryRemoveValue(obj_to_handle, o);
        rt_handle_free(g);
    }
    CFRelease(o);
}
SHIM(CFGetTypeID) { RET(CFGetTypeID(O(0))); }
SHIM(CFEqual) { RET(CFEqual(O(0), O(1))); }
SHIM(CFHash) { RET((u32)CFHash(O(0))); }
SHIM(CFShow) { CFShow(O(0)); }
SHIM(CFCopyDescription) { RET(cf_wrap(CFCopyDescription(O(0)))); }
SHIM(CFAllocatorCreate) { RET(0); }
SHIM(CFAllocatorGetDefault) { RET(0); }
#define TYPEID(n) SHIM(n) { RET(n()); }
TYPEID(CFStringGetTypeID) TYPEID(CFArrayGetTypeID) TYPEID(CFDictionaryGetTypeID) TYPEID(CFNumberGetTypeID)
TYPEID(CFBooleanGetTypeID) TYPEID(CFDataGetTypeID) TYPEID(CFDateGetTypeID) TYPEID(CFURLGetTypeID)

// ---------- strings ----------
SHIM(CFStringCreateWithCString) { RET(cf_wrap(CFStringCreateWithCString(NULL, GS(ARG(1)), ARG(2)))); }
SHIM(CFStringCreateWithBytes) { RET(cf_wrap(CFStringCreateWithBytes(NULL, GP(ARG(1)), (i32)ARG(2), ARG(3), (Boolean)ARG(4)))); }
SHIM(CFStringCreateWithBytesNoCopy) { RET(cf_wrap(CFStringCreateWithBytes(NULL, GP(ARG(1)), (i32)ARG(2), ARG(3), (Boolean)ARG(4)))); }
SHIM(CFStringCreateWithCharacters) { RET(cf_wrap(CFStringCreateWithCharacters(NULL, GP(ARG(1)), (i32)ARG(2)))); }
SHIM(CFStringCreateWithFileSystemRepresentation) { RET(cf_wrap(CFStringCreateWithFileSystemRepresentation(NULL, GS(ARG(1))))); }
SHIM(CFStringCreateCopy) { RET(cf_wrap(CFStringCreateCopy(NULL, S(1)))); }
SHIM(CFStringCreateMutable) { RET(cf_wrap(CFStringCreateMutable(NULL, (i32)ARG(1)))); }
SHIM(CFStringCreateMutableCopy) { RET(cf_wrap(CFStringCreateMutableCopy(NULL, (i32)ARG(1), S(2)))); }
SHIM(CFStringCreateWithSubstring) {
    i32 loc = (i32)ARG(2), len = (i32)ARG(3);
    CFStringRef str = S(1);
    if (loc < 0 || len < 0 || loc + len > CFStringGetLength(str)) {
        char b[600] = "";
        CFStringGetCString(str, b, sizeof b, kCFStringEncodingUTF8);
        fprintf(stderr, "[cf] bad substring range loc=%d len=%d of length %ld: \"%s\"\n", loc, len, (long)CFStringGetLength(str), b);
    }
    RET(cf_wrap(CFStringCreateWithSubstring(NULL, str, CFRangeMake(loc, len))));
}
SHIM(CFStringGetLength) { RET(CFStringGetLength(S(0))); }
SHIM(CFStringGetCString) { RET(CFStringGetCString(S(0), GP(ARG(1)), (i32)ARG(2), ARG(3))); }
SHIM(CFStringGetCharacters) { CFStringGetCharacters(S(0), CFRangeMake((i32)ARG(1), (i32)ARG(2)), GP(ARG(3))); }
SHIM(CFStringGetCharacterAtIndex) { RET(CFStringGetCharacterAtIndex(S(0), (i32)ARG(1))); }
SHIM(CFStringGetFileSystemRepresentation) { RET(CFStringGetFileSystemRepresentation(S(0), GP(ARG(1)), (i32)ARG(2))); }
SHIM(CFStringGetMaximumSizeForEncoding) { RET(CFStringGetMaximumSizeForEncoding((i32)ARG(0), ARG(1))); }
SHIM(CFStringGetMaximumSizeOfFileSystemRepresentation) { RET(CFStringGetMaximumSizeOfFileSystemRepresentation(S(0))); }
SHIM(CFStringGetSystemEncoding) { RET(CFStringGetSystemEncoding()); }
SHIM(CFStringGetIntValue) { RET(CFStringGetIntValue(S(0))); }
SHIM(CFStringGetDoubleValue) { RETF(CFStringGetDoubleValue(S(0))); }
SHIM(CFStringConvertWindowsCodepageToEncoding) { RET(CFStringConvertWindowsCodepageToEncoding(ARG(0))); }
SHIM(CFStringConvertIANACharSetNameToEncoding) { RET(CFStringConvertIANACharSetNameToEncoding(S(0))); }
SHIM(CFStringCompare) { RET(CFStringCompare(S(0), S(1), ARG(2))); }
SHIM(CFStringCompareWithOptions) { RET(CFStringCompareWithOptions(S(0), S(1), CFRangeMake((i32)ARG(2), (i32)ARG(3)), ARG(4))); }
SHIM(CFStringAppend) { CFStringAppend((CFMutableStringRef)S(0), S(1)); }
SHIM(CFStringInsert) { CFStringInsert((CFMutableStringRef)S(0), (i32)ARG(1), S(2)); }
SHIM(CFStringDelete) { CFStringDelete((CFMutableStringRef)S(0), CFRangeMake((i32)ARG(1), (i32)ARG(2))); }
SHIM(CFStringReplace) { CFStringReplace((CFMutableStringRef)S(0), CFRangeMake((i32)ARG(1), (i32)ARG(2)), S(3)); }
SHIM(CFStringLowercase) { CFStringLowercase((CFMutableStringRef)S(0), NULL); }
SHIM(CFStringUppercase) { CFStringUppercase((CFMutableStringRef)S(0), NULL); }
SHIM(CFStringFindWithOptions) {
    CFRange r;
    Boolean ok = CFStringFindWithOptions(S(0), S(1), CFRangeMake((i32)ARG(2), (i32)ARG(3)), ARG(4), &r);
    if (ok && ARG(5)) {
        G32(ARG(5)) = (u32)r.location;
        G32(ARG(5) + 4) = (u32)r.length;
    }
    RET(ok);
}
extern char *rt_format_guest(const char *fmt, u32 va);
static const char *cf_cstr(CFStringRef s, char *b, size_t n) {
    if (!CFStringGetCString(s, b, (CFIndex)n, kCFStringEncodingUTF8)) b[0] = 0;
    return b;
}
SHIM(CFStringCreateWithFormat) {
    static char b[8192];
    char *z = rt_format_guest(cf_cstr(S(2), b, sizeof b), c->esp + 4 + 3 * 4);
    RET(cf_wrap(CFStringCreateWithCString(NULL, z, kCFStringEncodingUTF8)));
    free(z);
}
SHIM(CFStringAppendFormat) {
    static char b[8192];
    char *z = rt_format_guest(cf_cstr(S(2), b, sizeof b), c->esp + 4 + 3 * 4);
    CFStringAppendCString((CFMutableStringRef)S(0), z, kCFStringEncodingUTF8);
    free(z);
}
SHIM(CFStringFindAndReplace) {
    RET(CFStringFindAndReplace((CFMutableStringRef)S(0), S(1), S(2), CFRangeMake((i32)ARG(3), (i32)ARG(4)), ARG(5)));
}
SHIM(CFStringGetBytes) {
    CFIndex used = 0;
    CFIndex n = CFStringGetBytes(S(0), CFRangeMake((i32)ARG(1), (i32)ARG(2)), ARG(3), (UInt8)ARG(4), (Boolean)ARG(5), NP(ARG(6)), (i32)ARG(7), &used);
    if (ARG(8)) G32(ARG(8)) = (u32)used;
    RET(n);
}
SHIM(CFStringCreateExternalRepresentation) { RET(cf_wrap(CFStringCreateExternalRepresentation(NULL, S(1), ARG(2), (UInt8)ARG(3)))); }
SHIM(CFStringCreateArrayBySeparatingStrings) { RET(cf_wrap(CFStringCreateArrayBySeparatingStrings(NULL, S(1), S(2)))); }

// ---------- URLs ----------
SHIM(CFURLCreateWithFileSystemPath) { RET(cf_wrap(CFURLCreateWithFileSystemPath(NULL, S(1), ARG(2), (Boolean)ARG(3)))); }
SHIM(CFURLCreateWithFileSystemPathRelativeToBase) { RET(cf_wrap(CFURLCreateWithFileSystemPathRelativeToBase(NULL, S(1), ARG(2), (Boolean)ARG(3), U(4)))); }
SHIM(CFURLCreateFromFileSystemRepresentation) { RET(cf_wrap(CFURLCreateFromFileSystemRepresentation(NULL, GP(ARG(1)), (i32)ARG(2), (Boolean)ARG(3)))); }
SHIM(CFURLCreateWithString) { RET(cf_wrap(CFURLCreateWithString(NULL, S(1), U(2)))); }
SHIM(CFURLCreateWithBytes) { RET(cf_wrap(CFURLCreateWithBytes(NULL, GP(ARG(1)), (i32)ARG(2), ARG(3), U(4)))); }
SHIM(CFURLCopyAbsoluteURL) { RET(cf_wrap(CFURLCopyAbsoluteURL(U(0)))); }
SHIM(CFURLCopyFileSystemPath) { RET(cf_wrap(CFURLCopyFileSystemPath(U(0), ARG(1)))); }
SHIM(CFURLCopyLastPathComponent) { RET(cf_wrap(CFURLCopyLastPathComponent(U(0)))); }
SHIM(CFURLCopyScheme) { RET(cf_wrap(CFURLCopyScheme(U(0)))); }
SHIM(CFURLCopyHostName) { RET(cf_wrap(CFURLCopyHostName(U(0)))); }
SHIM(CFURLGetString) { RET(cf_wrap(CFURLGetString(U(0)))); }
SHIM(CFURLHasDirectoryPath) { RET(CFURLHasDirectoryPath(U(0))); }
SHIM(CFURLGetFileSystemRepresentation) { RET(CFURLGetFileSystemRepresentation(U(0), (Boolean)ARG(1), GP(ARG(2)), (i32)ARG(3))); }
SHIM(CFURLCreateCopyAppendingPathComponent) { RET(cf_wrap(CFURLCreateCopyAppendingPathComponent(NULL, U(1), S(2), (Boolean)ARG(3)))); }
SHIM(CFURLCreateCopyDeletingLastPathComponent) { RET(cf_wrap(CFURLCreateCopyDeletingLastPathComponent(NULL, U(1)))); }
SHIM(CFURLCreateCopyDeletingPathExtension) { RET(cf_wrap(CFURLCreateCopyDeletingPathExtension(NULL, U(1)))); }
SHIM(CFURLCreateFromFSRef) {
    FSRef ref;
    memcpy(&ref, GP(ARG(1)), sizeof ref);
    RET(cf_wrap(CFURLCreateFromFSRef(NULL, &ref)));
}
SHIM(CFURLGetFSRef) {
    FSRef ref;
    Boolean ok = CFURLGetFSRef(U(0), &ref);
    if (ok) memcpy(GP(ARG(1)), &ref, sizeof ref);
    RET(ok);
}
SHIM(FSFindFolder) {
    FSRef ref;
    OSErr e = FSFindFolder((FSVolumeRefNum)(i16)ARG(0), ARG(1), (Boolean)ARG(2), &ref);
    if (!e) memcpy(GP(ARG(3)), &ref, sizeof ref);
    RET((i32)e);
}
SHIM(CFURLCreateStringByAddingPercentEscapes) {
    RET(cf_wrap(CFURLCreateStringByAddingPercentEscapes(NULL, S(1), S(2), S(3), ARG(4))));
}
SHIM(CFURLCreateStringByReplacingPercentEscapes) { RET(cf_wrap(CFURLCreateStringByReplacingPercentEscapes(NULL, S(1), S(2)))); }
SHIM(FSResolveAliasFile) {
    FSRef ref;
    Boolean isdir = 0, was = 0;
    memcpy(&ref, GP(ARG(0)), sizeof ref);
    OSErr e = FSResolveAliasFile(&ref, (Boolean)ARG(1), &isdir, &was);
    memcpy(GP(ARG(0)), &ref, sizeof ref);
    if (ARG(2)) G8(ARG(2)) = isdir;
    if (ARG(3)) G8(ARG(3)) = was;
    RET((i32)e);
}
SHIM(FSPathMakeRef) {
    FSRef ref;
    Boolean isdir = 0;
    OSStatus e = FSPathMakeRef(GP(ARG(0)), &ref, &isdir);
    if (!e) memcpy(GP(ARG(1)), &ref, sizeof ref);
    if (ARG(2)) G8(ARG(2)) = isdir;
    RET(e);
}

// Classic file APIs: FSRef and FSCatalogInfo keep their layout across architectures (they are packed structs).
SHIM(FSCompareFSRefs) { RET((i32)FSCompareFSRefs(GP(ARG(0)), GP(ARG(1)))); }
// FSCatalogInfo differs between 32 and 64 bits: permissions end in a pointer, so every later field sits 4 bytes
// lower in the guest's layout. The packed struct is otherwise identical.
static void catinfo_to_guest(u32 g, const FSCatalogInfo *h) {
    size_t perm = offsetof(FSCatalogInfo, permissions);
    size_t after = perm + sizeof(h->permissions);
    memcpy(GP(g), h, perm + 12);
    G32(g + (u32)perm + 12) = 0;
    memcpy(GP(g + (u32)perm + 16), (const u8 *)h + after, sizeof *h - after);
}
SHIM(FSGetCatalogInfo) {
    FSRef ref;
    memcpy(&ref, GP(ARG(0)), sizeof ref);
    FSCatalogInfo info;
    memset(&info, 0, sizeof info);
    u32 which = ARG(1);
    OSErr e = FSGetCatalogInfo(&ref, which, ARG(2) ? &info : NULL, NP(ARG(3)), NULL, NP(ARG(5)));
    if (!e && ARG(2)) {
        if ((which & kFSCatInfoValence) && (info.nodeFlags & kFSNodeIsDirectoryMask) && info.valence == 0) {
            // Modern volumes don't report item counts here; count the entries the way HFS+ did.
            UInt8 path[2048];
            if (FSRefMakePath(&ref, path, sizeof path) == 0) {
                DIR *d = opendir((const char *)path);
                if (d) {
                    struct dirent *de;
                    while ((de = readdir(d))) {
                        if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) info.valence++;
                    }
                    closedir(d);
                }
            }
        }
        catinfo_to_guest(ARG(2), &info);
    }
    RET((i32)e);
}
SHIM(FSGetVolumeInfo) {
    RET((i32)FSGetVolumeInfo((FSVolumeRefNum)(i16)ARG(0), (ItemCount)ARG(1), NP(ARG(2)), ARG(3), NP(ARG(4)), NP(ARG(5)), NP(ARG(6))));
}
SHIM(FSIsAliasFile) {
    FSRef ref;
    memcpy(&ref, GP(ARG(0)), sizeof ref);
    Boolean a = 0, f = 0;
    OSErr e = FSIsAliasFile(&ref, &a, &f);
    if (ARG(1)) G8(ARG(1)) = a;
    if (ARG(2)) G8(ARG(2)) = f;
    RET((i32)e);
}
SHIM(UpTime) { UInt64 t = mach_absolute_time(); RET64(t); }
SHIM(AbsoluteToDuration) { RET((u32)(ARGQ(0) / 1000000ull)); }
SHIM(SecRandomCopyBytes) { arc4random_buf(GP(ARG(2)), ARG(1)); RET(0); }

// Classic process APIs answer as if this process were the game's bundle.
SHIM(GetCurrentProcess) { G32(ARG(0)) = 0; G32(ARG(0) + 4) = 2; RET(0); }
SHIM(GetProcessBundleLocation) {
    char path[1200];
    snprintf(path, sizeof path,
             "%s/Library/Application Support/Steam/steamapps/common/Batman Arkham Asylum GOTY/Batman Arkham Asylum GOTY.app",
             getenv("HOME"));
    FSRef ref;
    OSStatus e = FSPathMakeRef((const UInt8 *)path, &ref, NULL);
    if (!e) memcpy(GP(ARG(1)), &ref, sizeof ref);
    RET(e);
}

// ---------- the game's bundle ----------
static CFBundleRef main_bundle(void) {
    static CFBundleRef b;
    if (!b) {
        char path[1200];
        snprintf(path, sizeof path,
                 "%s/Library/Application Support/Steam/steamapps/common/Batman Arkham Asylum GOTY/Batman Arkham Asylum GOTY.app",
                 getenv("HOME"));
        CFURLRef u = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), true);
        b = CFBundleCreate(NULL, u);
        CFRelease(u);
    }
    return b;
}
#define B(n) ((CFBundleRef)cf_unwrap(ARG(n)))
SHIM(CFBundleGetMainBundle) { RET(cf_wrap(main_bundle())); }
SHIM(CFBundleGetBundleWithIdentifier) { RET(cf_wrap(CFBundleGetBundleWithIdentifier(S(0)))); }
SHIM(CFBundleCreate) { RET(cf_wrap(CFBundleCreate(NULL, U(1)))); }
SHIM(CFBundleCopyBundleURL) { RET(cf_wrap(CFBundleCopyBundleURL(B(0)))); }
SHIM(CFBundleCopyResourcesDirectoryURL) { RET(cf_wrap(CFBundleCopyResourcesDirectoryURL(B(0)))); }
SHIM(CFBundleCopyPrivateFrameworksURL) { RET(cf_wrap(CFBundleCopyPrivateFrameworksURL(B(0)))); }
SHIM(CFBundleCopyResourceURL) { RET(cf_wrap(CFBundleCopyResourceURL(B(0), S(1), S(2), S(3)))); }
SHIM(CFBundleCopyResourceURLsOfType) { RET(cf_wrap(CFBundleCopyResourceURLsOfType(B(0), S(1), S(2)))); }
SHIM(CFBundleGetInfoDictionary) { RET(cf_wrap(CFBundleGetInfoDictionary(B(0)))); }
SHIM(CFBundleGetValueForInfoDictionaryKey) { RET(cf_wrap(CFBundleGetValueForInfoDictionaryKey(B(0), S(1)))); }
SHIM(CFBundleCopyBundleLocalizations) { RET(cf_wrap(CFBundleCopyBundleLocalizations(B(0)))); }
// With ARKHAM_LANGUAGE the game's own windows are told they already show that language. Feral's code compares the two and otherwise asks to be relaunched
// at every start, because its windows only come in English, French, Italian, German and Spanish; for any other language they stay in English.
SHIM(CFBundleCopyPreferredLocalizationsFromArray) {
    const char *lang = getenv("ARKHAM_LANGUAGE");
    if (lang && *lang) {
        CFStringRef s = CFStringCreateWithCString(NULL, lang, kCFStringEncodingUTF8);
        CFArrayRef a = CFArrayCreate(NULL, (const void **)&s, 1, &kCFTypeArrayCallBacks);
        CFRelease(s);
        RET(cf_wrap(a));
        return;
    }
    RET(cf_wrap(CFBundleCopyPreferredLocalizationsFromArray((CFArrayRef)O(0))));
}
SHIM(CFBundleCopyLocalizationsForPreferences) { RET(cf_wrap(CFBundleCopyLocalizationsForPreferences((CFArrayRef)O(0), (CFArrayRef)O(1)))); }
SHIM(CFBundleCopyLocalizedString) { RET(cf_wrap(CFBundleCopyLocalizedString(B(0), S(1), S(2), S(3)))); }
// The game calls some system functions by looking them up by name, free among them. A failed lookup quietly turns such a
// call into nothing, so every answer comes from the same shims the game's imports use.
void (*rt_lookup_shim(const char *sym))(CPU *);
u32 rt_dyn_import(const char *name, void (*fn)(CPU *));
SHIM(CFBundleGetFunctionPointerForName) {
    char name[256] = "_";
    CFStringRef s = S(1);
    if (!s || !CFStringGetCString(s, name + 1, sizeof name - 1, kCFStringEncodingUTF8)) {
        RET(0);
        return;
    }
    void (*fn)(CPU *) = rt_lookup_shim(name);
    if (!fn) {
        fprintf(stderr, "[cf] no implementation for function looked up by name: %s\n", name + 1);
        RET(0);
        return;
    }
    RET(rt_dyn_import(name, fn));
}
SHIM(CFBundleIsExecutableLoaded) { RET(1); }
SHIM(CFBundleLoadExecutable) { RET(1); }

// ---------- containers ----------
static const void *cb_or_null(u32 g, u32 known, const void *host) { return g == known ? host : NULL; }
static u32 a_cb, d_kcb, d_vcb;
SHIM(CFArrayCreateMutable) {
    RET(cf_wrap(CFArrayCreateMutable(NULL, (i32)ARG(1), cb_or_null(ARG(2), a_cb, &kCFTypeArrayCallBacks))));
}
SHIM(CFArrayCreate) {
    i32 n = (i32)ARG(2);
    const void **v = calloc((size_t)n + 1, sizeof(void *));
    for (i32 i = 0; i < n; i++) v[i] = cf_unwrap(G32(ARG(1) + (u32)i * 4));
    CFArrayRef a = CFArrayCreate(NULL, v, n, cb_or_null(ARG(3), a_cb, &kCFTypeArrayCallBacks));
    free(v);
    RET(cf_wrap(a));
}
SHIM(CFArrayCreateCopy) { RET(cf_wrap(CFArrayCreateCopy(NULL, (CFArrayRef)O(1)))); }
SHIM(CFArrayGetCount) { RET(CFArrayGetCount((CFArrayRef)O(0))); }
SHIM(CFArrayGetValueAtIndex) { RET(val_to_guest(CFArrayGetValueAtIndex((CFArrayRef)O(0), (i32)ARG(1)))); }
SHIM(CFArrayAppendValue) { CFArrayAppendValue(A(0), O(1)); }
SHIM(CFArrayInsertValueAtIndex) { CFArrayInsertValueAtIndex(A(0), (i32)ARG(1), O(2)); }
SHIM(CFArrayRemoveValueAtIndex) { CFArrayRemoveValueAtIndex(A(0), (i32)ARG(1)); }
SHIM(CFArrayRemoveAllValues) { CFArrayRemoveAllValues(A(0)); }
SHIM(CFArrayApplyFunction) {
    CFArrayRef a = (CFArrayRef)O(0);
    u32 loc = ARG(1), len = ARG(2), fn = ARG(3), ctx = ARG(4);
    for (u32 i = 0; i < len; i++) {
        u32 args[2] = {val_to_guest(CFArrayGetValueAtIndex(a, (CFIndex)(loc + i))), ctx};
        rt_call_guest(c, fn, 2, args);
    }
}
SHIM(CFDictionaryCreateMutable) {
    RET(cf_wrap(CFDictionaryCreateMutable(NULL, (i32)ARG(1), cb_or_null(ARG(2), d_kcb, &kCFTypeDictionaryKeyCallBacks),
                                          cb_or_null(ARG(3), d_vcb, &kCFTypeDictionaryValueCallBacks))));
}
SHIM(CFDictionaryCreate) {
    i32 n = (i32)ARG(3);
    const void **k = calloc((size_t)n + 1, sizeof(void *)), **v = calloc((size_t)n + 1, sizeof(void *));
    for (i32 i = 0; i < n; i++) {
        k[i] = cf_unwrap(G32(ARG(1) + (u32)i * 4));
        v[i] = cf_unwrap(G32(ARG(2) + (u32)i * 4));
    }
    CFDictionaryRef d = CFDictionaryCreate(NULL, k, v, n, cb_or_null(ARG(4), d_kcb, &kCFTypeDictionaryKeyCallBacks),
                                           cb_or_null(ARG(5), d_vcb, &kCFTypeDictionaryValueCallBacks));
    free(k);
    free(v);
    RET(cf_wrap(d));
}
SHIM(CFDictionaryCreateCopy) { RET(cf_wrap(CFDictionaryCreateCopy(NULL, (CFDictionaryRef)O(1)))); }
SHIM(CFDictionaryCreateMutableCopy) { RET(cf_wrap(CFDictionaryCreateMutableCopy(NULL, (i32)ARG(1), (CFDictionaryRef)O(2)))); }
SHIM(CFDictionaryGetCount) { RET(CFDictionaryGetCount((CFDictionaryRef)O(0))); }
SHIM(CFDictionaryGetValue) { RET(val_to_guest(CFDictionaryGetValue((CFDictionaryRef)O(0), O(1)))); }
SHIM(CFDictionaryGetValueIfPresent) {
    const void *v = NULL;
    Boolean ok = CFDictionaryGetValueIfPresent((CFDictionaryRef)O(0), O(1), &v);
    if (ok && ARG(2)) G32(ARG(2)) = val_to_guest(v);
    RET(ok);
}
SHIM(CFDictionaryGetCountOfKey) { RET(CFDictionaryGetCountOfKey((CFDictionaryRef)O(0), O(1))); }
SHIM(CFDictionarySetValue) { CFDictionarySetValue(D(0), O(1), O(2)); }
SHIM(CFDictionaryAddValue) { CFDictionaryAddValue(D(0), O(1), O(2)); }
SHIM(CFDictionaryGetKeysAndValues) {
    CFDictionaryRef d = (CFDictionaryRef)O(0);
    CFIndex n = CFDictionaryGetCount(d);
    const void **k = calloc((size_t)n + 1, sizeof(void *)), **v = calloc((size_t)n + 1, sizeof(void *));
    CFDictionaryGetKeysAndValues(d, k, v);
    for (CFIndex i = 0; i < n; i++) {
        if (ARG(1)) G32(ARG(1) + (u32)i * 4) = val_to_guest(k[i]);
        if (ARG(2)) G32(ARG(2) + (u32)i * 4) = val_to_guest(v[i]);
    }
    free(k);
    free(v);
}

SHIM(CFDictionaryApplyFunction) {
    CFDictionaryRef d = (CFDictionaryRef)O(0);
    u32 fn = ARG(1), ctx = ARG(2);
    CFIndex n = CFDictionaryGetCount(d);
    const void **k = calloc((size_t)n + 1, sizeof(void *)), **v = calloc((size_t)n + 1, sizeof(void *));
    CFDictionaryGetKeysAndValues(d, k, v);
    for (CFIndex i = 0; i < n; i++) {
        u32 a[3] = {val_to_guest(k[i]), val_to_guest(v[i]), ctx};
        rt_call_guest(c, fn, 3, a);
    }
    free(k);
    free(v);
}

// ---------- numbers, booleans, data ----------
SHIM(CFBooleanGetValue) { RET(CFBooleanGetValue((CFBooleanRef)O(0))); }
static CFNumberType num_type(u32 t) {
    switch (t) {
        case kCFNumberLongType: case kCFNumberCFIndexType: case kCFNumberNSIntegerType: return kCFNumberSInt32Type;
        case kCFNumberCGFloatType: return kCFNumberFloat32Type;
        default: return (CFNumberType)t;
    }
}
SHIM(CFNumberCreate) { RET(cf_wrap(CFNumberCreate(NULL, num_type(ARG(1)), GP(ARG(2))))); }
SHIM(CFNumberGetValue) { RET(CFNumberGetValue((CFNumberRef)O(0), num_type(ARG(1)), GP(ARG(2)))); }
SHIM(CFDataCreate) { RET(cf_wrap(CFDataCreate(NULL, GP(ARG(1)), (i32)ARG(2)))); }
SHIM(CFDataCreateWithBytesNoCopy) { RET(cf_wrap(CFDataCreate(NULL, GP(ARG(1)), (i32)ARG(2)))); }
SHIM(CFDataCreateMutable) { RET(cf_wrap(CFDataCreateMutable(NULL, (i32)ARG(1)))); }
SHIM(CFDataGetLength) { RET(CFDataGetLength((CFDataRef)O(0))); }
SHIM(CFDataGetBytes) { CFDataGetBytes((CFDataRef)O(0), CFRangeMake((i32)ARG(1), (i32)ARG(2)), GP(ARG(3))); }
// Host-owned bytes are copied into a guest buffer that stays alive for the object's lifetime in practice.
// The game does arithmetic between pointers from separate calls, so one data object must always give the same address.
// Recent objects stay retained, which keeps their addresses from being reused while the guest may still hold the pointer.
typedef struct { CFDataRef d; u32 g; CFIndex n; } DataCopy;
#define DATA_COPIES 256
static DataCopy data_copies[DATA_COPIES];
static unsigned data_next;
SHIM(CFDataGetBytePtr) {
    CFDataRef d = (CFDataRef)O(0);
    CFIndex n = CFDataGetLength(d);
    for (unsigned i = 0; i < DATA_COPIES; i++) {
        DataCopy *e = &data_copies[i];
        if (e->d == d && e->n == n) {
            memcpy(GP(e->g), CFDataGetBytePtr(d), (size_t)n);
            RET(e->g);
            return;
        }
    }
    u32 g = rt_guest_malloc((u32)n + 1);
    memcpy(GP(g), CFDataGetBytePtr(d), (size_t)n);
    DataCopy *e = &data_copies[data_next++ % DATA_COPIES];
    if (e->d) CFRelease(e->d);
    e->d = CFRetain(d);
    e->g = g;
    e->n = n;
    RET(g);
}

// CFGregorianDate comes back through a hidden pointer that the callee pops: {year, month, day, hour, minute, second}.
SHIM(CFAbsoluteTimeGetDayOfWeek) { RET(CFAbsoluteTimeGetDayOfWeek(ARGD(0), (CFTimeZoneRef)O(2))); }
SHIM(CFAbsoluteTimeGetGregorianDate) {
    u32 buf = ARG(0);
    CFGregorianDate g = CFAbsoluteTimeGetGregorianDate(ARGD(1), (CFTimeZoneRef)O(3));
    G32(buf) = (u32)g.year;
    G8(buf + 4) = (u8)g.month;
    G8(buf + 5) = (u8)g.day;
    G8(buf + 6) = (u8)g.hour;
    G8(buf + 7) = (u8)g.minute;
    *(f64a *)GP(buf + 8) = g.second;
    RET(buf);
    c->esp += 4;
}
SHIM(CFGregorianDateGetAbsoluteTime) {
    CFGregorianDate g;
    g.year = (SInt32)ARG(0);
    u32 w = ARG(1);
    g.month = (SInt8)w;
    g.day = (SInt8)(w >> 8);
    g.hour = (SInt8)(w >> 16);
    g.minute = (SInt8)(w >> 24);
    g.second = ARGD(2);
    RETF(CFGregorianDateGetAbsoluteTime(g, (CFTimeZoneRef)O(4)));
}
SHIM(UCConvertCFAbsoluteTimeToLongDateTime) {
    LongDateTime v = 0;
    OSStatus e = UCConvertCFAbsoluteTimeToLongDateTime(ARGD(0), &v);
    *(u64a *)GP(ARG(2)) = (u64)v;
    RET(e);
}
SHIM(UCConvertLongDateTimeToCFAbsoluteTime) {
    CFAbsoluteTime v = 0;
    OSStatus e = UCConvertLongDateTimeToCFAbsoluteTime((LongDateTime)ARGQ(0), &v);
    *(f64a *)GP(ARG(2)) = v;
    RET(e);
}
SHIM(CFPropertyListCreateFromXMLData) {
    CFStringRef err = NULL;
    CFPropertyListRef pl = CFPropertyListCreateFromXMLData(NULL, (CFDataRef)O(1), ARG(2), &err);
    if (ARG(3)) G32(ARG(3)) = cf_wrap(err);
    RET(cf_wrap(pl));
}

// ---------- time ----------
SHIM(CFAbsoluteTimeGetCurrent) { RETF(CFAbsoluteTimeGetCurrent()); }
SHIM(CFTimeZoneCopyDefault) { RET(cf_wrap(CFTimeZoneCopyDefault())); }
SHIM(CFTimeZoneCopySystem) { RET(cf_wrap(CFTimeZoneCopySystem())); }
SHIM(CFTimeZoneGetSecondsFromGMT) { RETF(CFTimeZoneGetSecondsFromGMT((CFTimeZoneRef)O(0), ARGD(1))); }
SHIM(CFTimeZoneIsDaylightSavingTime) { RET(CFTimeZoneIsDaylightSavingTime((CFTimeZoneRef)O(0), ARGD(1))); }
SHIM(LSCopyDisplayNameForURL) {
    CFStringRef out = NULL;
    OSStatus e = LSCopyDisplayNameForURL(U(0), &out);
    if (!e && ARG(1)) G32(ARG(1)) = cf_wrap(out);
    RET(e);
}
SHIM(CFBundleCopyAuxiliaryExecutableURL) { RET(cf_wrap(CFBundleCopyAuxiliaryExecutableURL((CFBundleRef)O(0), S(1)))); }
// Run loops are pumped by the game itself through AppKit; timers registered here don't fire yet.
SHIM(CFRunLoopGetMain) { RET(cf_wrap(CFRunLoopGetMain())); }
SHIM(CFRunLoopGetCurrent) { RET(cf_wrap(CFRunLoopGetCurrent())); }
// Real run loops and timers; callouts re-enter guest code holding the gate.
typedef struct { u32 callout, info, handle; } TimerCtx;
static void timer_cb(CFRunLoopTimerRef t, void *p) {
    (void)t;
    TimerCtx *x = p;
    int held = rt_gate_held();
    if (!held) rt_gate_acquire();
    CPU *c = rt_current_cpu();
    if (!c) c = rt_new_cpu(2u << 20);
    u32 a[2] = {x->handle, x->info};
    rt_call_guest(c, x->callout, 2, a);
    if (!held) rt_gate_release();
}
SHIM(CFRunLoopTimerCreate) {
    // alloc, fireDate(double), interval(double), flags, order, callout, context
    TimerCtx *x = calloc(1, sizeof *x);
    x->callout = ARG(7);
    x->info = ARG(8) ? G32(ARG(8) + 4) : 0;
    CFRunLoopTimerContext ctx = {0, x, NULL, NULL, NULL};
    CFRunLoopTimerRef t = CFRunLoopTimerCreate(NULL, ARGD(1), ARGD(3), ARG(5), (CFIndex)(i32)ARG(6), timer_cb, &ctx);
    x->handle = cf_wrap(t);
    RET(x->handle);
}
SHIM(CFRunLoopAddTimer) { CFRunLoopAddTimer((CFRunLoopRef)O(0), (CFRunLoopTimerRef)O(1), S(2)); }
SHIM(CFRunLoopRemoveTimer) { CFRunLoopRemoveTimer((CFRunLoopRef)O(0), (CFRunLoopTimerRef)O(1), S(2)); }
SHIM(CFRunLoopTimerInvalidate) { CFRunLoopTimerInvalidate((CFRunLoopTimerRef)O(0)); }
SHIM(CFRunLoopTimerSetNextFireDate) { CFRunLoopTimerSetNextFireDate((CFRunLoopTimerRef)O(0), ARGD(1)); }
SHIM(CFRunLoopTimerGetContext) {
    CFRunLoopTimerContext ctx;
    CFRunLoopTimerGetContext((CFRunLoopTimerRef)O(0), &ctx);
    if (ARG(1)) G32(ARG(1) + 4) = ((TimerCtx *)ctx.info)->info;
}
SHIM(CFRunLoopRun) {
    rt_gate_release();
    CFRunLoopRun();
    rt_gate_acquire();
}
SHIM(CFRunLoopRunInMode) {
    CFStringRef mode = S(0);
    double secs = ARGD(1);
    Boolean once = (Boolean)ARG(3);
    rt_gate_release();
    i32 r = CFRunLoopRunInMode(mode, secs, once);
    rt_gate_acquire();
    RET(r);
}
SHIM(CFRunLoopStop) { CFRunLoopStop((CFRunLoopRef)O(0)); }
SHIM(CFRunLoopWakeUp) { CFRunLoopWakeUp((CFRunLoopRef)O(0)); }
SHIM(CFRunLoopGetNextTimerFireDate) { RETF(CFRunLoopGetNextTimerFireDate((CFRunLoopRef)O(0), S(1))); }

// ---------- no networking: HTTP requests can be built, but every stream reports a connection error ----------
typedef struct { u32 stream; u32 cb; u32 info; u32 mask; int pending; } NetStream;
static NetStream netstreams[64];
static NetStream *net_find(u32 h, int create) {
    for (int i = 0; i < 64; i++)
        if (netstreams[i].stream == h) return &netstreams[i];
    if (create)
        for (int i = 0; i < 64; i++)
            if (!netstreams[i].stream) {
                netstreams[i].stream = h;
                return &netstreams[i];
            }
    return NULL;
}
// The answer arrives from the run loop, as a real network reply would.
static void net_answer(NetStream *n) {
    CFRunLoopTimerRef t = CFRunLoopTimerCreateWithHandler(NULL, CFAbsoluteTimeGetCurrent() + 0.001, 0, 0, 0, ^(CFRunLoopTimerRef tt) {
        (void)tt;
        if (!n->stream || !n->pending || !n->cb) return;
        n->pending = 0;
        int held = rt_gate_held();
        if (!held) rt_gate_acquire();
        // A definitive "404 Not Found" rather than a connection error, which the updater keeps retrying.
        u32 a[3] = {n->stream, 16 /* kCFStreamEventEndEncountered */, n->info};
        if (getenv("ARKHAM_TRACE_FS")) fprintf(stderr, "[net] answering 404 locally on stream %08x\n", n->stream);
        rt_call_guest(rt_current_cpu(), n->cb, 3, a);
        if (!held) rt_gate_release();
    });
    CFRunLoopAddTimer(CFRunLoopGetMain(), t, kCFRunLoopCommonModes);
    CFRelease(t);
}
SHIM(CFHTTPMessageCreateRequest) { RET(cf_wrap(CFHTTPMessageCreateRequest(NULL, S(1), U(2), S(3)))); }
SHIM(CFHTTPMessageSetHeaderFieldValue) { CFHTTPMessageSetHeaderFieldValue((CFHTTPMessageRef)O(0), S(1), S(2)); }
SHIM(CFHTTPMessageCreateEmpty) { RET(cf_wrap(CFHTTPMessageCreateEmpty(NULL, (Boolean)ARG(1)))); }
SHIM(CFHTTPMessageSetBody) { CFHTTPMessageSetBody((CFHTTPMessageRef)O(0), (CFDataRef)O(1)); }
SHIM(CFReadStreamCreateForHTTPRequest) {
    u8 z = 0;
    u32 h = cf_wrap(CFDataCreate(NULL, &z, 1));  // an ordinary CF object standing in for the stream
    net_find(h, 1);
    RET(h);
}
SHIM(CFReadStreamSetClient) {
    NetStream *n = net_find(ARG(0), 1);
    n->mask = ARG(1);
    n->cb = ARG(2);
    n->info = ARG(3) ? G32(ARG(3) + 4) : 0;
    RET(1);
}
SHIM(CFReadStreamOpen) {
    NetStream *n = net_find(ARG(0), 1);
    n->pending = 1;
    net_answer(n);
    RET(1);
}
SHIM(CFReadStreamClose) {
    NetStream *n = net_find(ARG(0), 0);
    if (n) memset(n, 0, sizeof *n);
}
SHIM(CFReadStreamCopyProperty) {
    CFStringRef key = S(1);
    if (key && CFEqual(key, kCFStreamPropertyHTTPResponseHeader)) {
        RET(cf_wrap(CFHTTPMessageCreateResponse(NULL, 404, NULL, kCFHTTPVersion1_1)));
        return;
    }
    RET(0);
}
SHIM(CFHTTPMessageGetResponseStatusCode) { RET(CFHTTPMessageGetResponseStatusCode((CFHTTPMessageRef)O(0))); }
SHIM(CFHTTPMessageCopyHeaderFieldValue) { RET(cf_wrap(CFHTTPMessageCopyHeaderFieldValue((CFHTTPMessageRef)O(0), S(1)))); }
SHIM(CFHTTPMessageCopyAllHeaderFields) { RET(cf_wrap(CFHTTPMessageCopyAllHeaderFields((CFHTTPMessageRef)O(0)))); }
SHIM(CFHTTPMessageCopyBody) { RET(cf_wrap(CFHTTPMessageCopyBody((CFHTTPMessageRef)O(0)))); }
SHIM(CFHTTPMessageAppendBytes) { RET(CFHTTPMessageAppendBytes((CFHTTPMessageRef)O(0), GP(ARG(1)), (i32)ARG(2))); }
SHIM(net_true) { RET(1); }
SHIM(net_minus1) { RET(-1); }
SHIM(cf_noop) { RET(0); }
SHIM(cf_fail) { RET(-43); }

// ---------- preferences: the app id is explicit since this process is not the bundle ----------
static CFStringRef app_id(CFStringRef s) {
    if (s && s != kCFPreferencesCurrentApplication) return s;
    return CFSTR("com.feralinteractive.bmaa");
}
// ARKHAM_TRACE_PREFS=1 logs each preference the game reads, with what it got.
static void trace_pref(const char *how, CFStringRef key, CFStringRef app, CFTypeRef v) {
    static int on = -1;
    if (on < 0) on = getenv("ARKHAM_TRACE_PREFS") != NULL;
    if (!on) return;
    char k[128] = "", a[128] = "", val[160] = "";
    if (key) CFStringGetCString(key, k, sizeof k, kCFStringEncodingUTF8);
    if (app) CFStringGetCString(app, a, sizeof a, kCFStringEncodingUTF8);
    if (v) {
        CFStringRef d = CFCopyDescription(v);
        CFStringGetCString(d, val, sizeof val, kCFStringEncodingUTF8);
        CFRelease(d);
    }
    for (char *p = val; *p; p++)
        if (*p == '\n') *p = ' ';
    fprintf(stderr, "[prefs] %s %s in %s -> %s\n", how, k, a, v ? val : "nothing");
}
SHIM(CFPreferencesCopyAppValue) {
    CFTypeRef v = CFPreferencesCopyAppValue(S(0), app_id(S(1)));
    trace_pref("read", S(0), app_id(S(1)), v);
    RET(cf_wrap(v));
}
SHIM(CFPreferencesSetAppValue) { CFPreferencesSetAppValue(S(0), O(1), app_id(S(2))); }
SHIM(CFPreferencesAppSynchronize) { RET(CFPreferencesAppSynchronize(app_id(S(0)))); }
SHIM(CFPreferencesGetAppBooleanValue) {
    Boolean ok = 0;
    Boolean v = CFPreferencesGetAppBooleanValue(S(0), app_id(S(1)), &ok);
    if (ARG(2)) G8(ARG(2)) = ok;
    RET(v);
}
SHIM(CFPreferencesCopyValue) {
    CFTypeRef v = CFPreferencesCopyValue(S(0), app_id(S(1)), S(2), S(3));
    trace_pref("read", S(0), app_id(S(1)), v);
    RET(cf_wrap(v));
}

// ---------- Mach-O self-inspection ----------
static u32 header_copy;
void rt_set_header_copy(u32 g) { header_copy = g; }
SHIM(getsegbyname) {
    u32 p = header_copy + (u32)sizeof(struct mach_header);
    u32 n = ((struct mach_header *)GP(header_copy))->ncmds;
    const char *want = GS(ARG(0));
    for (u32 i = 0; i < n; i++) {
        struct load_command *lc = GP(p);
        if (lc->cmd == LC_SEGMENT && !strncmp(((struct segment_command *)lc)->segname, want, 16)) {
            RET(p);
            return;
        }
        p += lc->cmdsize;
    }
    RET(0);
}
SHIM(getsectdata) {
    u32 p = header_copy + (u32)sizeof(struct mach_header);
    u32 n = ((struct mach_header *)GP(header_copy))->ncmds;
    const char *seg = GS(ARG(0)), *sect = GS(ARG(1));
    for (u32 i = 0; i < n; i++) {
        struct load_command *lc = GP(p);
        if (lc->cmd == LC_SEGMENT && !strncmp(((struct segment_command *)lc)->segname, seg, 16)) {
            struct segment_command *sc = (void *)lc;
            struct section *s = (void *)(sc + 1);
            for (u32 k = 0; k < sc->nsects; k++, s++) {
                if (!strncmp(s->sectname, sect, 16)) {
                    G32(ARG(2)) = s->size;
                    RET(s->addr);
                    return;
                }
            }
        }
        p += lc->cmdsize;
    }
    RET(0);
}

static void set_data_import(const char *guest_name, const void *value) {
    u32 s = rt_import_addr(guest_name);
    if (s) G32(s) = cf_wrap(value);
}

void rt_cf_init(u32 cfstring_start, u32 cfstring_end) {
    cfstring_lo = cfstring_start;
    cfstring_hi = cfstring_end;
    obj_to_handle = CFDictionaryCreateMutable(NULL, 0, NULL, NULL);
    literal_cache = CFDictionaryCreateMutable(NULL, 0, NULL, &kCFTypeDictionaryValueCallBacks);
    a_cb = rt_import_addr("_kCFTypeArrayCallBacks");
    d_kcb = rt_import_addr("_kCFTypeDictionaryKeyCallBacks");
    d_vcb = rt_import_addr("_kCFTypeDictionaryValueCallBacks");
    // Imported string and object constants hold host objects; plain values keep their low bits.
    for (unsigned i = 0; i < rt_import_n; i++) {
        const char *n = rt_import_names[i];
        if (strncmp(n, "_k", 2) && strncmp(n, "_NS", 3)) continue;
        if (strstr(n, "CallBacks") || strstr(n, "TimeInterval") || strstr(n, "NSZero") || strstr(n, "NSApp") || strstr(n, "NSConcrete")) continue;
        void **p = dlsym(RTLD_DEFAULT, n + 1);
        if (!p) continue;
        void *v = *p;
        u32 s = rt_import_addr(n);
        if ((uintptr_t)v > 0xFFFFFFFFu) G32(s) = cf_wrap(v);
        else G32(s) = (u32)(uintptr_t)v;
    }
    (void)call_host;
}

const ShimReg rt_shims_cf[] = {
    R(CFRetain) R(CFRelease) R(CFGetTypeID) R(CFEqual) R(CFHash) R(CFShow) R(CFCopyDescription) R(CFAllocatorCreate) R(CFAllocatorGetDefault)
    R(CFStringGetTypeID) R(CFArrayGetTypeID) R(CFDictionaryGetTypeID) R(CFNumberGetTypeID) R(CFBooleanGetTypeID) R(CFDataGetTypeID)
    R(CFDateGetTypeID) R(CFURLGetTypeID)
    R(CFStringCreateWithCString) R(CFStringCreateWithBytes) R(CFStringCreateWithBytesNoCopy) R(CFStringCreateWithCharacters)
    R(CFStringCreateWithFileSystemRepresentation) R(CFStringCreateCopy) R(CFStringCreateMutable) R(CFStringCreateMutableCopy)
    R(CFStringCreateWithSubstring) R(CFStringGetLength) R(CFStringGetCString) R(CFStringGetCharacters) R(CFStringGetCharacterAtIndex)
    R(CFStringGetFileSystemRepresentation) R(CFStringGetMaximumSizeForEncoding) R(CFStringGetMaximumSizeOfFileSystemRepresentation)
    R(CFStringGetSystemEncoding) R(CFStringGetIntValue) R(CFStringGetDoubleValue) R(CFStringConvertWindowsCodepageToEncoding)
    R(CFStringConvertIANACharSetNameToEncoding) R(CFStringCompare) R(CFStringCompareWithOptions) R(CFStringAppend) R(CFStringInsert)
    R(CFStringDelete) R(CFStringReplace) R(CFStringLowercase) R(CFStringUppercase) R(CFStringFindWithOptions) R(CFStringFindAndReplace) R(CFStringCreateWithFormat) R(CFStringAppendFormat)
    R(CFStringGetBytes) R(CFStringCreateExternalRepresentation) R(CFStringCreateArrayBySeparatingStrings)
    R(CFURLCreateWithFileSystemPath) R(CFURLCreateWithFileSystemPathRelativeToBase) R(CFURLCreateFromFileSystemRepresentation)
    R(CFURLCreateWithString) R(CFURLCreateWithBytes) R(CFURLCopyAbsoluteURL) R(CFURLCopyFileSystemPath) R(CFURLCopyLastPathComponent)
    R(CFURLCopyScheme) R(CFURLCopyHostName) R(CFURLGetString) R(CFURLHasDirectoryPath) R(CFURLGetFileSystemRepresentation)
    R(CFURLCreateCopyAppendingPathComponent) R(CFURLCreateCopyDeletingLastPathComponent) R(CFURLCreateCopyDeletingPathExtension)
    R(CFURLCreateFromFSRef) R(CFURLGetFSRef) R(FSFindFolder) R(FSPathMakeRef) R(FSResolveAliasFile)
    R(CFDictionaryApplyFunction) R(CFTimeZoneCopyDefault) R(CFTimeZoneCopySystem) R(CFTimeZoneGetSecondsFromGMT) R(CFTimeZoneIsDaylightSavingTime)
    R(LSCopyDisplayNameForURL) R(CFBundleCopyAuxiliaryExecutableURL) R(CFRunLoopGetMain) R(CFRunLoopGetCurrent) R(CFRunLoopTimerCreate)
    R(CFRunLoopAddTimer) R(CFRunLoopRemoveTimer) R(CFRunLoopTimerInvalidate) R(CFRunLoopTimerSetNextFireDate) R(CFRunLoopTimerGetContext)
    R2("_CFRunLoopAddSource", cf_noop) R(CFRunLoopWakeUp) R(CFRunLoopStop) R(CFRunLoopRun) R(CFRunLoopRunInMode) R(CFRunLoopGetNextTimerFireDate)
    R2("_AudioFileOpenWithCallbacks", cf_fail) R2("_ExtAudioFileWrapAudioFileID", cf_fail) R2("_ExtAudioFileGetProperty", cf_fail)
    R2("_ExtAudioFileSetProperty", cf_fail) R2("_ExtAudioFileDispose", cf_noop) R2("_AudioFileClose", cf_noop)
    R2("_SecRequirementCreateWithString", cf_fail) R2("_CRYPTO_set_mem_functions", cf_noop) R2("_CurResFile", cf_noop)
    R(CFHTTPMessageCreateRequest) R(CFHTTPMessageCreateEmpty) R(CFHTTPMessageSetHeaderFieldValue) R(CFHTTPMessageSetBody) R(CFReadStreamCreateForHTTPRequest)
    R(CFReadStreamSetClient) R(CFReadStreamOpen) R(CFReadStreamClose) R2("_CFReadStreamSetProperty", net_true)
    R2("_CFReadStreamScheduleWithRunLoop", cf_noop) R2("_CFReadStreamUnscheduleFromRunLoop", cf_noop) R2("_CFReadStreamRead", net_minus1)
    R2("_CFReadStreamHasBytesAvailable", cf_noop) R(CFReadStreamCopyProperty) R(CFHTTPMessageGetResponseStatusCode)
    R(CFHTTPMessageCopyHeaderFieldValue) R(CFHTTPMessageCopyAllHeaderFields) R(CFHTTPMessageCopyBody) R(CFHTTPMessageAppendBytes)
    R2("_CFNetworkCopySystemProxySettings", cf_noop) R2("_CFNetworkCopyProxiesForURL", cf_noop)
    R(CFAbsoluteTimeGetDayOfWeek) R(CFAbsoluteTimeGetGregorianDate) R(CFGregorianDateGetAbsoluteTime) R(UCConvertCFAbsoluteTimeToLongDateTime)
    R(UCConvertLongDateTimeToCFAbsoluteTime) R(CFPropertyListCreateFromXMLData) R(GetCurrentProcess) R(GetProcessBundleLocation) R(UpTime) R(AbsoluteToDuration) R(FSIsAliasFile) R(FSCompareFSRefs) R(FSGetCatalogInfo) R(FSGetVolumeInfo) R(SecRandomCopyBytes)
    R(CFURLCreateStringByAddingPercentEscapes) R(CFURLCreateStringByReplacingPercentEscapes)
    R(CFBundleGetMainBundle) R(CFBundleGetBundleWithIdentifier) R(CFBundleCreate) R(CFBundleCopyBundleURL) R(CFBundleCopyResourcesDirectoryURL)
    R(CFBundleCopyPrivateFrameworksURL) R(CFBundleCopyResourceURL) R(CFBundleCopyResourceURLsOfType) R(CFBundleGetInfoDictionary)
    R(CFBundleGetValueForInfoDictionaryKey) R(CFBundleCopyBundleLocalizations) R(CFBundleCopyPreferredLocalizationsFromArray)
    R(CFBundleCopyLocalizationsForPreferences) R(CFBundleCopyLocalizedString) R(CFBundleGetFunctionPointerForName)
    R(CFBundleIsExecutableLoaded) R(CFBundleLoadExecutable)
    R(CFArrayCreateMutable) R(CFArrayCreate) R(CFArrayCreateCopy) R(CFArrayGetCount) R(CFArrayGetValueAtIndex) R(CFArrayAppendValue)
    R(CFArrayInsertValueAtIndex) R(CFArrayRemoveValueAtIndex) R(CFArrayRemoveAllValues) R(CFArrayApplyFunction)
    R(CFDictionaryCreateMutable) R(CFDictionaryCreate) R(CFDictionaryCreateCopy) R(CFDictionaryCreateMutableCopy) R(CFDictionaryGetCount)
    R(CFDictionaryGetValue) R(CFDictionaryGetValueIfPresent) R(CFDictionaryGetCountOfKey) R(CFDictionarySetValue) R(CFDictionaryAddValue)
    R(CFDictionaryGetKeysAndValues) R(CFBooleanGetValue) R(CFNumberCreate) R(CFNumberGetValue) R(CFDataCreate) R(CFDataCreateWithBytesNoCopy) R(CFDataCreateMutable) R(CFDataGetLength)
    R(CFDataGetBytes) R(CFDataGetBytePtr) R(CFAbsoluteTimeGetCurrent) R(CFPreferencesCopyAppValue) R(CFPreferencesSetAppValue)
    R(CFPreferencesAppSynchronize) R(CFPreferencesGetAppBooleanValue) R(CFPreferencesCopyValue) R(getsegbyname) R(getsectdata)
    {NULL, NULL},
};
