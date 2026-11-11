/*
 * shz_abi.h - the stable C interface of ShizoScript native plugins (issue #201)
 *
 * This header is the whole plugin SDK. A plugin includes it, implements the three entry points below and is built
 * into a shared library with any C99 (or C++) compiler, without the ShizoScript sources: no C++ type, no STL type,
 * no core allocator and no reference count crosses this boundary. Everything is plain C, fixed-width integers,
 * natural alignment (no #pragma pack) and versioned structs. The guide for plugin authors (package layout, build,
 * lifetime and thread rules, errors, versions) is core/shizoscript3_core_cabi/README.md.
 *
 * Overview
 *   1. The host loads shizotech/packages/<package>/native/<platform>/shzmodule_<package>.<so|dylib|dll> on
 *      "import <package>;" and finds the export shz_module_abi.
 *   2. shz_module_abi(): the version handshake. The host refuses a plugin of another major version, of a newer minor
 *      version or one that needs a feature the host does not have - a clean import error, the plugin is not called
 *      again.
 *   3. shz_module_register(): the plugin registers functions, types, constants and (1.2, 1.3) native VibeVJ assets through
 *      shz_register_api and keeps the shz_host_api pointer (valid until shz_module_unregister). Nothing is visible to
 *      scripts before it returned SHZ_OK; when it fails (or another library of the same package fails) nothing of it
 *      is registered.
 *   4. Scripts call the functions and use the types like built-in ones. Every callback gets a shz_ctx* (the running
 *      call) and works on values through the host API.
 *   5. shz_module_unregister() (optional) runs when the host drops a plugin it registered (the import of its package
 *      failed after it registered) and before a reload unloads it (std.native_reload, C ABI 1.3 hosts: the plugin
 *      was rebuilt while the process runs; rules in the README, "Reloading a plugin while it runs").
 *
 * Rules that hold everywhere
 *   - Threads: every callback runs on the script thread that uses the value, with the script lock held. Host API
 *     functions may only be called from inside a callback, on that thread, with that callback's ctx (exception:
 *     release(), string_data(), bytes_size(), count(), type_name() and object_self() need no ctx, they too run on the
 *     script thread only, e.g. in a finalizer). A plugin may run its own threads, but they must not call the host API
 *     and must not touch values; they hand results to the next callback. Scripts on different threads can call the
 *     plugin at the same time: global state of the plugin needs its own lock.
 *   - Values passed to a callback (args, the value of a setter, ...) are borrowed: valid until the callback returns.
 *     Values the host API creates during a callback (make_*, list_get, ...) belong to that callback and are released
 *     when it returns. To hold a value longer, keep() it and release() it later. A value whose time is over is
 *     detected (SHZ_ERR_STALE) in most cases, but do not rely on that.
 *   - Strings are UTF-8. Every char* the plugin passes is copied by the host before the call returns.
 *   - Errors: a callback returns SHZ_OK or an error code. An error code becomes a script runtime error with the
 *     message given to host->error() (or a default one): try { } catch(e) { } in the script catches it, uncaught it
 *     stops the task like std.runtime_error(). host->log(ctx, SHZ_LEVEL_ERROR, ...) reports a non-fatal error
 *     instead (printed with the script call stack, the script goes on).
 *   - Do not throw C++ exceptions or longjmp out of a callback, do not install process-wide signal / exception
 *     handlers. The host catches a C++ exception of a callback and turns it into a runtime error, but the state of the
 *     plugin is unknown after that. A crash inside a plugin call names the plugin and the function in the crash
 *     report of shz.
 *
 * Forward compatibility
 *   - SHZ_ABI_VERSION_MAJOR changes only for an incompatible change; a plugin works with every host of the same
 *     major version whose minor version is at least its own. New host API functions and struct fields are appended at
 *     the end of the structs (minor version + 1). Every struct starts with struct_size: the side that fills it sets
 *     sizeof() as it was compiled, the other side reads only what that size covers.
 *   - Plugins fill struct_size of every definition they pass (shz_func_def, shz_type_def, ...) with sizeof(); use the
 *     SHZ_INIT() macro or set it by hand. Set unused fields to 0 / NULL.
 *
 * Not on microcontroller builds (SHZ_FEAT_MICROCONTROLLER) and not in the Emscripten build: there is no dynamic
 * loading there, import of such a package fails.
 */

#ifndef SHZ_ABI_H
#define SHZ_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------------------------------ version, features */

#define SHZ_ABI_VERSION_MAJOR 1
#define SHZ_ABI_VERSION_MINOR 3

/* Feature bits. A plugin lists what it cannot work without in shz_abi_handshake.plugin_required_features. */
#define SHZ_FEATURE_FUNCTIONS ((uint64_t)1 << 0) /* namespace functions and constants */
#define SHZ_FEATURE_TYPES     ((uint64_t)1 << 1) /* object types: constructor, methods, properties, finalizer */
#define SHZ_FEATURE_BUFFERS   ((uint64_t)1 << 2) /* bytes_lock / bytes_unlock / bytes_resize (zero-copy buffers) */
#define SHZ_FEATURE_CALLBACKS ((uint64_t)1 << 3) /* host->call(): start a script function */
#define SHZ_FEATURE_TYPE_OPS  ((uint64_t)1 << 4) /* to_string, index_get/set, size, compare, arith of a type */
#define SHZ_FEATURE_SERIALIZE ((uint64_t)1 << 5) /* serialize / deserialize of a type */
#define SHZ_FEATURE_FILES     ((uint64_t)1 << 6) /* file_kind / file_read / file_write (C ABI 1.1) */
#define SHZ_FEATURE_ASSETS    ((uint64_t)1 << 7) /* reg->add_asset: native VibeVJ assets (C ABI 1.2) */
#define SHZ_FEATURE_ASSET_INPUTS ((uint64_t)1 << 8) /* picture inputs of native assets (C ABI 1.3) */
#define SHZ_FEATURE_ASSET_TEXT   ((uint64_t)1 << 9) /* SHZ_CONTROL_TEXT controls of native assets (C ABI 1.3) */
#define SHZ_FEATURES_1_0 (SHZ_FEATURE_FUNCTIONS | SHZ_FEATURE_TYPES | SHZ_FEATURE_BUFFERS | SHZ_FEATURE_CALLBACKS | \
	SHZ_FEATURE_TYPE_OPS | SHZ_FEATURE_SERIALIZE)
