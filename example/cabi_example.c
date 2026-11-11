/*
 * cabi_example: a ShizoScript native plugin in plain C99, built only against shz_abi.h (issue #201).
 *
 * It shows every part of the C ABI and is what _testing/cabi_plugin.shio checks:
 *   - functions (fixed, optional and variadic arguments), a child namespace, constants, doc strings
 *   - catchable errors (host->error) and a non-fatal error (host->log)
 *   - values in and out: numbers, strings, lists, maps, buffers, objects, functions
 *   - zero-copy buffer access (bytes_lock / bytes_unlock)
 *   - a type with constructor, methods, properties, finalizer, to_string, index, size, compare, arith,
 *     serialize / deserialize, and a value kept across calls (keep / release)
 *   - starting a script function (host->call)
 *   - a native VibeVJ asset with one control of every kind (C ABI 1.2, reg->add_asset; _testing/native_asset.shio)
 *   - a native asset with picture inputs and a text control (C ABI 1.3, probe), one declared with the shorter structs
 *     of C ABI 1.2 (legacy)
 *
 * Built a second time by _testing/native_asset.shio as the plugin cabi_reload (-DNS='"cabi_reload"') for the test of
 * std.native_reload: CABI_EXAMPLE_VARIANT changes a constant (2), drops a function (3) or gives the probe one control more
 * (4: the smoke test of the app builds its window again, _testing/linux_app/smoke.py phase native).
 *
 * Build (Linux): gcc -std=c99 -shared -fPIC -I<sdk>/include cabi_example.c -o shzmodule_cabi_example.so
 * Package:       cabi_example/package.shz, cabi_example/native/linux_x64/shzmodule_cabi_example.so
 * See core/shizoscript3_core_cabi/README.md.
 */

#include "shz_abi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef NS
#define NS "cabi_example"
#endif
#ifndef CABI_EXAMPLE_VARIANT
#define CABI_EXAMPLE_VARIANT 1
#endif
#define CABI_EXAMPLE_STR2(x) #x
#define CABI_EXAMPLE_STR(x) CABI_EXAMPLE_STR2(x)

static const shz_host_api* host = NULL; /* valid from shz_module_register to shz_module_unregister */
static shz_type_handle counter_type = NULL;
static int64_t counters_alive = 0;
static int64_t counters_finalized = 0;
static shz_value last_callback_result; /* kept: the result of the last apply() */

/* ------------------------------------------------------------------------------------------------ handshake */

/* SHZ_MODULE_ABI("cabi_example", "1.0.0", SHZ_FEATURES_1_0) writes this function; spelled out here to show it.
 * CABI_EXAMPLE_ABI_MAJOR builds a plugin of another ABI version, which the host must refuse (the test does that). */
#ifndef CABI_EXAMPLE_ABI_MAJOR
#define CABI_EXAMPLE_ABI_MAJOR SHZ_ABI_VERSION_MAJOR
#endif

SHZ_PLUGIN_API shz_result shz_module_abi(shz_abi_handshake* hs)
{
	/* only write what the host's struct has */
	if (!hs || hs->struct_size < offsetof(shz_abi_handshake, plugin_version) + sizeof(const char*))
		return SHZ_ERR_VERSION;
	hs->plugin_abi_major = CABI_EXAMPLE_ABI_MAJOR;
	hs->plugin_abi_minor = SHZ_ABI_VERSION_MINOR;
	hs->plugin_required_features = SHZ_FEATURE_FUNCTIONS | SHZ_FEATURE_TYPES | SHZ_FEATURE_BUFFERS;
	hs->plugin_name = NS;
	hs->plugin_version = CABI_EXAMPLE_VARIANT == 1 ? "1.0.0" : "1.0.0-variant" CABI_EXAMPLE_STR(CABI_EXAMPLE_VARIANT);
	return SHZ_OK;
}

/* ------------------------------------------------------------------------------------------------ functions */

/* add(a, b): int + int is an int, anything with a float a float */
static shz_result fn_add(shz_ctx* ctx, const shz_value* args, uint32_t argc, shz_value* out)
{
	(void)argc;
	if (!shz_is_number(&args[0]) || !shz_is_number(&args[1]))
		return host->error(ctx, SHZ_ERR_ARGS, "add needs two numbers, got %s and %s", host->type_name(&args[0]),
			host->type_name(&args[1]));
	if (args[0].type == SHZ_T_FLOAT || args[1].type == SHZ_T_FLOAT)
		*out = shz_float(shz_to_double(&args[0]) + shz_to_double(&args[1]));
	else
		*out = shz_int(args[0].as.i + args[1].as.i);
	return SHZ_OK;
}

/* divide(a, b): a catchable error for b == 0 */
static shz_result fn_divide(shz_ctx* ctx, const shz_value* args, uint32_t argc, shz_value* out)
{
	double b;
	(void)argc;
	if (!shz_is_number(&args[0]) || !shz_is_number(&args[1]))
		return host->error(ctx, SHZ_ERR_ARGS, "divide needs two numbers");
	b = shz_to_double(&args[1]);
	if (b == 0.0)
		return host->error(ctx, SHZ_ERR_RUNTIME, "division by zero");
	*out = shz_float(shz_to_double(&args[0]) / b);
	return SHZ_OK;
}

/* echo(v): the value back, whatever it is (round trip of every type) */
static shz_result fn_echo(shz_ctx* ctx, const shz_value* args, uint32_t argc, shz_value* out)
{
	(void)ctx;
	(void)argc;
	*out = args[0];
	return SHZ_OK;
}

/* kind(v): the C ABI type of a value, as a name */
static shz_result fn_kind(shz_ctx* ctx, const shz_value* args, uint32_t argc, shz_value* out)
{
	static const char* names[] = { "none", "bool", "int", "float", "string", "bytes", "list", "map", "object",
		"function", "other" };
	(void)argc;
	return host->make_string(ctx, args[0].type <= SHZ_T_OTHER ? names[args[0].type] : "?", SHZ_LEN_CSTR, out);
}

