/* What both runners of the gate do with a core once they have its exports:
 * the native one (native/run-frames.cc) calls them directly, the sandboxed
 * one (run-wbx.c) through miniBox. One file, so that the two runs are asked
 * the same things in the same order.
 *
 *   <runner> ... [--frames N] [--every K] [--press F:N:MASK]... [--axis F:N:I:V]...
 *                [--pads N] [--renderer R] [--log] [--trace-schedule]
 *                [--rerecord] [--save-state FILE --at F] [--state FILE]
 *
 * --press F:N:MASK  from frame F (the first is 1) for N frames, these button
 *                   bits (hex; sixteen a port, the console's own)
 * --axis F:N:I:V    from frame F for N frames, axis I is V
 * --every K         a line of digest every K frames, and always one at the end
 * --rerecord        a state saved and loaded before every frame (sandbox)
 * --save-state FILE --at F   after frame F, save a state to FILE and stop
 * --state FILE      carry on from a state another process saved
 * --ppm FILE [--ppm-at F]    the picture after frame F (the last one without
 *                   --ppm-at), as a file a person can look at
 */
#ifndef CHIMERA_XENIA_GATE_HARNESS_H_
#define CHIMERA_XENIA_GATE_HARNESS_H_

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct harness_core {
	int (*Init)(void);
	const char *(*GetLoadError)(void);
	void (*SetAxis)(int32_t, int32_t);
	void (*FrameAdvance)(uint64_t);
	int (*InputWasRead)(void);
	int16_t *(*GetAudio)(void);
	int (*GetAudioSampleCount)(void);
	uint64_t (*GetMachineTimeNs)(void);
	uint64_t (*GetSwitchCount)(void);
	uint64_t (*GetGpuCounter)(void);
	uint64_t (*GetRamDigest)(void);
	const uint8_t *(*ReadBus)(int32_t, int64_t, int32_t);
	uint32_t *(*GetVideoBgra)(void);
	int (*GetVideoWidth)(void);
	int (*GetVideoHeight)(void);
	/* the sandbox's; null natively */
	int (*seal)(void);
	int (*save)(uint8_t **buf, size_t *len);
	int (*load)(const uint8_t *buf, size_t len);
};

#define HARNESS_MAX_EVENTS 64
struct harness_event { uint64_t from, count; int index; int64_t value; uint64_t mask; };
struct harness_opts {
	uint64_t frames, every, save_at;
	int rerecord, log, pads, trace_schedule;
	const char *renderer, *save_state, *state, *ppm;
	uint64_t ppm_at;
	struct harness_event press[HARNESS_MAX_EVENTS], axis[HARNESS_MAX_EVENTS];
	int presses, axes;
};

/* The options from argv[first...]. 0 with a message when one is not one. */
static int harness_parse(int argc, char **argv, int first, struct harness_opts *o)
{
	memset(o, 0, sizeof *o);
	o->frames = 300;
	o->pads = 1;
	o->renderer = "null";
	for (int i = first; i < argc; i++) {
		const char *a = argv[i];
		const char *v = i + 1 < argc ? argv[i + 1] : NULL;
		if (!strcmp(a, "--rerecord")) { o->rerecord = 1; continue; }
		if (!strcmp(a, "--log")) { o->log = 1; continue; }
		if (!strcmp(a, "--trace-schedule")) { o->trace_schedule = 1; continue; }
		if (!v) { fprintf(stderr, "%s wants a value\n", a); return 0; }
		i++;
		if (!strcmp(a, "--frames")) o->frames = strtoull(v, NULL, 10);
		else if (!strcmp(a, "--every")) o->every = strtoull(v, NULL, 10);
		else if (!strcmp(a, "--at")) o->save_at = strtoull(v, NULL, 10);
		else if (!strcmp(a, "--pads")) o->pads = atoi(v);
		else if (!strcmp(a, "--renderer")) o->renderer = v;
		else if (!strcmp(a, "--save-state")) o->save_state = v;
		else if (!strcmp(a, "--state")) o->state = v;
		else if (!strcmp(a, "--ppm")) o->ppm = v;
		else if (!strcmp(a, "--ppm-at")) o->ppm_at = strtoull(v, NULL, 10);
		else if (!strcmp(a, "--press")) {
			struct harness_event e = { 0 };
			if (o->presses == HARNESS_MAX_EVENTS ||
			    sscanf(v, "%" SCNu64 ":%" SCNu64 ":%" SCNx64, &e.from, &e.count, &e.mask) != 3) {
				fprintf(stderr, "--press wants <frame>:<frames>:<hex mask>, not %s\n", v);
				return 0;
			}
			o->press[o->presses++] = e;
		} else if (!strcmp(a, "--axis")) {
			struct harness_event e = { 0 };
			if (o->axes == HARNESS_MAX_EVENTS ||
			    sscanf(v, "%" SCNu64 ":%" SCNu64 ":%d:%" SCNd64, &e.from, &e.count, &e.index, &e.value) != 4) {
				fprintf(stderr, "--axis wants <frame>:<frames>:<index>:<value>, not %s\n", v);
				return 0;
			}
			o->axis[o->axes++] = e;
		} else {
			fprintf(stderr, "no such option: %s\n", a);
			return 0;
		}
	}
	if (o->save_state && (!o->save_at || o->save_at > o->frames)) {
		fprintf(stderr, "--save-state wants --at <a frame of the run>\n");
		return 0;
	}
	return 1;
}