#define SHZ_FEATURES_1_1 (SHZ_FEATURES_1_0 | SHZ_FEATURE_FILES)
#define SHZ_FEATURES_1_2 (SHZ_FEATURES_1_1 | SHZ_FEATURE_ASSETS)
#define SHZ_FEATURES_1_3 (SHZ_FEATURES_1_2 | SHZ_FEATURE_ASSET_INPUTS | SHZ_FEATURE_ASSET_TEXT)

/* ------------------------------------------------------------------------------------------------------- exporting */

#if defined(_WIN32) || defined(__CYGWIN__)
#define SHZ_PLUGIN_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define SHZ_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define SHZ_PLUGIN_EXPORT
#endif

/* Put this in front of the definitions of the entry points (also correct in a C++ plugin). */
#ifdef __cplusplus
#define SHZ_PLUGIN_API extern "C" SHZ_PLUGIN_EXPORT
#else
#define SHZ_PLUGIN_API SHZ_PLUGIN_EXPORT
#endif

/* Sets struct_size of a definition struct: shz_func_def def = {0}; SHZ_INIT(def); */
#define SHZ_INIT(s) ((s).struct_size = (uint32_t)sizeof(s))

/* --------------------------------------------------------------------------------------------------------- results */

typedef int32_t shz_result;

#define SHZ_OK              0
#define SHZ_ERR_RUNTIME     1  /* a runtime error of the plugin, message from host->error() */
#define SHZ_ERR_ARGS        2  /* wrong number or type of arguments */
#define SHZ_ERR_TYPE        3  /* the value has the wrong type for this operation */
#define SHZ_ERR_RANGE       4  /* index or size out of range */
#define SHZ_ERR_NOT_FOUND   5  /* no such map key / member */
#define SHZ_ERR_NOMEM       6  /* out of memory */
#define SHZ_ERR_STATE       7  /* not possible now: buffer locked, registration closed, name taken, ... */
#define SHZ_ERR_THREAD      8  /* host API called outside its callback or on another thread */
#define SHZ_ERR_STALE       9  /* a value whose time is over (see the lifetime rules) */
#define SHZ_ERR_VERSION     10 /* ABI version or feature mismatch */
#define SHZ_ERR_UNSUPPORTED 11 /* operation not supported: compare / arith hooks return it to get the default */
#define SHZ_ERR_INVALID     12 /* invalid argument of a host API call (NULL, bad struct_size, bad name) */

/* ---------------------------------------------------------------------------------------------------------- values */

typedef uint32_t shz_value_type;

#define SHZ_T_NONE     0  /* None */
#define SHZ_T_BOOL     1  /* as.i is 0 or 1. Scripts have no bool type (true == 1): arguments arrive as SHZ_T_INT */
#define SHZ_T_INT      2  /* as.i, 64 bit signed */
#define SHZ_T_FLOAT    3  /* as.f, double */
#define SHZ_T_STRING   4  /* handle: string_data() */
#define SHZ_T_BYTES    5  /* handle: a std.buffer, bytes_lock() / bytes_size() */
#define SHZ_T_LIST     6  /* handle: a JSON list ([1, 2, 3]), list_* and count() */
#define SHZ_T_MAP      7  /* handle: a JSON object ([a = 1]), map_*, also list_get() by position */
#define SHZ_T_OBJECT   8  /* handle: a native object (of this plugin: object_self(); of others: type_name()) */
#define SHZ_T_FUNCTION 9  /* handle: a script function, lambda or builtin: call() */
#define SHZ_T_OTHER    10 /* handle: anything else (a script class instance, ...): keep / pass on / type_name only */

/*
 * A value. Small and copyable: copying the struct copies the reference, not the data (like an assignment in a
 * script: a list or object is shared, see the README). For the handle types `as.ref` is an opaque host handle and
 * `tag` a host-private check value; copy both along, never change them.
 */
typedef struct shz_value
{
	shz_value_type type; /* SHZ_T_* */
	uint32_t tag;        /* host private */
	union
	{
		int64_t i;   /* SHZ_T_BOOL, SHZ_T_INT */
		double f;    /* SHZ_T_FLOAT */
		void* ref;   /* every other type except SHZ_T_NONE */
	} as;
} shz_value;

/* Plain value constructors (no host call, nothing to release). */
static inline shz_value shz_none(void) { shz_value v; v.type = SHZ_T_NONE; v.tag = 0; v.as.i = 0; return v; }
static inline shz_value shz_bool(int b) { shz_value v; v.type = SHZ_T_BOOL; v.tag = 0; v.as.i = b ? 1 : 0; return v; }
static inline shz_value shz_int(int64_t i) { shz_value v; v.type = SHZ_T_INT; v.tag = 0; v.as.i = i; return v; }
static inline shz_value shz_float(double f) { shz_value v; v.type = SHZ_T_FLOAT; v.tag = 0; v.as.f = f; return v; }

/* Numbers: INT, BOOL and FLOAT all count (a script passes 2 for 2.0). */
static inline int shz_is_number(const shz_value* v)
{
	return v->type == SHZ_T_INT || v->type == SHZ_T_FLOAT || v->type == SHZ_T_BOOL;
}
static inline double shz_to_double(const shz_value* v)
{
	return v->type == SHZ_T_FLOAT ? v->as.f : (v->type == SHZ_T_INT || v->type == SHZ_T_BOOL) ? (double)v->as.i : 0.0;
}
static inline int64_t shz_to_int(const shz_value* v) /* a float is truncated toward zero, no range check */
{
	return v->type == SHZ_T_FLOAT ? (int64_t)v->as.f : (v->type == SHZ_T_INT || v->type == SHZ_T_BOOL) ? v->as.i : 0;
}

