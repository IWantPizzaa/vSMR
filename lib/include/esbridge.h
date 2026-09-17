/*  esbridge.h  --  EuroScope Plugin Bridge, public C ABI, version 1
 *
 *  Drop this single header into any EuroScope plugin. It depends on nothing but
 *  <stdint.h> and <windows.h>, links against no import library, and degrades to
 *  a clean no-op when the bridge DLL is not installed on the user's machine.
 *
 *  ABI RULES -- read before changing anything below.
 *    - C linkage only. No C++ types, no exceptions, no STL across the boundary.
 *      Plugins are built with whatever MSVC toolset their author happens to
 *      have; an std::string crossing this line is a crash waiting for a
 *      version bump.
 *    - Every struct that may grow begins with uint32_t struct_size.
 *    - The bridge never frees caller memory; the caller never frees bridge
 *      memory.
 *    - Released members of ESB_Api_v1 are never reordered, retyped or removed.
 *      New capability is appended (readers gate on struct_size) or lands in v2.
 *    - EVERY call must be made on the EuroScope main thread. See ARCHITECTURE.md.
 */
#ifndef ESBRIDGE_H_INCLUDED
#define ESBRIDGE_H_INCLUDED

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESB_ABI_VERSION    1u
#define ESB_MODULE_NAME    "EuroScopeBridge.dll"
#define ESB_ENTRY_SYMBOL   "ESB_GetApi"

/* The bridge is released standalone -- do NOT ship a copy alongside your
 * plugin. Windows maps only one module of a given base name per process, so
 * bundled copies silently collapse onto whichever the user listed first,
 * which may be far older than yours. See ARCHITECTURE.md 14.1. */
#define ESB_DOWNLOAD_URL \
    "https://github.com/AlexisBalzano/Euroscope-Plugin-Bridge/releases"

/* One consistent instruction, so a user running several bridge-aware plugins
 * learns it once instead of three different ways. Print it once with
 * DisplayUserMessage after ~10 failed ticks, then stop. */
#define ESB_MISSING_MESSAGE \
    "EuroScope Plugin Bridge is not loaded. Download EuroScopeBridge.dll and " \
    "add it under Other Settings > Plug-ins > Load: " ESB_DOWNLOAD_URL

/* ------------------------------------------------------------------ status */

typedef int32_t ESB_Status;

#define ESB_OK                    0
#define ESB_E_NO_PROVIDER        -1   /* provider id not registered (yet)        */
#define ESB_E_NO_FIELD           -2   /* provider exists, field name unknown     */
#define ESB_E_TYPE_MISMATCH      -3   /* resolve/set disagrees with the schema   */
#define ESB_E_NOT_OWNER          -4   /* write attempted on a field you don't own*/
#define ESB_E_PROVIDER_TAKEN     -5   /* another live module owns that id        */
#define ESB_E_UNKNOWN_AIRCRAFT   -6   /* callsign not currently on the network    */
#define ESB_E_STALE_AIRCRAFT     -7   /* handle refers to an earlier connection   */
#define ESB_E_BUFFER_TOO_SMALL   -8   /* *io_bytes set to the size required       */
#define ESB_E_UNSET              -9   /* field is declared but holds no value     */
#define ESB_E_WRONG_THREAD      -10   /* debug builds only; release asserts       */
#define ESB_E_INVALID_ARG       -11
#define ESB_E_SCHEMA_CONFLICT   -12   /* re-register with an incompatible schema  */
#define ESB_E_LIMIT             -13   /* quota exceeded (fields, size, rate)      */
#define ESB_E_NO_BRIDGE         -14   /* client-shim only: bridge DLL not loaded  */
#define ESB_E_SHUTDOWN          -15   /* bridge has shut down; state is flushed   */
                                      /* and takes no more writes. Reads,         */
                                      /* unregister_provider and unsubscribe keep */
                                      /* working, so teardown still unwinds.      */

/* ------------------------------------------------------------- value model */

typedef uint32_t ESB_Type;
#define ESB_T_I64    1u
#define ESB_T_F64    2u
#define ESB_T_BOOL   3u
#define ESB_T_STR    4u   /* UTF-8; length carried explicitly, NUL not required */
#define ESB_T_BLOB   5u