/* The settings a runner hands the core, as the frontend would. */
static void harness_settings(const struct harness_opts *o, char *out, size_t size)
{
	snprintf(out, size, "{\"renderer\":\"%s\",\"pads\":\"%d\",\"log\":\"%d\",\"trace_schedule\":\"%d\"}",
	         o->renderer, o->pads, o->log, o->trace_schedule);
}

static uint64_t harness_hash(uint64_t h, const void *data, size_t bytes)
{
	const uint8_t *p = (const uint8_t *)data;
	size_t i = 0;
	for (; i + 8 <= bytes; i += 8) {
		uint64_t w;
		memcpy(&w, p + i, 8);
		h = (h ^ w) * 0x100000001b3ull;
	}
	for (; i < bytes; i++)
		h = (h ^ p[i]) * 0x100000001b3ull;
	return h;
}

/* What a run has added up so far; a state file carries it, so that a run
 * carried on in another process ends with the sums of one that never stopped. */
struct harness_sums { uint64_t frame, input_frames, audio_pairs, audio_hash; };
#define HARNESS_STATE_MAGIC "xenia-gate-state-1"

static void harness_line(const struct harness_core *c, const struct harness_sums *s, const char *what)
{
	const int w = c->GetVideoWidth(), h = c->GetVideoHeight();
	/* The bus, as a tool reads it, at places a tool might look: nothing at
	 * all, the program, the GPU's registers, memory by its physical address
	 * (its first page, and a megabyte near the top where a program keeps
	 * what it closes), and the last page there is. None of these may hurt. */
	static const int64_t places[] = { 0x00010000, 0x82000000, 0x7FC80000, 0xA0000000, 0xBF500000, 0xFFFF0000 };
	static const int32_t spans[] = { 256, 4096, 256, 4096, 65536, 65536 };
	uint64_t bus = 0xcbf29ce484222325ull;
	for (size_t i = 0; i < sizeof places / sizeof *places; i++)
		for (int32_t at = 0; at < (i == 4 ? 0x100000 : spans[i]); at += spans[i])
			bus = harness_hash(bus, c->ReadBus(0, places[i] + at, spans[i]), (size_t)spans[i]);
	printf("%s frame=%" PRIu64 " virtualNs=%" PRIu64 " switches=%" PRIu64 " gpu=%" PRIu64 " inputFrames=%" PRIu64
	       " audioPairs=%" PRIu64 " audioHash=%016" PRIx64 " ramHash=%016" PRIx64 " busHash=%016" PRIx64
	       " video=%dx%d videoHash=%016" PRIx64 "\n",
	       what, s->frame, c->GetMachineTimeNs(), c->GetSwitchCount(), c->GetGpuCounter(), s->input_frames,
	       s->audio_pairs, s->audio_hash, c->GetRamDigest(), bus,
	       w, h, harness_hash(0xcbf29ce484222325ull, c->GetVideoBgra(), (size_t)w * (size_t)h * 4));
	fflush(stdout);
}

