#define _GNU_SOURCE
/* Runs the core (core.wbx) in the sandbox the way the frontend does: the
 * program mounted under its own name without being read, "rom.name" and
 * "settings" beside it, Init, the machine sealed, and then a frame at a time
 * by the harness the native runner uses (gate-harness.h).
 *
 *   run-wbx <core.wbx> <program> [options]
 *
 * The options are the harness's; the states are miniBox's.
 */
#include "minibox.h"
#include "vk-host.h"

#include "gate-harness.h"

static mb_host *g_host;
static int g_vulkan;

typedef struct { FILE *f; } freader;
static intptr_t file_read(uintptr_t ud, uint8_t *data, uintptr_t size)
{
	return (intptr_t)fread(data, 1, size, ((freader *)ud)->f);
}

typedef struct { uint8_t *b; size_t len, cap, pos; } membuf;
static int32_t mem_write(uintptr_t ud, const uint8_t *d, uintptr_t n)
{
	membuf *m = (membuf *)ud;
	if (m->len + n > m->cap) {
		size_t cap = m->cap ? m->cap * 2 : ((size_t)64 << 20);
		while (cap < m->len + n)
			cap *= 2;
		uint8_t *b = (uint8_t *)realloc(m->b, cap);
		if (!b)
			return -1;
		m->b = b;
		m->cap = cap;
	}
	memcpy(m->b + m->len, d, n);
	m->len += n;
	return 0;
}
static intptr_t mem_read(uintptr_t ud, uint8_t *d, uintptr_t n)
{
	membuf *m = (membuf *)ud;
	size_t left = m->len - m->pos;
	if (n > left)
		n = left;
	memcpy(d, m->b + m->pos, n);
	m->pos += n;
	return (intptr_t)n;
}

/* the machine as Init left it is what every state is against */
static int do_seal(void)
{
	mb_return r;
	wbx_deactivate_host(g_host, &r);
	wbx_seal(g_host, &r);
	if (r.error_message[0]) {
		fprintf(stderr, "run-wbx: seal: %s\n", (const char *)r.error_message);
		return 0;
	}
	wbx_activate_host(g_host, &r);
	return 1;
}
static int do_save(uint8_t **buf, size_t *len)
{
	membuf m = { 0 };
	mb_return r;
	wbx_save_state(g_host, mem_write, (uintptr_t)&m, &r);
	if (r.error_message[0]) {
		fprintf(stderr, "run-wbx: save: %s\n", (const char *)r.error_message);
		free(m.b);
		return 0;
	}
	*buf = m.b;
	*len = m.len;
	return 1;
}
static int do_load(const uint8_t *buf, size_t len)
{
	membuf m = { (uint8_t *)buf, len, len, 0 };
	mb_return r;
	wbx_load_state(g_host, mem_read, (uintptr_t)&m, &r);
	if (r.error_message[0]) {
		fprintf(stderr, "run-wbx: load: %s\n", (const char *)r.error_message);
		return 0;
	}
	/* What the frontend's session does after a load: every object the loaded
	 * memory names is another moment's, and the context is a new one. The
	 * gate's negative control leaves it out. */
	const char *keep = getenv("RUN_WBX_KEEP_CONTEXT");
	if (g_vulkan && !(keep && *keep))
		chimera_vk_host_state_loaded();
	return 1;
}

static uintptr_t proc(const char *name)
{
	mb_return r;
	wbx_get_proc_addr(g_host, name, &r);
	if (r.error_message[0] || !r.data) {
		fprintf(stderr, "run-wbx: the core has no %s %s\n", name, (const char *)r.error_message);
		exit(2);
	}
	return r.data;
}