/* concat(...): the texts of all arguments, joined (variadic) */
static shz_result fn_concat(shz_ctx* ctx, const shz_value* args, uint32_t argc, shz_value* out)
{
	size_t total = 0, used = 0;
	uint32_t i;
	char* buffer;
	shz_result r;
	shz_value* texts = (shz_value*)calloc(argc ? argc : 1, sizeof(shz_value));
	if (!texts)
		return SHZ_ERR_NOMEM;
	for (i = 0; i < argc; i++)
	{
		size_t len = 0;
		r = host->to_string(ctx, &args[i], &texts[i]);
		if (r != SHZ_OK)
		{
			free(texts);
			return r;
		}
		host->string_data(&texts[i], &len);
		total += len;
	}
	buffer = (char*)malloc(total + 1);
	if (!buffer)
	{
		free(texts);
		return SHZ_ERR_NOMEM;
	}
	for (i = 0; i < argc; i++)
	{
		size_t len = 0;
		const char* text = host->string_data(&texts[i], &len);
		memcpy(buffer + used, text, len);
		used += len;
	}
	buffer[used] = 0;
	r = host->make_string(ctx, buffer, used, out); /* the host copies it */
	free(buffer);
	free(texts);
	return r;
}

/* upper(s): a string in, a new string out */
static shz_result fn_upper(shz_ctx* ctx, const shz_value* args, uint32_t argc, shz_value* out)
{
	size_t len = 0, i;
	const char* text = host->string_data(&args[0], &len);
	char* copy;
	shz_result r;
	(void)argc;
	if (!text)
		return host->error(ctx, SHZ_ERR_ARGS, "upper needs a string, got %s", host->type_name(&args[0]));
	copy = (char*)malloc(len + 1);
	if (!copy)
		return SHZ_ERR_NOMEM;
	for (i = 0; i < len; i++)
		copy[i] = (text[i] >= 'a' && text[i] <= 'z') ? (char)(text[i] - 'a' + 'A') : text[i];
	r = host->make_string(ctx, copy, len, out);
	free(copy);
	return r;
}

/* stats(list): [count = n, sum = s, min = a, max = b] of a list of numbers */
static shz_result fn_stats(shz_ctx* ctx, const shz_value* args, uint32_t argc, shz_value* out)
{
	size_t n = 0, i;
	double sum = 0, lo = 0, hi = 0;
	shz_value result, v;
	(void)argc;
	if (args[0].type != SHZ_T_LIST && args[0].type != SHZ_T_MAP)
		return host->error(ctx, SHZ_ERR_ARGS, "stats needs a list, got %s", host->type_name(&args[0]));
	host->count(&args[0], &n);
	for (i = 0; i < n; i++)
	{
		double d;
		host->list_get(ctx, &args[0], i, &v);
		if (!shz_is_number(&v))
			return host->error(ctx, SHZ_ERR_TYPE, "stats: element %d is a %s, not a number", (int)i, host->type_name(&v));
		d = shz_to_double(&v);
		sum += d;
		if (i == 0 || d < lo)
			lo = d;
		if (i == 0 || d > hi)
			hi = d;
	}
	host->make_map(ctx, &result);
	v = shz_int((int64_t)n);
	host->map_set(ctx, &result, "count", &v);
	v = shz_float(sum);
	host->map_set(ctx, &result, "sum", &v);
	v = shz_float(lo);
	host->map_set(ctx, &result, "min", &v);
	v = shz_float(hi);
	host->map_set(ctx, &result, "max", &v);
	*out = result;
	return SHZ_OK;
}

/* keys(map): the keys of a map as a list (map_entry) */
static shz_result fn_keys(shz_ctx* ctx, const shz_value* args, uint32_t argc, shz_value* out)
{
	size_t n = 0, i;
	shz_value list, key;
	(void)argc;
	if (args[0].type != SHZ_T_MAP && args[0].type != SHZ_T_LIST)
		return host->error(ctx, SHZ_ERR_ARGS, "keys needs a map, got %s", host->type_name(&args[0]));
	host->count(&args[0], &n);
	host->make_list(ctx, &list);
	for (i = 0; i < n; i++)
	{
		host->map_entry(ctx, &args[0], i, &key, NULL);
		host->list_push(ctx, &list, &key);
	}
	*out = list;
	return SHZ_OK;
}

/* invert(buffer): inverts every byte in place (zero copy), returns the number of bytes */
static shz_result fn_invert(shz_ctx* ctx, const shz_value* args, uint32_t argc, shz_value* out)
{
	shz_buffer_view view;
	size_t i;
	shz_result r;
	(void)argc;
	memset(&view, 0, sizeof(view));
	SHZ_INIT(view);
	r = host->bytes_lock(ctx, &args[0], SHZ_LOCK_WRITE, &view);
	if (r != SHZ_OK)
		return host->error(ctx, r, "invert needs a std.buffer, got %s", host->type_name(&args[0]));
	for (i = 0; i < view.size; i++)
		view.data[i] = (uint8_t)~view.data[i];
	*out = shz_int((int64_t)view.size);
	return host->bytes_unlock(ctx, &view);
}

/* resize_locked(buffer): resizing a locked buffer must fail (SHZ_ERR_STATE); returns 1 when it did */
static shz_result fn_resize_locked(shz_ctx* ctx, const shz_value* args, uint32_t argc, shz_value* out)
{
	shz_buffer_view view;
	shz_result r;
	(void)argc;
	memset(&view, 0, sizeof(view));
	SHZ_INIT(view);
	r = host->bytes_lock(ctx, &args[0], SHZ_LOCK_READ, &view);
	if (r != SHZ_OK)
		return r;
	r = host->bytes_resize(ctx, &args[0], view.size + 10);
	host->bytes_unlock(ctx, &view);
	*out = shz_bool(r == SHZ_ERR_STATE);
	return SHZ_OK;
}

/* make_bytes(n): a new buffer with the bytes 0, 1, 2, ... */
static shz_result fn_make_bytes(shz_ctx* ctx, const shz_value* args, uint32_t argc, shz_value* out)
{
	shz_buffer_view view;
	size_t i;
	int64_t n = shz_to_int(&args[0]);
	shz_result r;
	(void)argc;
	if (n < 0 || n > 1 << 24)
		return host->error(ctx, SHZ_ERR_RANGE, "make_bytes: %d is not a size", (int)n);
	r = host->make_bytes(ctx, NULL, (size_t)n, out);
	if (r != SHZ_OK)
		return r;
	memset(&view, 0, sizeof(view));
	SHZ_INIT(view);
	r = host->bytes_lock(ctx, out, SHZ_LOCK_WRITE, &view);
	if (r != SHZ_OK)
		return r;
	for (i = 0; i < view.size; i++)
		view.data[i] = (uint8_t)i;
	return host->bytes_unlock(ctx, &view);
}