typedef uint32_t ESB_Scope;
#define ESB_SCOPE_GLOBAL    1u
#define ESB_SCOPE_AIRCRAFT  2u

/* Field declaration flags. */
#define ESB_F_SYNC      (1u << 0)  /* eligible for the online relay IF the user  */
                                   /* also enables it in bridge.json. Author     */
                                   /* consent alone never puts data on the wire. */
#define ESB_F_PERSIST   (1u << 1)  /* global scope only; survives a restart      */
#define ESB_F_DENSE     (1u << 2)  /* storage hint: most aircraft carry a value  */

typedef struct ESB_Value {
    ESB_Type  type;
    uint32_t  bytes;          /* STR/BLOB: payload length. Scalars: sizeof.     */
    union {
        int64_t     i64;
        double      f64;
        int32_t     b;
        const void *ptr;      /* STR/BLOB payload                               */
    } v;
} ESB_Value;

/* ------------------------------------------------------------------ handles */

typedef uint64_t ESB_Aircraft;   /* opaque: slot index | generation counter     */
typedef uint32_t ESB_FieldId;    /* opaque; resolve once at startup, reuse      */
typedef struct ESB_Provider ESB_Provider;   /* opaque write-authority token     */
typedef struct ESB_Sub      ESB_Sub;        /* opaque subscription token        */

#define ESB_AIRCRAFT_NONE ((ESB_Aircraft)0)
#define ESB_FIELD_NONE    ((ESB_FieldId)0)

/* ------------------------------------------------------------------ schemas */

typedef struct ESB_FieldDecl {
    const char *name;        /* "cleared_level" -- [a-z0-9_], 47 chars max      */
    ESB_Type    type;
    ESB_Scope   scope;
    uint32_t    flags;
    uint32_t    max_bytes;   /* STR/BLOB only; hard cap enforced on write       */
    const char *doc;         /* one line, shown by ".esb schema <provider>"     */
} ESB_FieldDecl;

typedef struct ESB_ProviderDecl {
    uint32_t             struct_size;   /* = sizeof(ESB_ProviderDecl)           */
    const char          *provider_id;   /* claimed namespace, e.g. "ccams"      */
    uint32_t             schema_major;  /* breaking change -> bump              */
    uint32_t             schema_minor;  /* additive change  -> bump             */
    const char          *display_name;
    const char          *contact;       /* URL or email, shown in diagnostics   */
    const ESB_FieldDecl *fields;
    uint32_t             field_count;
    void                *module;        /* HMODULE of the CALLING plugin.       */
                                        /* The bridge uses this for liveness    */
                                        /* reaping -- see ARCHITECTURE.md 9.1.  */
} ESB_ProviderDecl;

/* ------------------------------------------------------------------ logging */

#define ESB_LOG_DEBUG  0
#define ESB_LOG_INFO   1
#define ESB_LOG_WARN   2
#define ESB_LOG_ERROR  3

/* -------------------------------------------------------------- change feed */

/* Deliberately carries no value: the subscriber reads it, so there is no
 * pointer-lifetime question and coalescing repeated writes is trivial.
 *
 * Called on the EuroScope main thread, synchronously inside the publisher's
 * write -- so local delivery costs zero ticks. Keep it short. You MAY write
 * from here; those writes apply immediately but their own notifications are
 * deferred until the outermost dispatch unwinds (ARCHITECTURE.md 7.1).
 *
 * Lifecycle drops (a disconnect, a TTL sweep) do NOT notify: a mass retirement
 * would be a storm, and the aircraft handle going stale already tells you. */
typedef void (__cdecl *ESB_OnChange)(ESB_FieldId field,
                                     ESB_Aircraft ac,   /* NONE if global      */
                                     void *user);

/* ------------------------------------------------------------ peers, origin */

/* ESB_Peer and ESB_Origin are FIXED-LAYOUT output records, not extensible
 * descriptors, so they carry no struct_size: the bridge fills arrays of them
 * at a stride both sides agree on at compile time. Their layout is frozen for
 * the whole life of ABI v1. Anything new goes in ESB_Peer2 / ESB_Origin2. */

typedef struct ESB_Peer {
    char     callsign[16];   /* the remote controller's position                */
    uint64_t last_seen_ms;   /* bridge monotonic clock                          */
} ESB_Peer;

