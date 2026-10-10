#define _GNU_SOURCE
/* Runs the instruction tests' guest (ppc-tests.wbx) in the sandbox: mounts
 * the test files under their bare names with a list of them, calls the one
 * export, and ends with its answer. What each test did is what the guest
 * wrote to standard error; standard output has, before the guest's own
 * lines, a line "mounted <file>" for each file of tests, so that a caller
 * running a part knows which part.
 *
 *   run-ppc-wbx <ppc-tests.wbx> <dir of .s files> <dir of .bin and .map files> [<every>]
 *
 * With <every>, only every that-many-th file of tests is run (the first, the
 * one after that many, ...): a new processor is made for each test, and in
 * the sandbox that is slow enough for the whole set to take half an hour.
 */
#include "minibox.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { FILE *f; } freader;
static intptr_t file_read(uintptr_t ud, uint8_t *data, uintptr_t size)
{
	return (intptr_t)fread(data, 1, size, ((freader *)ud)->f);
}
typedef struct { const uint8_t *p; size_t n, pos; } memreader;
static intptr_t mem_read(uintptr_t ud, uint8_t *data, uintptr_t size)
{
	memreader *m = (memreader *)ud;
	size_t take = size < (m->n - m->pos) ? size : (m->n - m->pos);
	memcpy(data, m->p + m->pos, take);
	m->pos += take;
	return (intptr_t)take;
}

static void die(const char *what, const mb_return *r)
{
	fprintf(stderr, "run-ppc-wbx: %s: %s\n", what, (const char *)r->error_message);
	exit(2);
}

static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int main(int argc, char **argv)
{
	if (argc != 4 && argc != 5) {
		fprintf(stderr, "usage: run-ppc-wbx <ppc-tests.wbx> <dir of .s> <dir of .bin and .map> [<every>]\n");
		return 2;
	}
	const size_t every = argc == 5 ? (size_t)strtoul(argv[4], NULL, 10) : 1;
	FILE *f = fopen(argv[1], "rb");
	if (!f) {
		fprintf(stderr, "run-ppc-wbx: cannot open %s\n", argv[1]);
		return 2;
	}
	/* The emulated address space is 4 GiB in a reservation of 12.5 (it must
	 * begin on a multiple of 4 GiB, with the recompiler's function table
	 * after it); the heap is the emulator's own. Nothing of it is committed
	 * until it is touched. */
	mb_memory_layout_template layout = {
		.sbrk_size = 2048ull << 20, .sealed_size = 16ull << 20, .invis_size = 64ull << 20,
		.plain_size = 64ull << 20, .mmap_size = 16384ull << 20,
	};
	freader fr = { f };
	mb_return r;
	wbx_create_host(&layout, "ppc-tests.wbx", file_read, (uintptr_t)&fr, &r);
	fclose(f);
	if (r.error_message[0])
		die("create_host", &r);
	mb_host *host = (mb_host *)r.data;

	/* the tests: every instr_*.s that has its .bin and .map, sorted */
	char **names = NULL;
	size_t count = 0, room = 0;
	DIR *d = opendir(argv[2]);
	if (!d) {
		fprintf(stderr, "run-ppc-wbx: cannot list %s\n", argv[2]);
		return 2;
	}
	for (struct dirent *e; (e = readdir(d)) != NULL;) {
		const size_t n = strlen(e->d_name);
		if (n < 9 || strncmp(e->d_name, "instr_", 6) != 0 || strcmp(e->d_name + n - 2, ".s") != 0)
			continue;
		if (count == room) {
			room = room ? room * 2 : 1024;
			names = realloc(names, room * sizeof *names);
		}
		names[count++] = strdup(e->d_name);
	}
	closedir(d);
	qsort(names, count, sizeof *names, by_name);

	size_t list_len = 0, list_room = 1 << 16;
	char *list = malloc(list_room);
	char path[4096], name[512];
	for (size_t i = 0; i < count; i += every ? every : 1) {
		const size_t stem = strlen(names[i]) - 2;
		static const char *const ext[] = { ".s", ".bin", ".map" };
		int have = 1;
		for (int k = 0; k < 3 && have; k++) {
			snprintf(path, sizeof path, "%s/%.*s%s", k == 0 ? argv[2] : argv[3], (int)stem, names[i], ext[k]);
			FILE *t = fopen(path, "rb");
			have = t != NULL;
			if (t)
				fclose(t);
		}
		if (!have)
			continue;
		for (int k = 0; k < 3; k++) {
			snprintf(path, sizeof path, "%s/%.*s%s", k == 0 ? argv[2] : argv[3], (int)stem, names[i], ext[k]);
			snprintf(name, sizeof name, "%.*s%s", (int)stem, names[i], ext[k]);
			wbx_mount_file_path(host, name, path, &r);
			if (r.error_message[0])
				die(name, &r);
		}
		if (list_len + strlen(names[i]) + 2 > list_room)
			list = realloc(list, list_room *= 2);
		list_len += (size_t)sprintf(list + list_len, "%s\n", names[i]);
		printf("mounted %.*s\n", (int)stem, names[i]);
	}
	memreader lr = { (const uint8_t *)list, list_len, 0 };
	wbx_mount_file(host, "tests.list", mem_read, (uintptr_t)&lr, false, &r);
	if (r.error_message[0])
		die("tests.list", &r);
	/* Upstream's list of tests to leave out (skip.txt) is NOT mounted: the
	 * native reference runs every test, failing ones included, and the point
	 * is that the same ones fail here. */

	wbx_activate_host(host, &r);
	if (r.error_message[0])
		die("activate", &r);
	wbx_get_proc_addr(host, "ChimeraPpcTests", &r);
	if (r.error_message[0])
		die("ChimeraPpcTests", &r);
	const int result = ((int (MB_GUEST_ABI *)(void))r.data)();
	fflush(stdout);
	fflush(stderr);
	return result;
}