/* ------------------------------------------------------------------------------------------------- opaque handles */

typedef struct shz_ctx shz_ctx;                 /* the running callback */
typedef struct shz_module shz_module;           /* this plugin as the host knows it */
typedef const struct shz_type_rec* shz_type_handle; /* a type this plugin registered (add_type) */

/* ----------------------------------------------------------------------------------------------- callback types */

/* A namespace function: args[0 .. argc-1] (argc is within min_args .. max_args), the result into *out (NONE when
 * left alone). host->call_user(ctx) gives shz_func_def.user. */
typedef shz_result (*shz_function_fn)(shz_ctx* ctx, const shz_value* args, uint32_t argc, shz_value* out);

/* A method of a type: self is the pointer the constructor / make_object() gave the object. host->self(ctx) gives the
 * object as a value (return it to allow chaining: obj.a().b()). host->call_user(ctx) gives shz_method_def.user. */
typedef shz_result (*shz_method_fn)(shz_ctx* ctx, void* self, const shz_value* args, uint32_t argc, shz_value* out);

/* A property: obj.name reads through get, obj.name = v writes through set. */
typedef shz_result (*shz_getter_fn)(shz_ctx* ctx, void* self, shz_value* out);
typedef shz_result (*shz_setter_fn)(shz_ctx* ctx, void* self, const shz_value* value);

/* Constructor: the script calls the type name (cabi_example.counter(5)). Put the new object's state into *self_out
 * (may stay NULL). On an error nothing is created and finalize is not called. */
typedef shz_result (*shz_construct_fn)(shz_ctx* ctx, const shz_value* args, uint32_t argc, void** self_out);

/* Finalizer: exactly once per object, when the last reference goes or std.free(obj) destroys it. No ctx: only
 * release() of kept values may be called here, no other host API function. type_user is shz_type_def.user. */
typedef void (*shz_finalize_fn)(void* self, void* type_user);

/* Optional operations of a type (shz_type_def), NULL = default behaviour. host->call_user(ctx) gives
 * shz_type_def.user in all of them. */
typedef shz_result (*shz_to_string_fn)(shz_ctx* ctx, void* self, shz_value* out); /* std.string(obj), print: a STRING */
/* obj[key] and obj.name for a name that is no method / property (key: INT or STRING) */
typedef shz_result (*shz_index_get_fn)(shz_ctx* ctx, void* self, const shz_value* key, shz_value* out);
typedef shz_result (*shz_index_set_fn)(shz_ctx* ctx, void* self, const shz_value* key, const shz_value* value);
typedef shz_result (*shz_size_fn)(shz_ctx* ctx, void* self, uint64_t* size_out); /* the size of the object */
/* ==, <, <=, >, >= with another object: *order < 0, 0 or > 0 for self < other, == , >. SHZ_ERR_UNSUPPORTED (for this
 * other value): the default (== is identity, < and > are false). */
typedef shz_result (*shz_compare_fn)(shz_ctx* ctx, void* self, const shz_value* other, int32_t* order);
/* +, -, *, / with the object on either side (self_is_left: 1 for self <op> other, 0 for other <op> self).
 * SHZ_ERR_UNSUPPORTED: "cannot add ..." runtime error of the script. */
typedef shz_result (*shz_arith_fn)(shz_ctx* ctx, void* self, uint32_t op, const shz_value* other, int32_t self_is_left,
	shz_value* out);
/* obj.serialize(): the state as plain data (NONE, BOOL, INT, FLOAT, STRING, BYTES, LIST, MAP), e.g. for presets. */
typedef shz_result (*shz_serialize_fn)(shz_ctx* ctx, void* self, shz_value* out);
/* obj.deserialize(data): restore the state from what serialize gave (possibly from an older plugin version). */
typedef shz_result (*shz_deserialize_fn)(shz_ctx* ctx, void* self, const shz_value* data);

/* Result of a script function started with host->call(), when it has finished (on the script thread, later or
 * before call() returns). result is NONE when the function failed. */
typedef void (*shz_done_fn)(shz_ctx* ctx, const shz_value* result, void* user);

#define SHZ_ARITH_ADD 1
#define SHZ_ARITH_SUB 2
#define SHZ_ARITH_MUL 3
#define SHZ_ARITH_DIV 4

/* ------------------------------------------------------------------------------------------------- definitions */

#define SHZ_ARGS_VARIADIC 0xFFFFFFFFu /* max_args: any number */

typedef struct shz_func_def
{
	uint32_t struct_size;           /* sizeof(shz_func_def) */
	uint32_t flags;                 /* 0 */
	const char* name;               /* the script name: [A-Za-z_][A-Za-z0-9_]* */
	shz_function_fn fn;
	uint32_t min_args;
	uint32_t max_args;              /* >= min_args, or SHZ_ARGS_VARIADIC */
	const char* doc;                /* what it does and returns, for std.docs / "shz docs" (please do fill it) */
	const char* const* param_docs;  /* one per parameter: "name: type - meaning", may be NULL */
	uint32_t param_doc_count;
	void* user;                     /* host->call_user(ctx) */
} shz_func_def;

typedef struct shz_method_def
{
	uint32_t struct_size;           /* sizeof(shz_method_def) */
	uint32_t flags;                 /* 0 */
	const char* name;
	shz_method_fn fn;
	uint32_t min_args;
	uint32_t max_args;
	const char* doc;
	const char* const* param_docs;
	uint32_t param_doc_count;
	void* user;
} shz_method_def;

typedef struct shz_property_def
{
	uint32_t struct_size;           /* sizeof(shz_property_def) */
	uint32_t flags;                 /* 0 */
	const char* name;
	shz_getter_fn get;              /* NULL: write-only (reading it is a runtime error) */
	shz_setter_fn set;              /* NULL: read-only (writing it is a runtime error) */
	const char* doc;
	void* user;
} shz_property_def;