/* warn_nonfatal(): a non-fatal error (logged with the call stack, not catchable), returns 1 */
static shz_result fn_warn_nonfatal(shz_ctx* ctx, const shz_value* args, uint32_t argc, shz_value* out)
{
	(void)args;
	(void)argc;
	host->log(ctx, SHZ_LEVEL_ERROR, "a non-fatal error of the example plugin (the script goes on)");
	*out = shz_int(1);
	return SHZ_OK;
}

static void apply_done(shz_ctx* ctx, const shz_value* result, void* user)
{
	(void)user;
	host->release(&last_callback_result);
	host->keep(ctx, result, &last_callback_result);
}

/* apply(fn, x): starts fn(x); its result is read with last_result() */
static shz_result fn_apply(shz_ctx* ctx, const shz_value* args, uint32_t argc, shz_value* out)
{
	(void)argc;
	*out = shz_int(1);
	return host->call(ctx, &args[0], &args[1], 1, apply_done, NULL);
}

static shz_result fn_last_result(shz_ctx* ctx, const shz_value* args, uint32_t argc, shz_value* out)
{
	(void)ctx;
	(void)args;
	(void)argc;
	*out = last_callback_result; /* the host copies it for the script */
	return SHZ_OK;
}

static shz_result fn_counters_alive(shz_ctx* ctx, const shz_value* args, uint32_t argc, shz_value* out)
{
	(void)ctx;
	(void)args;
	(void)argc;
	*out = shz_int(counters_alive);
	return SHZ_OK;
}

static shz_result fn_counters_finalized(shz_ctx* ctx, const shz_value* args, uint32_t argc, shz_value* out)
{
	(void)ctx;
	(void)args;
	(void)argc;
	*out = shz_int(counters_finalized);
	return SHZ_OK;
}

/* math.square(x): a function of a child namespace; user carries the factor (shz_func_def.user) */
static shz_result fn_scaled_square(shz_ctx* ctx, const shz_value* args, uint32_t argc, shz_value* out)
{
	double factor = *(const double*)host->call_user(ctx);
	double x;
	(void)argc;
	if (!shz_is_number(&args[0]))
		return host->error(ctx, SHZ_ERR_ARGS, "needs a number");
	x = shz_to_double(&args[0]);
	*out = shz_float(factor * x * x);
	return SHZ_OK;
}

/* ------------------------------------------------------------------------------------------------ the counter type */

typedef struct counter
{
	int64_t count;
	int64_t step;
	shz_value history; /* kept LIST of the values after each add() */
} counter;

static shz_result counter_construct(shz_ctx* ctx, const shz_value* args, uint32_t argc, void** self_out)
{
	counter* c;
	shz_value list;
	if (argc >= 1 && !shz_is_number(&args[0]))
		return host->error(ctx, SHZ_ERR_ARGS, "counter(start, step): start must be a number, got %s", host->type_name(&args[0]));
	if (argc >= 2 && !shz_is_number(&args[1]))
		return host->error(ctx, SHZ_ERR_ARGS, "counter(start, step): step must be a number");
	c = (counter*)calloc(1, sizeof(counter));
	if (!c)
		return SHZ_ERR_NOMEM;
	c->count = argc >= 1 ? shz_to_int(&args[0]) : 0;
	c->step = argc >= 2 ? shz_to_int(&args[1]) : 1;
	host->make_list(ctx, &list);
	host->keep(ctx, &list, &c->history); /* lives longer than this call: released in the finalizer */
	counters_alive++;
	*self_out = c;
	return SHZ_OK;
}

static void counter_finalize(void* self, void* type_user)
{
	counter* c = (counter*)self;
	(void)type_user;
	if (!c)
		return;
	host->release(&c->history);
	free(c);
	counters_alive--;
	counters_finalized++;
}

/* add(n = step): adds, returns the counter (chaining: c.add().add(5)) */
static shz_result counter_add(shz_ctx* ctx, void* self, const shz_value* args, uint32_t argc, shz_value* out)
{
	counter* c = (counter*)self;
	shz_value v;
	if (argc >= 1 && !shz_is_number(&args[0]))
		return host->error(ctx, SHZ_ERR_ARGS, "add(n): n must be a number, got %s", host->type_name(&args[0]));
	c->count += argc >= 1 ? shz_to_int(&args[0]) : c->step;
	v = shz_int(c->count);
	host->list_push(ctx, &c->history, &v);
	return host->self(ctx, out);
}

static shz_result counter_reset(shz_ctx* ctx, void* self, const shz_value* args, uint32_t argc, shz_value* out)
{
	counter* c = (counter*)self;
	size_t n = 0;
	(void)args;
	(void)argc;
	c->count = 0;
	host->count(&c->history, &n);
	while (n > 0)
		host->list_remove(ctx, &c->history, --n);
	return host->self(ctx, out);
}

static shz_result counter_history(shz_ctx* ctx, void* self, const shz_value* args, uint32_t argc, shz_value* out)
{
	counter* c = (counter*)self;
	(void)ctx;
	(void)args;
	(void)argc;
	*out = c->history; /* the same list: the script sees later add()s in it */
	return SHZ_OK;
}

/* same(other): whether other is this very counter (object_self) */
static shz_result counter_same(shz_ctx* ctx, void* self, const shz_value* args, uint32_t argc, shz_value* out)
{
	(void)ctx;
	(void)argc;
	*out = shz_bool(host->object_self(&args[0], counter_type) == self);
	return SHZ_OK;
}

static shz_result counter_get_count(shz_ctx* ctx, void* self, shz_value* out)
{
	(void)ctx;
	*out = shz_int(((counter*)self)->count);
	return SHZ_OK;
}

static shz_result counter_set_count(shz_ctx* ctx, void* self, const shz_value* value)
{
	if (!shz_is_number(value))
		return host->error(ctx, SHZ_ERR_TYPE, "count must be a number, got %s", host->type_name(value));
	((counter*)self)->count = shz_to_int(value);
	return SHZ_OK;
}