/* The picture as a PPM: the simplest file a viewer opens. */
static void harness_ppm(const struct harness_core *c, const char *path)
{
	const int w = c->GetVideoWidth(), h = c->GetVideoHeight();
	const uint32_t *p = c->GetVideoBgra();
	FILE *f = fopen(path, "wb");
	if (!f) {
		fprintf(stderr, "cannot write %s\n", path);
		return;
	}
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (int i = 0; i < w * h; i++) {
		const uint8_t rgb[3] = { (uint8_t)(p[i] >> 16), (uint8_t)(p[i] >> 8), (uint8_t)p[i] };
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
}

/* Init has not been called. The process's exit code. */
static int harness_run(const struct harness_core *c, const struct harness_opts *o)
{
	if ((o->rerecord || o->save_state || o->state) && !c->save) {
		fprintf(stderr, "states are the sandbox's: this runner has none\n");
		return 2;
	}
	if (!c->Init()) {
		fprintf(stderr, "Init: %s\n", c->GetLoadError());
		return 1;
	}
	/* the machine as it stands after Init is what every state is against */
	if (c->seal && !c->seal())
		return 1;

	struct harness_sums s = { 0, 0, 0, 0xcbf29ce484222325ull };
	if (o->state) {
		FILE *f = fopen(o->state, "rb");
		char magic[sizeof HARNESS_STATE_MAGIC];
		uint64_t len = 0;
		if (!f || fread(magic, 1, sizeof magic, f) != sizeof magic || memcmp(magic, HARNESS_STATE_MAGIC, sizeof magic) ||
		    fread(&s, sizeof s, 1, f) != 1 || fread(&len, sizeof len, 1, f) != 1) {
			fprintf(stderr, "%s is not a state of this gate\n", o->state);
			return 2;
		}
		uint8_t *buf = (uint8_t *)malloc(len ? len : 1);
		if (!buf || fread(buf, 1, len, f) != len) {
			fprintf(stderr, "%s is cut short\n", o->state);
			return 2;
		}
		fclose(f);
		if (!c->load(buf, len))
			return 1;
		free(buf);
	}

	while (s.frame < o->frames) {
		const uint64_t f = s.frame + 1;
		if (o->rerecord) {
			uint8_t *buf = NULL;
			size_t len = 0;
			if (!c->save(&buf, &len))
				return 1;
			/* the gate's negative control: once, half way, the state loaded
			 * is the one from the frame before - a state that is not this
			 * moment's must show in what the run ends with */
			static uint8_t *before;
			static size_t before_len;
			const char *stale = getenv("HARNESS_BREAK_STALE_STATE");
			if (stale && *stale && before && f == o->frames / 2) {
				if (!c->load(before, before_len))
					return 1;
			} else if (!c->load(buf, len)) {
				return 1;
			}
			free(before);
			before = buf;
			before_len = len;
		}
		uint64_t buttons = 0;
		for (int i = 0; i < o->presses; i++)
			if (f >= o->press[i].from && f < o->press[i].from + o->press[i].count)
				buttons |= o->press[i].mask;
		/* every axis, every frame, as the frontend sets them */
		int32_t axes[4 * 6] = { 0 };
		for (int i = 0; i < o->axes; i++)
			if (f >= o->axis[i].from && f < o->axis[i].from + o->axis[i].count && o->axis[i].index >= 0 &&
			    o->axis[i].index < (int)(sizeof axes / sizeof *axes))
				axes[o->axis[i].index] = (int32_t)o->axis[i].value;
		for (int i = 0; i < (int)(sizeof axes / sizeof *axes); i++)
			c->SetAxis(i, axes[i]);
		c->FrameAdvance(buttons);
		s.frame = f;
		s.input_frames += c->InputWasRead() ? 1 : 0;
		const int pairs = c->GetAudioSampleCount();
		if (pairs > 0) {
			s.audio_pairs += (uint64_t)pairs;
			s.audio_hash = harness_hash(s.audio_hash, c->GetAudio(), (size_t)pairs * 4);
		}
		if (o->every && f % o->every == 0 && f != o->frames)
			harness_line(c, &s, "at");
		if (o->ppm && f == (o->ppm_at ? o->ppm_at : o->frames))
			harness_ppm(c, o->ppm);
		if (o->save_state && f == o->save_at) {
			uint8_t *buf = NULL;
			size_t len = 0;
			if (!c->save(&buf, &len))
				return 1;
			FILE *out = fopen(o->save_state, "wb");
			const uint64_t len64 = len;
			if (!out || fwrite(HARNESS_STATE_MAGIC, 1, sizeof HARNESS_STATE_MAGIC, out) != sizeof HARNESS_STATE_MAGIC ||
			    fwrite(&s, sizeof s, 1, out) != 1 || fwrite(&len64, sizeof len64, 1, out) != 1 ||
			    fwrite(buf, 1, len, out) != len || fclose(out) != 0) {
				fprintf(stderr, "cannot write %s\n", o->save_state);
				return 2;
			}
			free(buf);
			harness_line(c, &s, "saved");
			fprintf(stderr, "state: %zu bytes after frame %" PRIu64 "\n", len, f);
			return 0;
		}
	}
	harness_line(c, &s, "end");
	return 0;
}

#endif /* CHIMERA_XENIA_GATE_HARNESS_H_ */