typedef struct shz_type_def
{
	uint32_t struct_size;           /* sizeof(shz_type_def) */
	uint32_t flags;                 /* 0 */
	const char* name;               /* in its namespace: "counter" in "cabi_example" is the type cabi_example.counter */
	const char* doc;

	/* Constructor, registered as the namespace function of the type name. NULL: scripts cannot construct it, only the
	 * plugin (host->make_object). */
	shz_construct_fn construct;
	uint32_t ctor_min_args;
	uint32_t ctor_max_args;
	const char* const* ctor_param_docs;
	uint32_t ctor_param_doc_count;

	shz_finalize_fn finalize;       /* may be NULL when self needs no cleanup */

	const shz_method_def* methods;
	uint32_t method_count;
	const shz_property_def* properties;
	uint32_t property_count;

	/* optional operations, NULL = default */
	shz_to_string_fn to_string;
	shz_index_get_fn index_get;
	shz_index_set_fn index_set;
	shz_size_fn size;
	shz_compare_fn compare;
	shz_arith_fn arith;
	shz_serialize_fn serialize;     /* adds the method serialize() */
	shz_deserialize_fn deserialize; /* adds the method deserialize(data) */

	void* user;                     /* type_user: given to finalize, host->call_user() in constructor and operations */
} shz_type_def;

/* -------------------------------------------------------------- native VibeVJ assets (C ABI 1.2, 1.3, assets) */

/*
 * A native asset is an algorithm the engine of VibeVJ runs like a shader asset. It declares controls (the same kinds a
 * shader declares with its //@ annotations, so the engine builds the same UI, and links, MIDI mappings and presets
 * work), value outputs and a picture, and it has a lifecycle: create, update every frame, render the picture, save /
 * load, destroy. reg->add_asset() registers it as a type of the plugin's namespace. The host implements the methods
 * scripts call (the VibeVJ adapter engine/generators/extensions/native/, or a test): describe(), set(), get(),
 * update(), render(), values(), serialize(), deserialize(). Guide: "A native VibeVJ asset" in
 * core/shizoscript3_core_cabi/README.md.
 *
 * The picture is made on the CPU: render() writes pixels into a buffer of the host (no copy; the engine uploads it as a
 * texture). Plugins get no GPU access in this version. Every callback runs on the script thread that uses the asset
 * (the thread of the VibeVJ engine), one call at a time per instance.
 *
 * C ABI 1.3 adds picture inputs (shz_asset_input_def: the engine reads a linked picture back from the GPU at the size
 * the asset asks for and gives it to update() / render() in frame->inputs) and text controls (SHZ_CONTROL_TEXT).
 *
 * Arrays of definitions (controls, values, inputs) are arrays of the struct as the plugin was compiled: the host steps
 * through them by the struct_size of their first entry (every entry has the same one) and reads only what that size
 * covers, so a plugin built against 1.2 (shorter shz_control_def, shz_asset_def) works with a 1.3 host unchanged.
 */

/* The kind of a control: the words of the shader annotations (//@slider, //@enum options=(a,b), ...) */
#define SHZ_CONTROL_SLIDER     1  /* a float with a slider, min..max (default 0..1) */
#define SHZ_CONTROL_INT        2  /* an integer field */
#define SHZ_CONTROL_FLOAT      3  /* a float field */
#define SHZ_CONTROL_BUTTON     4  /* on / off: 0 or 1 */
#define SHZ_CONTROL_PUSHBUTTON 5  /* on / off like BUTTON (a shader's //@pushbutton is the same toggle) */
#define SHZ_CONTROL_ENUM       6  /* a list: the index of the chosen entry of options */
#define SHZ_CONTROL_VEC2       7  /* 2 .. 4 float fields */
#define SHZ_CONTROL_VEC3       8
#define SHZ_CONTROL_VEC4       9
#define SHZ_CONTROL_IVEC2      10 /* 2 .. 4 integer fields */
#define SHZ_CONTROL_IVEC3      11
#define SHZ_CONTROL_IVEC4      12
#define SHZ_CONTROL_SLIDERVEC2 13 /* 2 .. 4 sliders */
#define SHZ_CONTROL_SLIDERVEC3 14
#define SHZ_CONTROL_SLIDERVEC4 15
#define SHZ_CONTROL_RGB        16 /* a colour: 3 sliders 0..1 and a colour picker */
#define SHZ_CONTROL_RGBA       17 /* the same with alpha */
#define SHZ_CONTROL_TEXT       18 /* a line of text (C ABI 1.3, SHZ_FEATURE_ASSET_TEXT): frame->texts[index], at most
                                     shz_control_def.max_length characters; v[0] of its value is its length in bytes */

#define SHZ_TEXT_DEFAULT_LENGTH 256   /* max_length 0 */
#define SHZ_TEXT_MAX_LENGTH     4096  /* the longest max_length */

/* The value of a control: v[0] for one component, v[0 .. n-1] for a vector or a colour. Ints, buttons and enums hold
 * whole numbers. The host keeps a value within min / max (sliders and colours: 0..1 when no range is given) and an
 * enum within its entries. */
typedef struct shz_control_value
{
	double v[4];
} shz_control_value;