static shz_result counter_get_step(shz_ctx* ctx, void* self, shz_value* out)
{
	(void)ctx;
	*out = shz_int(((counter*)self)->step);
	return SHZ_OK;
}

static shz_result counter_set_step(shz_ctx* ctx, void* self, const shz_value* value)
{
	if (!shz_is_number(value))
		return host->error(ctx, SHZ_ERR_TYPE, "step must be a number");
	((counter*)self)->step = shz_to_int(value);
	return SHZ_OK;
}

static shz_result counter_get_kind(shz_ctx* ctx, void* self, shz_value* out)
{
	(void)self;
	return host->make_string(ctx, "counter", SHZ_LEN_CSTR, out);
}

static shz_result counter_to_string(shz_ctx* ctx, void* self, shz_value* out)
{
	char text[64];
	snprintf(text, sizeof(text), "counter(%lld)", (long long)((counter*)self)->count);
	return host->make_string(ctx, text, SHZ_LEN_CSTR, out);
}

/* c[i]: the i-th value of the history */
static shz_result counter_index_get(shz_ctx* ctx, void* self, const shz_value* key, shz_value* out)
{
	counter* c = (counter*)self;
	size_t n = 0;
	if (key->type != SHZ_T_INT)
		return host->error(ctx, SHZ_ERR_NOT_FOUND, "counter has no member '%s'",
			key->type == SHZ_T_STRING ? host->string_data(key, NULL) : host->type_name(key));
	host->count(&c->history, &n);
	if (key->as.i < 0 || (size_t)key->as.i >= n)
		return host->error(ctx, SHZ_ERR_RANGE, "index %lld out of range (history has %d values)", (long long)key->as.i, (int)n);
	return host->list_get(ctx, &c->history, (size_t)key->as.i, out);
}

static shz_result counter_size(shz_ctx* ctx, void* self, uint64_t* size_out)
{
	size_t n = 0;
	(void)ctx;
	host->count(&((counter*)self)->history, &n);
	*size_out = n;
	return SHZ_OK;
}

static shz_result counter_compare(shz_ctx* ctx, void* self, const shz_value* other, int32_t* order)
{
	counter* a = (counter*)self;
	counter* b = (counter*)host->object_self(other, counter_type);
	(void)ctx;
	if (!b)
		return SHZ_ERR_UNSUPPORTED;
	*order = a->count < b->count ? -1 : a->count > b->count ? 1 : 0;
	return SHZ_OK;
}

/* counter + n, n + counter, counter - n, counter * n: an int */
static shz_result counter_arith(shz_ctx* ctx, void* self, uint32_t op, const shz_value* other, int32_t self_is_left,
	shz_value* out)
{
	int64_t a = ((counter*)self)->count, b;
	(void)ctx;
	if (other->type != SHZ_T_INT)
		return SHZ_ERR_UNSUPPORTED;
	b = other->as.i;
	if (!self_is_left)
	{
		int64_t t = a;
		a = b;
		b = t;
	}
	switch (op)
	{
	case SHZ_ARITH_ADD: *out = shz_int(a + b); return SHZ_OK;
	case SHZ_ARITH_SUB: *out = shz_int(a - b); return SHZ_OK;
	case SHZ_ARITH_MUL: *out = shz_int(a * b); return SHZ_OK;
	default: return SHZ_ERR_UNSUPPORTED;
	}
}

static shz_result counter_serialize(shz_ctx* ctx, void* self, shz_value* out)
{
	counter* c = (counter*)self;
	shz_value v;
	host->make_map(ctx, out);
	v = shz_int(c->count);
	host->map_set(ctx, out, "count", &v);
	v = shz_int(c->step);
	host->map_set(ctx, out, "step", &v);
	return SHZ_OK;
}

static shz_result counter_deserialize(shz_ctx* ctx, void* self, const shz_value* data)
{
	counter* c = (counter*)self;
	shz_value v;
	if (data->type != SHZ_T_MAP)
		return host->error(ctx, SHZ_ERR_TYPE, "deserialize needs what serialize() returned (a map), got %s", host->type_name(data));
	if (host->map_get(ctx, data, "count", &v) == SHZ_OK && shz_is_number(&v))
		c->count = shz_to_int(&v);
	if (host->map_get(ctx, data, "step", &v) == SHZ_OK && shz_is_number(&v))
		c->step = shz_to_int(&v);
	return SHZ_OK;
}

/* ------------------------------------------------------------------------------- a native VibeVJ asset (C ABI 1.2) */

/* cabi_example.gradient: a native asset with one control of every kind (_testing/native_asset.shio checks them). A
 * gradient from colour_a to colour_b, in float pixels (RGBA32F) at any size (SHZ_ASSET_FOLLOW_SIZE). A real asset with
 * a walkthrough: _shizoengine3/assets/Generators/Native/Life.asset. */

enum { G_MIX, G_STEPS, G_GAIN, G_INVERT, G_FLASH, G_MODE, G_CENTER, G_TILT, G_WEIGHTS, G_CELLS, G_I3, G_I4, G_AMOUNT2,
	G_AMOUNT3, G_AMOUNT4, G_COLOR_A, G_COLOR_B, G_CONTROLS };

#define G_CONTROL(kind_, name_, min_, max_, ...) \
	{ sizeof(shz_control_def), kind_, name_, NULL, "test control " name_, { min_, min_, min_, min_ }, \
		{ max_, max_, max_, max_ }, { __VA_ARGS__ }, NULL, 0, 0, NULL, 0, 0 }