static void mount_text(const char *name, const char *text)
{
	membuf m = { (uint8_t *)text, strlen(text), strlen(text), 0 };
	mb_return r;
	wbx_mount_file(g_host, name, mem_read, (uintptr_t)&m, false, &r);
	if (r.error_message[0]) {
		fprintf(stderr, "run-wbx: %s: %s\n", name, (const char *)r.error_message);
		exit(2);
	}
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: run-wbx <core.wbx> <program> [options]\n");
		return 2;
	}
	struct harness_opts o;
	if (!harness_parse(argc, argv, 3, &o))
		return 2;
	FILE *f = fopen(argv[1], "rb");
	if (!f) {
		fprintf(stderr, "run-wbx: cannot open %s\n", argv[1]);
		return 2;
	}
	/* The emulated address space is 4 GiB in a reservation of 12.5; the heap
	 * is the emulator's own. Nothing is committed until it is touched. */
	mb_memory_layout_template layout = {
		.sbrk_size = 4096ull << 20, .sealed_size = 16ull << 20, .invis_size = 64ull << 20,
		.plain_size = 64ull << 20, .mmap_size = 20480ull << 20,
	};
	freader fr = { f };
	mb_return r;
	wbx_create_host(&layout, "core.wbx", file_read, (uintptr_t)&fr, &r);
	fclose(f);
	if (r.error_message[0]) {
		fprintf(stderr, "run-wbx: create_host: %s\n", (const char *)r.error_message);
		return 2;
	}
	g_host = (mb_host *)r.data;

	const char *base = strrchr(argv[2], '/');
	base = base ? base + 1 : argv[2];
	wbx_mount_file_path(g_host, base, argv[2], &r);
	if (r.error_message[0]) {
		fprintf(stderr, "run-wbx: %s: %s\n", argv[2], (const char *)r.error_message);
		return 2;
	}
	char settings[256];
	harness_settings(&o, settings, sizeof settings);
	mount_text("rom.name", base);
	mount_text("settings", settings);

	wbx_activate_host(g_host, &r);
	if (r.error_message[0]) {
		fprintf(stderr, "run-wbx: activate: %s\n", (const char *)r.error_message);
		return 2;
	}
	/* the Vulkan bridge, handed over before Init */
	if (!strcmp(o.renderer, "vulkan")) {
		char why[256] = "";
		if (chimera_vk_host_init(why, sizeof why) != 0) {
			fprintf(stderr, "run-wbx: no Vulkan on this machine: %s\n", why);
			return 5;
		}
		mb_return cr;
		wbx_get_callback_addr(g_host, (mb_external_callback)chimera_vk_host_dispatch, 0, &cr);
		if (!cr.data) {
			fprintf(stderr, "run-wbx: the sandbox would not take the bridge's callback\n");
			return 5;
		}
		((void (*)(uint64_t))proc("SetVulkanBridge"))((uint64_t)cr.data);
		g_vulkan = 1;
	}

	/* on Linux the guest's calling convention is the host's: the exports are
	 * called as plain functions */
	const struct harness_core core = {
		(int (*)(void))proc("Init"),
		(const char *(*)(void))proc("GetLoadError"),
		(void (*)(int32_t, int32_t))proc("SetAxis"),
		(void (*)(uint64_t))proc("FrameAdvance"),
		(int (*)(void))proc("InputWasRead"),
		(int16_t *(*)(void))proc("GetAudio"),
		(int (*)(void))proc("GetAudioSampleCount"),
		(uint64_t (*)(void))proc("GetMachineTimeNs"),
		(uint64_t (*)(void))proc("GetSwitchCount"),
		(uint64_t (*)(void))proc("GetInstructionCount"),
		(const char *(*)(void))proc("DebugInstructionsByThread"),
		(uint64_t (*)(void))proc("GetGpuCounter"),
		(uint64_t (*)(void))proc("GetRamDigest"),
		(const uint8_t *(*)(int32_t, int64_t, int32_t))proc("ReadBus"),
		(void (*)(void))proc("RemakeRenderer"),
		(void (*)(void))proc("StateSaving"),
		(uint32_t *(*)(void))proc("GetVideoBgra"),
		(int (*)(void))proc("GetVideoWidth"),
		(int (*)(void))proc("GetVideoHeight"),
		do_seal, do_save, do_load,
	};
	const int result = harness_run(&core, &o);
	if (g_vulkan) {
		long last = 0;
		const unsigned long refused = chimera_vk_host_refused(&last);
		fprintf(stderr, "run-wbx: vulkan on %s; %lu calls refused%s", chimera_vk_host_description(), refused,
		        refused ? "" : "\n");
		if (refused)
			fprintf(stderr, " (the last was opcode %ld)\n", last);
		/* with CHIMERA_VK_STATS=1 this is where the bridge says what crossed
		 * it and how long each command took */
		chimera_vk_host_shutdown();
	}
	fflush(stdout);
	fflush(stderr);
	return result;
}