typedef struct shz_control_def
{
	uint32_t struct_size;           /* sizeof(shz_control_def) */
	uint32_t kind;                  /* SHZ_CONTROL_* */
	const char* name;               /* the ID of the control, [A-Za-z_][A-Za-z0-9_]*: links, MIDI mappings and saved
	                                   projects find the control by it, do not rename it in a later version */
	const char* display_name;       /* NULL: made from the name like a shader uniform ("cell_size" -> "Cell size") */
	const char* doc;                /* the tooltip: one sentence */
	double min[4];                  /* the range per component; min == max (e.g. both 0): no range of its own */
	double max[4];
	double value[4];                /* the value of a new asset */
	const char* options;            /* SHZ_CONTROL_ENUM: the entries, comma separated: "Conway,HighLife,Seeds" */
	uint32_t flags;                 /* 0 */
	uint32_t reserved;              /* 0 */
	/* -- C ABI 1.3 -- */
	const char* text;               /* SHZ_CONTROL_TEXT: the text of a new asset (UTF-8; NULL: empty) */
	uint32_t max_length;            /* SHZ_CONTROL_TEXT: the most characters (Unicode code points) it holds, longer
	                                   texts are cut; 0: SHZ_TEXT_DEFAULT_LENGTH, at most SHZ_TEXT_MAX_LENGTH */
	uint32_t reserved2;             /* 0 */
} shz_control_def;

/* The size of shz_control_def in C ABI 1.2 (without text, max_length) */
#define SHZ_CONTROL_DEF_SIZE_1_2 offsetof(shz_control_def, text)

/* A value output: a number the asset gives every frame (the engine links it into controls of other generators). */
typedef struct shz_asset_value_def
{
	uint32_t struct_size;           /* sizeof(shz_asset_value_def) */
	uint32_t flags;                 /* 0 */
	const char* name;               /* the ID, like a control name */
	const char* display_name;       /* NULL: made from the name */
	const char* doc;                /* the tooltip */
	double min;                     /* the range it moves in, for the display (usually 0 and 1) */
	double max;
} shz_asset_value_def;

/* The format of the picture */
#define SHZ_PIXELS_NONE    0 /* no picture: an asset that gives values only */
#define SHZ_PIXELS_RGBA8   1 /* 4 bytes per pixel (R, G, B, A), 0..255 */
#define SHZ_PIXELS_RGBA32F 2 /* 4 floats per pixel, 0..1 (more is fine: HDR) */

/* shz_asset_def.flags */
#define SHZ_ASSET_FOLLOW_SIZE 1u /* the picture has the render size of the generator, not width x height (those are
                                    only the size until the engine sets one): render() must handle any size */

/* A picture input (C ABI 1.3, SHZ_FEATURE_ASSET_INPUTS): a port of the asset like the sampler2D input of a shader. The
 * user links a picture into it (a shader, a video, a camera, ...); the engine draws it at the size and in the format the
 * input asks for, reads it back from the GPU (a pixel buffer object, one or two frames late, no stall) and gives it to
 * update() and render() in frame->inputs: the bytes of a buffer of the host, no copy. The read back costs GPU -> CPU
 * bandwidth (width x height x 4 bytes, x 16 for RGBA32F, every frame): ask for the size the algorithm needs, not more.
 * Nothing is read while nothing is linked. */
typedef struct shz_asset_input_def
{
	uint32_t struct_size;           /* sizeof(shz_asset_input_def) */
	uint32_t flags;                 /* SHZ_INPUT_* */
	const char* name;               /* the ID of the input (its port), like a control name; unique among the controls,
	                                   values and inputs */
	const char* display_name;       /* NULL: made from the name */
	const char* doc;                /* the tooltip */
	uint32_t pixel_format;          /* SHZ_PIXELS_RGBA8 or SHZ_PIXELS_RGBA32F */
	uint32_t width;                 /* the size the asset gets the picture in (scaled by the engine) */
	uint32_t height;
	uint32_t reserved;              /* 0 */
} shz_asset_input_def;

/* shz_asset_input_def.flags */
#define SHZ_INPUT_FOLLOW_SIZE 1u /* the input has the size of the asset's picture (its render size with
                                    SHZ_ASSET_FOLLOW_SIZE) instead of width x height: an asset with a picture only */

/* The picture render() fills: the bytes of a buffer of the host, in place. Also the picture of an input. */
typedef struct shz_asset_image
{
	uint32_t struct_size;           /* set by the host */
	uint32_t format;                /* SHZ_PIXELS_RGBA8 / SHZ_PIXELS_RGBA32F */
	uint32_t width;
	uint32_t height;
	size_t stride;                  /* bytes from one row to the next (width * 4, or width * 16 for RGBA32F) */
	void* pixels;                   /* row 0 is the top row. It holds what the previous render() of this instance left
	                                   (zeros after a size change): drawing only what changed is fine */
	int32_t changed;                /* 1 when render() is called; set it to 0 when the picture did not change (the
	                                   engine then skips the upload). An input of frame->inputs: 1 when its pixels
	                                   are new since the previous frame */
	uint32_t reserved;
} shz_asset_image;

/* What create(), update() and render() get. Read-only for the plugin, except values. */
typedef struct shz_asset_frame
{
	uint32_t struct_size;           /* set by the host */
	uint32_t reserved;
	uint64_t frame;                 /* the number of update() calls before this one */
	double dt;                      /* seconds since the previous update() */
	double time;                    /* seconds: the time of the generator (VibeVJ: its Time controls, Free / Manual /
	                                   Sync with the timeline) */
	double beat;                    /* the position of the timeline in beats */
	double bpm;                     /* the tempo; 0 when the host has none */
	const shz_control_value* controls; /* one per control, in the order of shz_asset_def.controls */
	uint32_t control_count;
	uint32_t value_count;
	double* values;                 /* update(): write the value outputs here (value_count of them, in the order of
	                                   shz_asset_def.values); one not written keeps its last value. NULL elsewhere */
	uint32_t width;                 /* the size of the picture (0 without a picture) */
	uint32_t height;
	/* -- C ABI 1.3 (struct_size covers these for a plugin that declared 1.3 in its handshake) -- */
	const char* const* texts;       /* one per control: the text of a SHZ_CONTROL_TEXT control (UTF-8, 0-terminated,
	                                   valid during the callback), NULL for the other kinds; NULL when no control is a
	                                   text */
	const shz_asset_image* inputs;  /* one per picture input (input_count), in the order of shz_asset_def.inputs:
	                                   the pixels of the host's buffer (read-only, valid during the callback); pixels
	                                   NULL and width / height 0 while nothing is linked or nothing has arrived yet.
	                                   changed is 1 in the frame new pixels arrived */
	uint32_t input_count;
	uint32_t reserved2;
} shz_asset_frame;