static const shz_control_def gradient_controls[G_CONTROLS] = {
	G_CONTROL(SHZ_CONTROL_SLIDER, "mix", 0, 0, 0.5),
	G_CONTROL(SHZ_CONTROL_INT, "steps", 1, 16, 4),
	G_CONTROL(SHZ_CONTROL_FLOAT, "gain", 0, 0, 1),
	G_CONTROL(SHZ_CONTROL_BUTTON, "invert", 0, 0, 0),
	G_CONTROL(SHZ_CONTROL_PUSHBUTTON, "flash", 0, 0, 0),
	{ sizeof(shz_control_def), SHZ_CONTROL_ENUM, "mode", "Direction", "Horizontal or vertical", { 0 }, { 0 }, { 0 },
		"Horizontal, Vertical", 0, 0, NULL, 0, 0 },
	G_CONTROL(SHZ_CONTROL_VEC2, "center", 0, 0, 0.5, 0.5),
	G_CONTROL(SHZ_CONTROL_VEC3, "tilt", 0, 0, 1, 2, 3),
	G_CONTROL(SHZ_CONTROL_VEC4, "weights", 0, 0, 1, 1, 1, 1),
	G_CONTROL(SHZ_CONTROL_IVEC2, "cells", 1, 64, 8, 4),
	G_CONTROL(SHZ_CONTROL_IVEC3, "i3", 0, 0, 1, 2, 3),
	G_CONTROL(SHZ_CONTROL_IVEC4, "i4", 0, 0, 1, 2, 3, 4),
	G_CONTROL(SHZ_CONTROL_SLIDERVEC2, "amount2", -1, 1, 0, 0.5),
	G_CONTROL(SHZ_CONTROL_SLIDERVEC3, "amount3", 0, 0, 0.1, 0.2, 0.3),
	G_CONTROL(SHZ_CONTROL_SLIDERVEC4, "amount4", 0, 0, 0.1, 0.2, 0.3, 0.4),
	G_CONTROL(SHZ_CONTROL_RGB, "color_a", 0, 0, 0, 0, 0),
	G_CONTROL(SHZ_CONTROL_RGBA, "color_b", 0, 0, 1, 1, 1, 1),
};

static const shz_asset_value_def gradient_values[] = {
	{ sizeof(shz_asset_value_def), 0, "phase", NULL, "The fraction of the time, 0 to 1", 0, 1 },
	{ sizeof(shz_asset_value_def), 0, "changes", NULL, "How often a control changed (control_changed calls)", 0, 100 },
};

typedef struct gradient
{
	int64_t renders;
	int64_t changes;
} gradient;

static int64_t gradients_alive = 0;

/* floor() without libm (the plugin links nothing) */
static double g_floor(double v)
{
	double i = (double)(int64_t)v;
	return i > v ? i - 1 : i;
}

static shz_result gradient_create(shz_ctx* ctx, const shz_asset_frame* frame, void** instance_out)
{
	gradient* g = (gradient*)calloc(1, sizeof(gradient));
	(void)frame;
	if (!g)
		return host->error(ctx, SHZ_ERR_NOMEM, "out of memory");
	gradients_alive++;
	*instance_out = g;
	return SHZ_OK;
}

static void gradient_destroy(void* instance, void* asset_user)
{
	(void)asset_user;
	gradients_alive--;
	free(instance);
}

static shz_result gradient_update(shz_ctx* ctx, void* instance, const shz_asset_frame* frame)
{
	gradient* g = (gradient*)instance;
	(void)ctx;
	frame->values[0] = frame->time - g_floor(frame->time);
	frame->values[1] = (double)g->changes;
	return SHZ_OK;
}

static shz_result gradient_render(shz_ctx* ctx, void* instance, const shz_asset_frame* frame, shz_asset_image* image)
{
	gradient* g = (gradient*)instance;
	const shz_control_value* c = frame->controls;
	const double steps = c[G_STEPS].v[0], gain = c[G_GAIN].v[0];
	uint32_t x, y, k;
	(void)ctx;
	for (y = 0; y < image->height; y++)
	{
		float* row = (float*)((uint8_t*)image->pixels + y * image->stride);
		for (x = 0; x < image->width; x++)
		{
			double t = c[G_MODE].v[0] == 0 ? (image->width > 1 ? (double)x / (image->width - 1) : 0)
				: (image->height > 1 ? (double)y / (image->height - 1) : 0);
			t = g_floor(t * steps + 0.5) / steps;
			if (c[G_INVERT].v[0] != 0)
				t = 1 - t;
			for (k = 0; k < 3; k++)
				row[x * 4 + k] = (float)((c[G_COLOR_A].v[k] + (c[G_COLOR_B].v[k] - c[G_COLOR_A].v[k]) * t) * gain);
			row[x * 4 + 3] = (float)c[G_COLOR_B].v[3];
		}
	}
	g->renders++;
	return SHZ_OK;
}

static shz_result gradient_control_changed(shz_ctx* ctx, void* instance, uint32_t index, const shz_asset_frame* frame)
{
	(void)ctx;
	(void)index;
	(void)frame;
	((gradient*)instance)->changes++;
	return SHZ_OK;
}

static shz_result gradient_save(shz_ctx* ctx, void* self, shz_value* out)
{
	(void)ctx;
	*out = shz_int(((gradient*)self)->renders);
	return SHZ_OK;
}

static shz_result gradient_load(shz_ctx* ctx, void* self, const shz_value* data)
{
	if (!shz_is_number(data))
		return host->error(ctx, SHZ_ERR_TYPE, "the state is the number of renders, not a %s", host->type_name(data));
	((gradient*)self)->renders = shz_to_int(data);
	return SHZ_OK;
}

static shz_result fn_gradients_alive(shz_ctx* ctx, const shz_value* args, uint32_t argc, shz_value* out)
{
	(void)ctx;
	(void)args;
	(void)argc;
	*out = shz_int(gradients_alive);
	return SHZ_OK;
}

/* A native asset is optional for this plugin: registered when the host has add_asset (C ABI 1.2) */
static shz_result add_gradient(shz_module* self, const shz_register_api* reg)
{
	shz_asset_def def;
	if (!SHZ_REG_HAS(reg, add_asset))
		return SHZ_OK;
	memset(&def, 0, sizeof(def));
	SHZ_INIT(def);
	def.flags = SHZ_ASSET_FOLLOW_SIZE;
	def.name = "gradient";
	def.doc = "A test asset with one control of every kind";
	def.pixel_format = SHZ_PIXELS_RGBA32F;
	def.width = 8;
	def.height = 4;
	def.controls = gradient_controls;
	def.control_count = G_CONTROLS;
	def.values = gradient_values;
	def.value_count = 2;
	def.create = gradient_create;
	def.destroy = gradient_destroy;
	def.update = gradient_update;
	def.render = gradient_render;
	def.control_changed = gradient_control_changed;
	def.save = gradient_save;
	def.load = gradient_load;
	return reg->add_asset(self, NS, &def, NULL);
}