/* Provenance for one remote value.
 *
 * There is deliberately NO single authority for an aircraft. Any controller
 * may publish about any aircraft, whether or not they have it assumed -- a
 * CDM plugin has a TOBT for a departure nobody is tracking yet, a sequencer
 * has an arrival order for traffic still in the sector next door. A consumer
 * that finds several publishers arbitrates with this struct: most recent,
 * preferred peer, or its own domain rule. */
typedef struct ESB_Origin {
    char     peer[16];       /* publishing controller's position                */
    uint64_t revision;       /* the publisher's own monotonic counter           */
    uint64_t received_ms;    /* local receipt, bridge monotonic clock           */
} ESB_Origin;

/* ------------------------------------------------------------------ the API */

typedef struct ESB_Api_v1 {
    uint32_t struct_size;
    uint32_t abi_version;    /* shape of this struct -- what you negotiate on  */
    uint32_t bridge_build;   /* WHICH BUILD filled it in. Monotonic. Gate a    */
                             /* specific fix on this; quote it in bug reports. */
    uint32_t reserved_;      /* keeps the pointers below 8-byte aligned on x64 */

    /* -- registration (owner side) -------------------------------------- */
    ESB_Status (__cdecl *register_provider)(const ESB_ProviderDecl *decl,
                                            ESB_Provider **out);
    ESB_Status (__cdecl *unregister_provider)(ESB_Provider *p);
    ESB_Status (__cdecl *own_field)(ESB_Provider *p, const char *field,
                                    ESB_FieldId *out);

    /* -- discovery (consumer side) -------------------------------------- */
    /* qualified_name is "provider/field", e.g. "ccams/assigned_squawk". */
    ESB_Status (__cdecl *resolve)(const char *qualified_name, ESB_Type expect,
                                  ESB_FieldId *out);
    ESB_Status (__cdecl *provider_version)(const char *provider_id,
                                           uint32_t *major, uint32_t *minor);
    ESB_Status (__cdecl *list_providers)(char *buf, uint32_t *io_bytes);

    /* -- global scope ----------------------------------------------------- */
    ESB_Status (__cdecl *get_global)(ESB_FieldId f, ESB_Value *out,
                                     void *buf, uint32_t *io_bytes);
    ESB_Status (__cdecl *set_global)(ESB_Provider *p, ESB_FieldId f,
                                     const ESB_Value *v);
    ESB_Status (__cdecl *clear_global)(ESB_Provider *p, ESB_FieldId f);

    /* -- aircraft scope --------------------------------------------------- */
    ESB_Status (__cdecl *aircraft)(const char *callsign, ESB_Aircraft *out);
    ESB_Status (__cdecl *aircraft_callsign)(ESB_Aircraft a, char *buf,
                                            uint32_t *io_bytes);
    ESB_Status (__cdecl *get_ac)(ESB_Aircraft a, ESB_FieldId f, ESB_Value *out,
                                 void *buf, uint32_t *io_bytes);
    ESB_Status (__cdecl *set_ac)(ESB_Provider *p, ESB_Aircraft a, ESB_FieldId f,
                                 const ESB_Value *v);
    ESB_Status (__cdecl *clear_ac)(ESB_Provider *p, ESB_Aircraft a,
                                   ESB_FieldId f);

    /* -- change feed ------------------------------------------------------ */
    /* Monotonic, never reused, 0 == unset. Cheaper than comparing the value
     * itself, and the only thing a polling consumer needs. */
    uint64_t   (__cdecl *revision)(ESB_FieldId f, ESB_Aircraft a);
    uint64_t   (__cdecl *provider_revision)(const char *provider_id);
    ESB_Status (__cdecl *subscribe)(ESB_FieldId f, ESB_OnChange cb, void *user,
                                    void *module, ESB_Sub **out);
    ESB_Status (__cdecl *unsubscribe)(ESB_Sub *s);

    /* -- remote view (online relay; read-only, never merged with local) --- */
    /* Remote values are stored per publishing peer, for aircraft scope exactly
     * as for global scope. Ask who is publishing, then read the one you want.
     * *io_count is normally 0 or 1; the API just refuses to guarantee it. */
    ESB_Status (__cdecl *remote_publishers)(ESB_FieldId f, ESB_Aircraft a,
                                            ESB_Origin *out,
                                            uint32_t *io_count);
    ESB_Status (__cdecl *get_remote)(ESB_FieldId f, ESB_Aircraft a,
                                     const char *peer, ESB_Value *out,
                                     void *buf, uint32_t *io_bytes);
    ESB_Status (__cdecl *list_peers)(ESB_Peer *out, uint32_t *io_count);

    /* -- diagnostics ------------------------------------------------------ */
    void       (__cdecl *log)(ESB_Provider *p, int32_t level, const char *msg);
    ESB_Status (__cdecl *last_error)(char *buf, uint32_t *io_bytes);
} ESB_Api_v1;