/* create: a new instance. frame has the start values of the controls and the picture size; put the state into
 * *instance_out. On an error nothing is created and destroy is not called. */
typedef shz_result (*shz_asset_create_fn)(shz_ctx* ctx, const shz_asset_frame* frame, void** instance_out);
/* destroy: exactly once per created instance. No ctx: no host API call (release() of kept values is fine). */
typedef void (*shz_asset_destroy_fn)(void* instance, void* asset_user);
/* update: every frame, before render(). Advance the state, write the value outputs into frame->values. */
typedef shz_result (*shz_asset_update_fn)(shz_ctx* ctx, void* instance, const shz_asset_frame* frame);
/* render: the picture into image (an asset with a picture, after update(), when the engine shows it). */
typedef shz_result (*shz_asset_render_fn)(shz_ctx* ctx, void* instance, const shz_asset_frame* frame,
	shz_asset_image* image);
/* control_changed (optional): control index has a new value from a move of the user, a link or MIDI; frame->controls
 * has it already. For actions: a pushbutton that resets, an enum that changes the mode. Not called for the values a
 * project or a preset restores (deserialize(), set() with restoring = 1): read the values in update / render, and keep
 * what save() / load() restore in their state. */
typedef shz_result (*shz_asset_control_fn)(shz_ctx* ctx, void* instance, uint32_t index, const shz_asset_frame* frame);

typedef struct shz_asset_def
{
	uint32_t struct_size;           /* sizeof(shz_asset_def) */
	uint32_t flags;                 /* SHZ_ASSET_* */
	const char* name;               /* the type in the namespace: "life" in "native_life" is native_life.life */
	const char* display_name;       /* the name of the generator; NULL: the name */
	const char* doc;                /* what it does: std.docs and the tooltip */

	uint32_t pixel_format;          /* SHZ_PIXELS_*: NONE for an asset of values only */
	uint32_t width;                 /* the size of the picture (with SHZ_ASSET_FOLLOW_SIZE: until the engine sets one) */
	uint32_t height;
	uint32_t reserved;              /* 0 */

	const shz_control_def* controls;
	uint32_t control_count;
	uint32_t value_count;
	const shz_asset_value_def* values;

	shz_asset_create_fn create;     /* required */
	shz_asset_destroy_fn destroy;   /* may be NULL when the instance needs no cleanup */
	shz_asset_update_fn update;     /* may be NULL */
	shz_asset_render_fn render;     /* required with a picture */
	shz_asset_control_fn control_changed; /* may be NULL */
	/* The state of an instance beyond its control values (the host saves those itself), as plain data (NONE, INT,
	 * FLOAT, STRING, BYTES, LIST, MAP), for projects and presets: e.g. the cells of a simulation. self is the instance.
	 * NULL: the control values are the whole state. */
	shz_serialize_fn save;
	shz_deserialize_fn load;

	void* user;                     /* asset_user: host->call_user(ctx) in every callback; destroy gets it */

	/* -- C ABI 1.3 -- */
	const shz_asset_input_def* inputs; /* the picture inputs (SHZ_FEATURE_ASSET_INPUTS) */
	uint32_t input_count;
	uint32_t reserved2;             /* 0 */
} shz_asset_def;

/* The size of shz_asset_def in C ABI 1.2 (without inputs) */
#define SHZ_ASSET_DEF_SIZE_1_2 offsetof(shz_asset_def, inputs)

/* ------------------------------------------------------------------------------------------------ registration */

/*
 * Only valid inside shz_module_register. A namespace path is "name" or "name.child.child". The first part is the
 * root namespace of the plugin: it must not exist yet (std, fileio, another module's namespace are refused with
 * SHZ_ERR_STATE). All strings and arrays are copied. On an error the call registers nothing and last_error() says why.
 */
typedef struct shz_register_api
{
	uint32_t struct_size;           /* sizeof as the host knows it */
	uint32_t reserved;

	shz_result (*add_function)(shz_module* m, const char* ns, const shz_func_def* def);
	/* type_out: the handle for make_object / object_self (may be NULL when not needed) */
	shz_result (*add_type)(shz_module* m, const char* ns, const shz_type_def* def, shz_type_handle* type_out);
	/* a namespace variable (cabi_example.VERSION): NONE, BOOL, INT or FLOAT */
	shz_result (*add_constant)(shz_module* m, const char* ns, const char* name, const shz_value* value);
	shz_result (*add_constant_string)(shz_module* m, const char* ns, const char* name, const char* utf8);
	/* why the last add_* of this module failed (empty: it did not) */
	const char* (*last_error)(shz_module* m);

	/* -- C ABI 1.2 (SHZ_FEATURE_ASSETS) -- */
	/* A native VibeVJ asset: registered as the type ns.<def->name> (its constructor takes no arguments) with the
	 * methods the host implements (see shz_asset_def above). type_out may be NULL. A 1.3 host also takes inputs and
	 * text controls (SHZ_FEATURE_ASSET_INPUTS / SHZ_FEATURE_ASSET_TEXT: require them in the handshake). */
	shz_result (*add_asset)(shz_module* m, const char* ns, const shz_asset_def* def, shz_type_handle* type_out);
} shz_register_api;

/* 1 when the register API has this function (added by a minor version after 1.0): SHZ_REG_HAS(reg, add_asset) */
#define SHZ_REG_HAS(reg_, field_) \
	((reg_)->struct_size >= offsetof(shz_register_api, field_) + sizeof((reg_)->field_) && (reg_)->field_ != NULL)

/* -------------------------------------------------------------------------------------------------------- buffers */

#define SHZ_LOCK_READ  1u
#define SHZ_LOCK_WRITE 2u /* read and write */