/* ------------------------------------------------------- picture inputs and a text control (C ABI 1.3): probe */

/* cabi_example.probe: what a 1.3 asset gets, for _testing/native_asset.shio. Two inputs: "src" (RGBA8, 4 x 2) and
 * "deep" (RGBA32F, the size of the picture); a text control "label" (at most 8 characters) and a slider "gain". The
 * picture (RGBA8, 4 x 2) is src times gain. The values: the sum of the bytes of src (-1 while nothing is linked),
 * whether src changed in this frame, the length of the label (from frame->texts), CABI_EXAMPLE_VARIANT (the test of a
 * reload changes it), the width of deep (0: not linked) and the number of update() calls (its state: save / load). */

enum { P_GAIN, P_LABEL };
enum { P_SRC, P_DEEP, P_INPUTS };

static const shz_control_def probe_controls[] = {
	{ sizeof(shz_control_def), SHZ_CONTROL_SLIDER, "gain", NULL, "Times the picture of src", { 0 }, { 0 }, { 1 }, NULL, 0, 0,
		NULL, 0, 0 },
	{ sizeof(shz_control_def), SHZ_CONTROL_TEXT, "label", "Label", "A text of at most 8 characters", { 0 }, { 0 }, { 0 },
		NULL, 0, 0, "hi", 8, 0 },
#if CABI_EXAMPLE_VARIANT == 4
	{ sizeof(shz_control_def), SHZ_CONTROL_SLIDER, "extra", NULL, "A control of variant 4", { 0 }, { 0 }, { 0.5 }, NULL, 0, 0,
		NULL, 0, 0 },
#endif
};
#define P_CONTROLS ((uint32_t)(sizeof(probe_controls) / sizeof(probe_controls[0])))

static const shz_asset_input_def probe_inputs[P_INPUTS] = {
	{ sizeof(shz_asset_input_def), 0, "src", "Source", "A picture of 4 x 2 pixels", SHZ_PIXELS_RGBA8, 4, 2, 0 },
	{ sizeof(shz_asset_input_def), SHZ_INPUT_FOLLOW_SIZE, "deep", NULL, "Float pixels at the size of the picture",
		SHZ_PIXELS_RGBA32F, 0, 0, 0 },
};

static const shz_asset_value_def probe_values[] = {
	{ sizeof(shz_asset_value_def), 0, "src_sum", NULL, "The sum of the bytes of src, -1 without it", -1, 8160 },
	{ sizeof(shz_asset_value_def), 0, "src_changed", NULL, "1 in the frame src got new pixels", 0, 1 },
	{ sizeof(shz_asset_value_def), 0, "label_length", NULL, "The length of the label in bytes", 0, 32 },
	{ sizeof(shz_asset_value_def), 0, "variant", NULL, "CABI_EXAMPLE_VARIANT of the build", 0, 10 },
	{ sizeof(shz_asset_value_def), 0, "deep_width", NULL, "The width of the picture of deep, 0 without it", 0, 100 },
	{ sizeof(shz_asset_value_def), 0, "updates", NULL, "update() calls (saved with the state)", 0, 1000 },
};

typedef struct probe
{
	int64_t updates;
} probe;

static int64_t probes_alive = 0;

static shz_result probe_create(shz_ctx* ctx, const shz_asset_frame* frame, void** instance_out)
{
	probe* p = (probe*)calloc(1, sizeof(probe));
	(void)frame;
	if (!p)
		return host->error(ctx, SHZ_ERR_NOMEM, "out of memory");
	probes_alive++;
	*instance_out = p;
	return SHZ_OK;
}

static void probe_destroy(void* instance, void* asset_user)
{
	(void)asset_user;
	probes_alive--;
	free(instance);
}

static shz_result probe_update(shz_ctx* ctx, void* instance, const shz_asset_frame* frame)
{
	probe* p = (probe*)instance;
	const shz_asset_image* src;
	double sum = -1;
	uint32_t x, y;
	if (frame->struct_size < sizeof(shz_asset_frame) || frame->input_count != P_INPUTS || !frame->texts)
		return host->error(ctx, SHZ_ERR_VERSION, "probe needs the frame of C ABI 1.3");
	src = &frame->inputs[P_SRC];
	if (src->pixels)
	{
		sum = 0;
		for (y = 0; y < src->height; y++)
			for (x = 0; x < src->width * 4; x++)
				sum += ((const uint8_t*)src->pixels)[y * src->stride + x];
	}
	p->updates++;
	frame->values[0] = sum;
	frame->values[1] = src->changed;
	frame->values[2] = (double)strlen(frame->texts[P_LABEL]);
	frame->values[3] = CABI_EXAMPLE_VARIANT;
	frame->values[4] = frame->inputs[P_DEEP].pixels ? frame->inputs[P_DEEP].width : 0;
	frame->values[5] = (double)p->updates;
	return SHZ_OK;
}

static shz_result probe_render(shz_ctx* ctx, void* instance, const shz_asset_frame* frame, shz_asset_image* image)
{
	const shz_asset_image* src = &frame->inputs[P_SRC];
	const double gain = frame->controls[P_GAIN].v[0];
	uint32_t x, y;
	(void)ctx;
	(void)instance;
	for (y = 0; y < image->height; y++)
	{
		uint8_t* row = (uint8_t*)image->pixels + y * image->stride;
		for (x = 0; x < image->width * 4; x++)
		{
			double v = src->pixels && y < src->height && x < src->width * 4 ? ((const uint8_t*)src->pixels)[y * src->stride + x] * gain : 0;
			row[x] = (uint8_t)(v > 255 ? 255 : v);
		}
	}
	return SHZ_OK;
}

static shz_result probe_save(shz_ctx* ctx, void* self, shz_value* out)
{
	(void)ctx;
	*out = shz_int(((probe*)self)->updates);
	return SHZ_OK;
}

static shz_result probe_load(shz_ctx* ctx, void* self, const shz_value* data)
{
	(void)ctx;
	if (shz_is_number(data))
		((probe*)self)->updates = shz_to_int(data);
	return SHZ_OK;
}

static shz_result fn_probes_alive(shz_ctx* ctx, const shz_value* args, uint32_t argc, shz_value* out)
{
	(void)ctx;
	(void)args;
	(void)argc;
	*out = shz_int(probes_alive);
	return SHZ_OK;
}