/* Returns NULL if the requested ABI version cannot be served. */
typedef const ESB_Api_v1 * (__cdecl *ESB_GetApiFn)(uint32_t abi_version);

/* ============================================================== client shim */
/* Header-only lazy attach. Safe to call every tick: once attached it is a
 * single pointer test, and the bridge may legitimately load after you do,
 * because EuroScope plugin load order follows the user's settings file. */

#ifdef ESB_CLIENT_SHIM
#include <string.h>
#include <windows.h>

static const ESB_Api_v1 *esb_api = 0;

static inline const ESB_Api_v1 *ESB_Attach(void)
{
    HMODULE m;
    ESB_GetApiFn get;

    if (esb_api) return esb_api;

    /* GetModuleHandle, never LoadLibrary: the bridge must be a registered
     * EuroScope plugin so that it receives OnTimer and aircraft lifecycle
     * events. Loading it ourselves would produce a bridge that never ticks. */
    m = GetModuleHandleA(ESB_MODULE_NAME);
    if (!m) return 0;

    get = (ESB_GetApiFn)GetProcAddress(m, ESB_ENTRY_SYMBOL);
    if (!get) return 0;

    esb_api = get(ESB_ABI_VERSION);
    return esb_api;
}

/* Your own HMODULE, for ESB_ProviderDecl::module and subscribe(). Correct
 * even when your DLL has been renamed, unlike GetModuleHandleA("Mine.dll"). */
static inline void *ESB_SelfModule(void)
{
    HMODULE self = 0;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&ESB_SelfModule, &self);
    return (void *)self;
}

/* --- value constructors -------------------------------------------------
 * Filling an ESB_Value by hand is three lines of boilerplate per write and
 * an easy place to set `bytes` wrong. These do it correctly.
 *
 *     api->set_ac(prov, ac, tobt, &ESB_I64(742));      // C++
 *     ESB_Value v = ESB_Str("GND"); api->set_global(prov, label, &v);
 *
 * ESB_Str and ESB_Blob borrow the caller's buffer -- it must outlive the
 * set_* call, which for a literal or a local about to be passed is automatic.
 */
static inline ESB_Value ESB_I64(int64_t v)
{
    ESB_Value r; r.v.i64 = 0;
    r.type = ESB_T_I64; r.bytes = (uint32_t)sizeof(int64_t); r.v.i64 = v;
    return r;
}

static inline ESB_Value ESB_F64(double v)
{
    ESB_Value r; r.v.i64 = 0;
    r.type = ESB_T_F64; r.bytes = (uint32_t)sizeof(double); r.v.f64 = v;
    return r;
}

static inline ESB_Value ESB_Bool(int v)
{
    ESB_Value r; r.v.i64 = 0;
    r.type = ESB_T_BOOL; r.bytes = (uint32_t)sizeof(int32_t); r.v.b = v ? 1 : 0;
    return r;
}

static inline ESB_Value ESB_Blob(const void *p, uint32_t bytes)
{
    ESB_Value r; r.v.i64 = 0;
    r.type = ESB_T_BLOB; r.bytes = bytes; r.v.ptr = p;
    return r;
}

static inline ESB_Value ESB_Str(const char *s)
{
    ESB_Value r; r.v.i64 = 0;
    r.type = ESB_T_STR;
    r.bytes = s ? (uint32_t)strlen(s) : 0u;   /* length carried, no NUL needed */
    r.v.ptr = s;
    return r;
}
#endif /* ESB_CLIENT_SHIM */

#ifdef __cplusplus
}
#endif
#endif /* ESBRIDGE_H_INCLUDED */