/* The bytes of a std.buffer, in place (no copy). data stays valid until bytes_unlock(); while a buffer is locked,
 * bytes_resize() of it fails with SHZ_ERR_STATE. Unlock before the callback returns (the host unlocks a forgotten
 * lock when the callback returns and logs a warning). */
typedef struct shz_buffer_view
{
	uint32_t struct_size;           /* set by the plugin: sizeof(shz_buffer_view) */
	uint32_t mode;                  /* set by the host: the lock mode */
	uint8_t* data;                  /* NULL when size is 0 */
	size_t size;
	void* host;                     /* host private */
} shz_buffer_view;

/* -------------------------------------------------------------------------------------------------------- host api */

#define SHZ_LEVEL_VERBOSE 0
#define SHZ_LEVEL_INFO    1
#define SHZ_LEVEL_WARN    2
#define SHZ_LEVEL_ERROR   3 /* with a ctx: a non-fatal script error, printed with the call stack */

#define SHZ_LEN_CSTR ((size_t)-1) /* make_string: the text is 0-terminated */

/* file_kind (C ABI 1.1) */
#define SHZ_FILE_NONE  0 /* nothing at this path */
#define SHZ_FILE_FILE  1 /* a regular file */
#define SHZ_FILE_DIR   2 /* a directory */
#define SHZ_FILE_OTHER 3 /* something else (a device, a socket, ...) */

/*
 * The functions of the host. Given to shz_module_register, valid until shz_module_unregister returned. A function
 * added by a newer minor version may be missing in an older host: either require its feature bit in the handshake
 * (the import of the plugin then fails cleanly on an older host), or check SHZ_HOST_HAS(host, name) before using it.
 * Added in 1.1: file_kind, file_read, file_write (SHZ_FEATURE_FILES).
 * Out parameters (shz_value* out) are set to NONE on an error.
 */
typedef struct shz_host_api
{
	uint32_t struct_size;
	uint32_t abi_major;
	uint32_t abi_minor;
	uint32_t reserved0;
	uint64_t features;              /* SHZ_FEATURE_* of this host */
	const char* host_version;       /* "ShizoScript 3.2 (3001)" */

	/* -- errors, log, the call -- */
	/* Sets the message of the error the callback returns and returns code: return host->error(ctx, SHZ_ERR_ARGS,
	 * "count must be positive, got %d", n); printf format, the host prefixes the function name. */
	shz_result (*error)(shz_ctx* ctx, shz_result code, const char* fmt, ...);
	/* ctx may be NULL (then only the log of the host). */
	void (*log)(shz_ctx* ctx, int32_t level, const char* msg);
	/* user of the definition that runs: shz_func_def / shz_method_def / shz_property_def.user, type_user in a
	 * constructor or operation. */
	void* (*call_user)(shz_ctx* ctx);
	/* the object of a method / property / operation as a value (SHZ_ERR_STATE outside of those) */
	shz_result (*self)(shz_ctx* ctx, shz_value* out);

	/* -- any value -- */
	/* what std.type(v) says: "none", "int", "float", "string", "binary", "json", "cabi_example.counter", ... */
	const char* (*type_name)(const shz_value* v);
	/* the text of a value as std.string(v) makes it (a STRING) */
	shz_result (*to_string)(shz_ctx* ctx, const shz_value* v, shz_value* out);
	/* A copy of the value that stays valid after the callback (a scalar is just copied). Same as an assignment in a
	 * script: the copy of a list / map / object / buffer refers to the same container. release() it when done. */
	shz_result (*keep)(shz_ctx* ctx, const shz_value* v, shz_value* kept);
	/* releases a kept value, sets it to NONE. Script thread only; fine inside a finalizer. */
	void (*release)(shz_value* kept);

	/* -- strings -- */
	shz_result (*make_string)(shz_ctx* ctx, const char* utf8, size_t len, shz_value* out); /* len: SHZ_LEN_CSTR */
	/* the bytes of a STRING, 0-terminated, len (may be NULL) without the 0; valid as long as the value. NULL for
	 * another type. */
	const char* (*string_data)(const shz_value* v, size_t* len);

	/* -- bytes (std.buffer) -- */
	shz_result (*make_bytes)(shz_ctx* ctx, const void* data, size_t len, shz_value* out); /* data NULL: zeros */
	shz_result (*bytes_size)(const shz_value* v, size_t* size);
	shz_result (*bytes_resize)(shz_ctx* ctx, const shz_value* v, size_t size); /* new bytes are 0 */
	shz_result (*bytes_lock)(shz_ctx* ctx, const shz_value* v, uint32_t mode, shz_buffer_view* view);
	shz_result (*bytes_unlock)(shz_ctx* ctx, shz_buffer_view* view);

	/* -- lists (a map can be read by position too) -- */
	shz_result (*make_list)(shz_ctx* ctx, shz_value* out);
	/* entries of a LIST / MAP, bytes of a STRING / BYTES */
	shz_result (*count)(const shz_value* v, size_t* n);
	/* the element itself (changing it through list_set is seen by all holders of the list) */
	shz_result (*list_get)(shz_ctx* ctx, const shz_value* list, size_t index, shz_value* out);
	shz_result (*list_set)(shz_ctx* ctx, const shz_value* list, size_t index, const shz_value* value);
	shz_result (*list_push)(shz_ctx* ctx, const shz_value* list, const shz_value* value);
	shz_result (*list_remove)(shz_ctx* ctx, const shz_value* list, size_t index);

	/* -- maps (JSON objects) -- */
	shz_result (*make_map)(shz_ctx* ctx, shz_value* out);
	shz_result (*map_get)(shz_ctx* ctx, const shz_value* map, const char* key, shz_value* out); /* SHZ_ERR_NOT_FOUND */
	shz_result (*map_set)(shz_ctx* ctx, const shz_value* map, const char* key, const shz_value* value);
	shz_result (*map_remove)(shz_ctx* ctx, const shz_value* map, const char* key); /* SHZ_ERR_NOT_FOUND */
	/* entry index of a MAP or LIST: key_out a STRING (NONE for an entry without key), value_out the value; both may
	 * be NULL */
	shz_result (*map_entry)(shz_ctx* ctx, const shz_value* map, size_t index, shz_value* key_out, shz_value* value_out);

	/* -- objects of this plugin's types -- */
	/* A new object of the type with this state; from now on the host owns it and calls finalize(self) once. */
	shz_result (*make_object)(shz_ctx* ctx, shz_type_handle type, void* self, shz_value* out);
	/* the state of an object of this type, NULL for another type or a destroyed object */
	void* (*object_self)(const shz_value* v, shz_type_handle type);

	/* -- calling script functions -- */
	/* Starts fn (FUNCTION) with these arguments as a new task of the script: it runs now until it ends or waits
	 * (fileio, std.sleep, ...), the rest runs later. done (may be NULL) gets the return value when it ended. */
	shz_result (*call)(shz_ctx* ctx, const shz_value* fn, const shz_value* args, uint32_t argc, shz_done_fn done,
		void* done_user);

	/* -- files (C ABI 1.1, SHZ_FEATURE_FILES) -- */
	/* Through the file system the host gives the script, the one fileio and std use: a relative path is relative to
	 * the same place, and a host that limits or redirects the file access of a script (an embedded file system, a
	 * sandbox) does so for the plugin too. Prefer these to fopen() for the files a script names. */
	/* what is at the path: SHZ_FILE_NONE / FILE / DIR / OTHER (SHZ_OK in every case) */
	shz_result (*file_kind)(shz_ctx* ctx, const char* path, uint32_t* kind_out);
	/* the whole file as BYTES (a new value of the callback); SHZ_ERR_NOT_FOUND when it is no file (missing, a dir) */
	shz_result (*file_read)(shz_ctx* ctx, const char* path, shz_value* out);
	/* creates or replaces the file with these bytes (data may be NULL when len is 0), creates its folder;
	 * SHZ_ERR_RUNTIME when it cannot be written */
	shz_result (*file_write)(shz_ctx* ctx, const char* path, const void* data, size_t len);
} shz_host_api;