/* cabi_example.legacy: an asset declared with the structs of C ABI 1.2 (a plugin built against the old shz_abi.h): the
 * host steps through the controls by their struct_size and reads only what it covers */
typedef struct legacy_control_def /* shz_control_def of C ABI 1.2 */
{
	uint32_t struct_size;
	uint32_t kind;
	const char* name;
	const char* display_name;
	const char* doc;
	double min[4];
	double max[4];
	double value[4];
	const char* options;
	uint32_t flags;
	uint32_t reserved;
} legacy_control_def;

/* the same layout as the start of shz_control_def (a negative array size breaks the build otherwise) */
typedef char legacy_layout_check[sizeof(legacy_control_def) == SHZ_CONTROL_DEF_SIZE_1_2 ? 1 : -1];

static const legacy_control_def legacy_controls[] = {
	{ sizeof(legacy_control_def), SHZ_CONTROL_INT, "count", NULL, "an int of a 1.2 plugin", { 1 }, { 9 }, { 3 }, NULL, 0, 0 },
	{ sizeof(legacy_control_def), SHZ_CONTROL_ENUM, "pick", NULL, "an enum of a 1.2 plugin", { 0 }, { 0 }, { 1 }, "a,b,c", 0, 0 },
};

static shz_result legacy_create(shz_ctx* ctx, const shz_asset_frame* frame, void** instance_out)
{
	(void)ctx;
	(void)frame;
	*instance_out = NULL;
	return SHZ_OK;
}

static shz_result legacy_update(shz_ctx* ctx, void* instance, const shz_asset_frame* frame)
{
	(void)ctx;
	(void)instance;
	frame->values[0] = frame->controls[0].v[0] * 10 + frame->controls[1].v[0];
	return SHZ_OK;
}

static const shz_asset_value_def legacy_values[] = {
	{ sizeof(shz_asset_value_def), 0, "code", NULL, "count * 10 + pick", 0, 100 },
};

static shz_result add_probe_assets(shz_module* self, const shz_register_api* reg)
{
	shz_asset_def def;
	shz_result r;
	if (!SHZ_REG_HAS(reg, add_asset) || !(host->features & SHZ_FEATURE_ASSET_INPUTS))
		return SHZ_OK;
	memset(&def, 0, sizeof(def));
	SHZ_INIT(def);
	def.name = "probe";
	def.doc = "A test asset with picture inputs and a text control (C ABI 1.3)";
	def.pixel_format = SHZ_PIXELS_RGBA8;
	def.width = 4;
	def.height = 2;
	def.controls = probe_controls;
	def.control_count = P_CONTROLS;
	def.values = probe_values;
	def.value_count = 6;
	def.inputs = probe_inputs;
	def.input_count = P_INPUTS;
	def.create = probe_create;
	def.destroy = probe_destroy;
	def.update = probe_update;
	def.render = probe_render;
	def.save = probe_save;
	def.load = probe_load;
	r = reg->add_asset(self, NS, &def, NULL);
	if (r != SHZ_OK)
		return r;

	memset(&def, 0, sizeof(def));
	def.struct_size = (uint32_t)SHZ_ASSET_DEF_SIZE_1_2; /* as a plugin built against 1.2 fills it */
	def.name = "legacy";
	def.doc = "A test asset declared with the structs of C ABI 1.2";
	def.controls = (const shz_control_def*)(const void*)legacy_controls;
	def.control_count = 2;
	def.values = legacy_values;
	def.value_count = 1;
	def.create = legacy_create;
	def.update = legacy_update;
	return reg->add_asset(self, NS, &def, NULL);
}

/* ------------------------------------------------------------------------------------------------ registration */

static shz_result add_fn(shz_module* self, const shz_register_api* reg, const char* ns, const char* name,
	shz_function_fn fn, uint32_t min_args, uint32_t max_args, const char* doc, const char* const* param_docs,
	uint32_t param_doc_count, void* user)
{
	shz_func_def def;
	memset(&def, 0, sizeof(def));
	SHZ_INIT(def);
	def.name = name;
	def.fn = fn;
	def.min_args = min_args;
	def.max_args = max_args;
	def.doc = doc;
	def.param_docs = param_docs;
	def.param_doc_count = param_doc_count;
	def.user = user;
	return reg->add_function(self, ns, &def);
}

static const double square_factor = 1.0;