/* 1 when the host API has this function (added by a minor version after 1.0): SHZ_HOST_HAS(host, file_read) */
#define SHZ_HOST_HAS(host_, field_) \
	((host_)->struct_size >= offsetof(shz_host_api, field_) + sizeof((host_)->field_) && (host_)->field_ != NULL)

/* ------------------------------------------------------------------------------------------- handshake, entry points */

typedef struct shz_abi_handshake
{
	uint32_t struct_size;               /* set by the host: sizeof as the host knows it */
	uint32_t host_abi_major;            /* set by the host */
	uint32_t host_abi_minor;            /* set by the host */
	uint32_t plugin_abi_major;          /* set by the plugin: SHZ_ABI_VERSION_MAJOR */
	uint32_t plugin_abi_minor;          /* set by the plugin: SHZ_ABI_VERSION_MINOR */
	uint32_t reserved0;
	uint64_t host_features;             /* set by the host: SHZ_FEATURE_* */
	uint64_t plugin_required_features;  /* set by the plugin: the SHZ_FEATURE_* it cannot work without */
	const char* host_version;           /* set by the host */
	const char* plugin_name;            /* set by the plugin: for messages and crash reports (static text) */
	const char* plugin_version;         /* set by the plugin: free text, e.g. "1.2.0" (static text) */
} shz_abi_handshake;

/* The entry points a plugin exports (the host looks them up by name). */
typedef shz_result (*shz_module_abi_fn)(shz_abi_handshake* hs);
typedef shz_result (*shz_module_register_fn)(shz_module* self, const shz_host_api* host, const shz_register_api* reg);
typedef void (*shz_module_unregister_fn)(shz_module* self);

#ifndef SHZ_ABI_HOST
/* Required. Fill the plugin fields of hs (only those within hs->struct_size) and return SHZ_OK, or SHZ_ERR_VERSION
 * when the host is too old for the plugin (the host checks major / minor / features too). Called before anything
 * else, must not do anything else. SHZ_MODULE_ABI() writes it. */
SHZ_PLUGIN_EXPORT shz_result shz_module_abi(shz_abi_handshake* hs);
/* Required. Register everything, keep host (and self). Return SHZ_OK, or an error code: then the import fails and
 * nothing is registered. */
SHZ_PLUGIN_EXPORT shz_result shz_module_register(shz_module* self, const shz_host_api* host, const shz_register_api* reg);
/* Optional. The host drops the plugin: release kept values, free global state, stop the threads of the plugin. The
 * host has finalized every live object of the plugin's types before (their values read as destroyed objects
 * afterwards). Before a reload (std.native_reload, C ABI 1.3 hosts) the host has destroyed the instances of the
 * plugin's assets (save() first, the new library create()s and load()s them again); after this call the library is
 * unloaded: nothing of it may run any more. */
SHZ_PLUGIN_EXPORT void shz_module_unregister(shz_module* self);
#endif

/* shz_module_abi for a plugin of this header's version: SHZ_MODULE_ABI("my_plugin", "1.0.0", SHZ_FEATURE_FUNCTIONS) */
#define SHZ_MODULE_ABI(plugin_name_, plugin_version_, required_features_) \
	SHZ_PLUGIN_API shz_result shz_module_abi(shz_abi_handshake* hs) \
	{ \
		if (!hs || hs->struct_size < offsetof(shz_abi_handshake, plugin_version) + sizeof(const char*)) \
			return SHZ_ERR_VERSION; \
		hs->plugin_abi_major = SHZ_ABI_VERSION_MAJOR; \
		hs->plugin_abi_minor = SHZ_ABI_VERSION_MINOR; \
		hs->plugin_required_features = (required_features_); \
		hs->plugin_name = (plugin_name_); \
		hs->plugin_version = (plugin_version_); \
		return SHZ_OK; \
	}

#ifdef __cplusplus
}
#endif

#endif /* SHZ_ABI_H */