SHZ_PLUGIN_API shz_result shz_module_register(shz_module* self, const shz_host_api* host_api, const shz_register_api* reg)
{
	static const char* const add_params[] = { "a: number", "b: number" };
	static const char* const divide_params[] = { "a: number - dividend", "b: number - divisor, not 0" };
	static const char* const value_param[] = { "value: any value" };
	static const char* const concat_params[] = { "...: any values" };
	static const char* const string_param[] = { "text: string" };
	static const char* const list_param[] = { "values: list of numbers" };
	static const char* const map_param[] = { "map: json object" };
	static const char* const buffer_param[] = { "buffer: std.buffer - changed in place" };
	static const char* const size_param[] = { "n: int - number of bytes" };
	static const char* const apply_params[] = { "fn: function - called with x", "x: any value" };
	static const char* const number_param[] = { "x: number" };
	static const char* const ctor_params[] = { "start: int (optional, 0)", "step: int (optional, 1)" };
	static const char* const add_n_param[] = { "n: int (optional, the step)" };
	static const char* const other_param[] = { "other: counter" };
	static shz_method_def methods[4];
	static shz_property_def properties[3];
	shz_type_def type;
	shz_value v;
	shz_result r = SHZ_OK;

	if (host_api->struct_size < sizeof(shz_host_api))
		return SHZ_ERR_VERSION;
	host = host_api;
	last_callback_result = shz_none();

	r |= add_fn(self, reg, NS, "add", fn_add, 2, 2, "The sum of two numbers (an int for two ints).", add_params, 2, NULL);
	r |= add_fn(self, reg, NS, "divide", fn_divide, 2, 2, "a / b as a float; a runtime error (try / catch) for b == 0.",
		divide_params, 2, NULL);
	r |= add_fn(self, reg, NS, "echo", fn_echo, 1, 1, "Returns its argument (a round trip through the C ABI).", value_param, 1, NULL);
	r |= add_fn(self, reg, NS, "kind", fn_kind, 1, 1, "The C ABI type of a value: none, int, float, string, bytes, list, map, object, function or other.",
		value_param, 1, NULL);
	r |= add_fn(self, reg, NS, "concat", fn_concat, 0, SHZ_ARGS_VARIADIC, "The texts of all arguments joined into one string.",
		concat_params, 1, NULL);
#if CABI_EXAMPLE_VARIANT != 3 /* variant 3 drops a function: a reload to it is refused */
	r |= add_fn(self, reg, NS, "upper", fn_upper, 1, 1, "The string in upper case (ASCII letters).", string_param, 1, NULL);
#endif
	r |= add_fn(self, reg, NS, "stats", fn_stats, 1, 1, "[count, sum, min, max] of a list of numbers.", list_param, 1, NULL);
	r |= add_fn(self, reg, NS, "keys", fn_keys, 1, 1, "The keys of a json object as a list.", map_param, 1, NULL);
	r |= add_fn(self, reg, NS, "invert", fn_invert, 1, 1, "Inverts every byte of a buffer in place (no copy); returns the number of bytes.",
		buffer_param, 1, NULL);
	r |= add_fn(self, reg, NS, "resize_locked", fn_resize_locked, 1, 1, "Test helper: 1 when resizing a locked buffer is refused.",
		buffer_param, 1, NULL);
	r |= add_fn(self, reg, NS, "make_bytes", fn_make_bytes, 1, 1, "A new buffer of n bytes 0, 1, 2, ...", size_param, 1, NULL);
	r |= add_fn(self, reg, NS, "warn_nonfatal", fn_warn_nonfatal, 0, 0, "Reports a non-fatal error and returns 1.", NULL, 0, NULL);
	r |= add_fn(self, reg, NS, "apply", fn_apply, 2, 2, "Starts fn(x); last_result() has its return value once it finished.",
		apply_params, 2, NULL);
	r |= add_fn(self, reg, NS, "last_result", fn_last_result, 0, 0, "The return value of the last function started by apply().",
		NULL, 0, NULL);
	r |= add_fn(self, reg, NS, "counters_alive", fn_counters_alive, 0, 0, "How many counter objects exist.", NULL, 0, NULL);
	r |= add_fn(self, reg, NS, "counters_finalized", fn_counters_finalized, 0, 0, "How many counter objects were finalized.",
		NULL, 0, NULL);
	r |= add_fn(self, reg, NS ".math", "square", fn_scaled_square, 1, 1, "x * x (a function of a child namespace).", number_param, 1,
		(void*)&square_factor);

	v = shz_int(42);
	r |= reg->add_constant(self, NS, "ANSWER", &v);
	v = shz_float(3.25);
	r |= reg->add_constant(self, NS, "RATIO", &v);
	r |= reg->add_constant_string(self, NS, "VERSION", "1.0.0");

	/* the counter type */
	memset(methods, 0, sizeof(methods));
	SHZ_INIT(methods[0]);
	methods[0].name = "add";
	methods[0].fn = counter_add;
	methods[0].min_args = 0;
	methods[0].max_args = 1;
	methods[0].doc = "Adds n (or the step); returns the counter.";
	methods[0].param_docs = add_n_param;
	methods[0].param_doc_count = 1;
	SHZ_INIT(methods[1]);
	methods[1].name = "reset";
	methods[1].fn = counter_reset;
	methods[1].doc = "Back to 0, the history emptied; returns the counter.";
	SHZ_INIT(methods[2]);
	methods[2].name = "history";
	methods[2].fn = counter_history;
	methods[2].doc = "The list of the values after each add() (the counter's own list).";
	SHZ_INIT(methods[3]);
	methods[3].name = "same";
	methods[3].fn = counter_same;
	methods[3].min_args = 1;
	methods[3].max_args = 1;
	methods[3].doc = "1 when other is this counter.";
	methods[3].param_docs = other_param;
	methods[3].param_doc_count = 1;

	memset(properties, 0, sizeof(properties));
	SHZ_INIT(properties[0]);
	properties[0].name = "count";
	properties[0].get = counter_get_count;
	properties[0].set = counter_set_count;
	properties[0].doc = "The value.";
	SHZ_INIT(properties[1]);
	properties[1].name = "step";
	properties[1].get = counter_get_step;
	properties[1].set = counter_set_step;
	properties[1].doc = "What add() adds without an argument.";
	SHZ_INIT(properties[2]);
	properties[2].name = "kind";
	properties[2].get = counter_get_kind;
	properties[2].doc = "Always \"counter\" (read-only).";

	memset(&type, 0, sizeof(type));
	SHZ_INIT(type);
	type.name = "counter";
	type.doc = "A counter: counter(start, step). c.add(), c.count, c[i] (history), size(), ==, <, + int, serialize().";
	type.construct = counter_construct;
	type.ctor_min_args = 0;
	type.ctor_max_args = 2;
	type.ctor_param_docs = ctor_params;
	type.ctor_param_doc_count = 2;
	type.finalize = counter_finalize;
	type.methods = methods;
	type.method_count = 4;
	type.properties = properties;
	type.property_count = 3;
	type.to_string = counter_to_string;
	type.index_get = counter_index_get;
	type.size = counter_size;
	type.compare = counter_compare;
	type.arith = counter_arith;
	type.serialize = counter_serialize;
	type.deserialize = counter_deserialize;
	r |= reg->add_type(self, NS, &type, &counter_type);

	r |= add_gradient(self, reg);
	r |= add_fn(self, reg, NS, "gradients_alive", fn_gradients_alive, 0, 0, "How many gradient assets exist.", NULL, 0, NULL);
	r |= add_probe_assets(self, reg);
	r |= add_fn(self, reg, NS, "probes_alive", fn_probes_alive, 0, 0, "How many probe assets exist.", NULL, 0, NULL);

	if (r != SHZ_OK)
	{
		host->log(NULL, SHZ_LEVEL_ERROR, reg->last_error(self));
		return SHZ_ERR_RUNTIME;
	}
	return SHZ_OK;
}

SHZ_PLUGIN_API void shz_module_unregister(shz_module* self)
{
	(void)self;
	if (host)
		host->release(&last_callback_result);
	host = NULL;
}
