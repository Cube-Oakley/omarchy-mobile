// SPDX-License-Identifier: MIT
/*
 * omarchy-mobile-vkbd: the on-screen keyboard's key sender and swipe decoder.
 *
 * The keyboard's window (keyboard/Keyboard.qml) runs this and writes one
 * command per line on its stdin:
 *   tap CODE [shift]    press and release an evdev key code (linux/input.h)
 *   mods ctrl|alt ...   hold these modifiers for the next tap or character
 *   down CODE / up CODE hold a key (backspace repeat is done by taps)
 *   text STRING         type printable ASCII, through the US keymap
 *   keys CH:X,Y ...     the letter keys' centres, in key widths
 *   swipe X,Y X,Y ...   a finger path in the same units; answers on stdout
 *                       "words W1 W2 W3" (best first, maybe fewer, maybe none)
 *   learn WORD          the user typed WORD (kept by the user's word list)
 * Keys go through zwp_virtual_keyboard_v1 with an evdev/pc105/us keymap,
 * as wvkbd sends them.
 *
 * The swipe decoder compares the path, resampled to 40 points, with each
 * candidate word's path through the key centres (words whose first and last
 * keys lie near the path's ends) and adds the word's frequency:
 *   cost = d^2 / (2 sigma^2) + (s^2 + e^2) / (2 sigma_end^2) - ln p(word),
 * d the mean point distance, s and e the distances from the path's ends to
 * the word's first and last keys (where a swipe starts and stops is the
 * surest part: "help" against "hello").
 * Words come from WORDS.txt lines "word count" (most frequent first),
 * plus a few contractions typed without the apostrophe.
 *
 * The user's words (USER.txt, "word uses" lines) raise those words: a list
 * word gains 1e-4 of probability per use, about the 2000th word's share; a
 * word the list lacks becomes a candidate at its second use (a one-off typo
 * is not learned) and gains 2e-5 per use. Uses count up to 50.
 *
 *   omarchy-mobile-vkbd WORDS.txt [USER.txt] [--test]
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "virtual-keyboard-unstable-v1-client-protocol.h"

#define POINTS 40
#define MAX_WORDS 60000
#define SIGMA 0.24	/* tuned on synthetic swipes: 84 % first, 96 % in the top 3 */
#define END_SIGMA 0.4
#define RADIUS 1.0

static struct wl_display *display;
static struct wl_seat *seat;
static struct zwp_virtual_keyboard_manager_v1 *manager;
static struct zwp_virtual_keyboard_v1 *vkbd;
static uint32_t pending_mods;	/* one-shot Ctrl (4) and Alt (8), from "mods" */

static uint32_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
		   uint32_t version)
{
	if (!strcmp(interface, wl_seat_interface.name) && !seat)
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
	else if (!strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name))
		manager = wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
}

static void global_remove(void *data, struct wl_registry *registry, uint32_t name) {}

static const struct wl_registry_listener registry_listener = { global, global_remove };

static int upload_keymap(void)
{
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_rule_names names = { .rules = "evdev", .model = "pc105", .layout = "us" };
	struct xkb_keymap *keymap = ctx ? xkb_keymap_new_from_names(ctx, &names, 0) : NULL;
	char *text = keymap ? xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1) : NULL;
	if (!text)
		return -1;
	size_t size = strlen(text) + 1;
	int fd = memfd_create("omarchy-keymap", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) < 0)
		return -1;
	char *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
		return -1;
	memcpy(map, text, size);
	munmap(map, size);
	zwp_virtual_keyboard_v1_keymap(vkbd, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, size);
	close(fd);
	free(text);
	xkb_keymap_unref(keymap);
	xkb_context_unref(ctx);
	return 0;
}

static void key(uint32_t code, bool pressed)
{
	zwp_virtual_keyboard_v1_key(vkbd, now_ms(), code,
				    pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
}

static void tap(uint32_t code, bool shift)
{
	uint32_t mods = (shift ? 1 : 0) | pending_mods;
	if (mods)
		zwp_virtual_keyboard_v1_modifiers(vkbd, mods, 0, 0, 0);
	key(code, true);
	key(code, false);
	if (mods)
		zwp_virtual_keyboard_v1_modifiers(vkbd, 0, 0, 0, 0);
	pending_mods = 0;
}

/* Printable ASCII on the US layout: the key and whether it needs Shift. */
static bool ascii_key(unsigned char c, uint32_t *code, bool *shift)
{
	static const char *lower = "abcdefghijklmnopqrstuvwxyz";
	static const uint32_t letters[] = {
		KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M,
		KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
	};
	static const struct { char plain, shifted; uint32_t code; } others[] = {
		{ '1', '!', KEY_1 }, { '2', '@', KEY_2 }, { '3', '#', KEY_3 }, { '4', '$', KEY_4 },
		{ '5', '%', KEY_5 }, { '6', '^', KEY_6 }, { '7', '&', KEY_7 }, { '8', '*', KEY_8 },
		{ '9', '(', KEY_9 }, { '0', ')', KEY_0 }, { '-', '_', KEY_MINUS }, { '=', '+', KEY_EQUAL },
		{ '[', '{', KEY_LEFTBRACE }, { ']', '}', KEY_RIGHTBRACE }, { '\\', '|', KEY_BACKSLASH },
		{ ';', ':', KEY_SEMICOLON }, { '\'', '"', KEY_APOSTROPHE }, { '`', '~', KEY_GRAVE },
		{ ',', '<', KEY_COMMA }, { '.', '>', KEY_DOT }, { '/', '?', KEY_SLASH },
		{ ' ', 0, KEY_SPACE },
	};
	const char *p = c ? strchr(lower, tolower(c)) : NULL;
	if (p && isalpha(c)) {
		*code = letters[p - lower];
		*shift = isupper(c);
		return true;
	}
	for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
		if (c == others[i].plain || (others[i].shifted && c == others[i].shifted)) {
			*code = others[i].code;
			*shift = c == others[i].shifted;
			return true;
		}
	}
	return false;
}

static void type_text(const char *s)
{
	for (; *s; s++) {
		uint32_t code;
		bool shift;
		if (ascii_key(*s, &code, &shift))
			tap(code, shift);
	}
}

/* ---- swipe decoding ---- */

struct point { double x, y; };
struct word { char *text; double p, logp; int uses; bool listed; };

static struct point centre[26];
static bool have_centre[26];
static struct word *words;
static size_t nwords, capacity;
static const char *user_path;
static int unsaved;
static volatile sig_atomic_t terminated;

static void on_term(int sig) { terminated = 1; }

/* Typed without the apostrophe, these become the contraction. The word list
 * counts the part after the apostrophe on its own ("'t", "'m"); each
 * contraction gets a rough share of that count.
 */
static const struct { const char *typed, *word, *suffix; double share; } contractions[] = {
	{ "dont", "don't", "'t", 0.35 }, { "cant", "can't", "'t", 0.15 }, { "didnt", "didn't", "'t", 0.12 },
	{ "doesnt", "doesn't", "'t", 0.08 }, { "isnt", "isn't", "'t", 0.07 }, { "wont", "won't", "'t", 0.05 },
	{ "wasnt", "wasn't", "'t", 0.04 }, { "couldnt", "couldn't", "'t", 0.04 },
	{ "wouldnt", "wouldn't", "'t", 0.03 }, { "havent", "haven't", "'t", 0.03 },
	{ "shouldnt", "shouldn't", "'t", 0.02 }, { "arent", "aren't", "'t", 0.02 },
	{ "hasnt", "hasn't", "'t", 0.01 }, { "hadnt", "hadn't", "'t", 0.01 }, { "werent", "weren't", "'t", 0.01 },
	{ "im", "I'm", "'m", 1.0 }, { "ive", "I've", "'ve", 0.4 }, { "youve", "you've", "'ve", 0.2 },
	{ "theyve", "they've", "'ve", 0.1 }, { "youre", "you're", "'re", 0.5 }, { "theyre", "they're", "'re", 0.3 },
	{ "youll", "you'll", "'ll", 0.3 }, { "ill", "I'll", "'ll", 0.4 }, { "id", "I'd", "'d", 0.3 },
	{ "thats", "that's", "'s", 0.2 }, { "whats", "what's", "'s", 0.1 }, { "theres", "there's", "'s", 0.05 },
	{ "lets", "let's", "'s", 0.05 }, { "hes", "he's", "'s", 0.05 }, { "shes", "she's", "'s", 0.03 },
};
#define NCONTRACTIONS (sizeof(contractions) / sizeof(contractions[0]))

static void load_words(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "vkbd: no word list at %s\n", path);
		return;
	}
	capacity = MAX_WORDS + NCONTRACTIONS + 1024;
	words = calloc(capacity, sizeof(*words));
	char line[256], w[128];
	double total = 0, suffix[NCONTRACTIONS] = { 0 };
	long long count;
	while (nwords < MAX_WORDS && fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%127s %lld", w, &count) != 2 || count <= 0)
			continue;
		total += count;
		if (w[0] == '\'') {
			for (size_t i = 0; i < NCONTRACTIONS; i++)
				if (!strcmp(w, contractions[i].suffix))
					suffix[i] = count * contractions[i].share;
			continue;
		}
		bool ok = true;
		for (char *c = w; *c; c++)
			ok &= *c >= 'a' && *c <= 'z';
		if (!ok)
			continue;
		words[nwords].text = strdup(w);
		words[nwords].logp = (double)count;
		nwords++;
	}
	fclose(f);
	/* Each contraction replaces its spelling without the apostrophe, keeping
	 * the larger of the two counts, or is added. */
	for (size_t i = 0; i < NCONTRACTIONS; i++) {
		size_t j;
		for (j = 0; j < nwords && strcmp(words[j].text, contractions[i].typed); j++)
			;
		if (j == nwords) {
			words[j].logp = 0;
			nwords++;
		} else {
			free(words[j].text);
		}
		words[j].text = strdup(contractions[i].word);
		words[j].logp = fmax(words[j].logp, fmax(suffix[i], 1));
	}
	for (size_t i = 0; i < nwords; i++) {
		words[i].p = words[i].logp / total;
		words[i].logp = log(words[i].p);
		words[i].listed = true;
	}
	fprintf(stderr, "vkbd: %zu words from %s\n", nwords, path);
}

static void rescore(struct word *w)
{
	int uses = w->uses < 50 ? w->uses : 50;
	w->logp = w->listed ? log(w->p + 1e-4 * uses) : uses >= 2 ? log(2e-5 * uses) : -INFINITY;
}

static struct word *find_word(const char *text)
{
	for (size_t i = 0; i < nwords; i++)
		if (!strcasecmp(words[i].text, text))
			return &words[i];
	return NULL;
}

/* A word the user typed: letters and apostrophes, two or more. */
static void learn(const char *text, int uses)
{
	size_t len = strlen(text), letters = 0;
	for (size_t i = 0; i < len; i++) {
		if (!isalpha((unsigned char)text[i]) && text[i] != '\'')
			return;
		letters += isalpha((unsigned char)text[i]) != 0;
	}
	if (letters < 2 || len > 40)
		return;
	struct word *w = find_word(text);
	if (!w) {
		if (nwords == capacity) {
			struct word *more = realloc(words, (capacity * 2) * sizeof(*words));
			if (!more)
				return;
			words = more;
			capacity *= 2;
		}
		w = &words[nwords++];
		*w = (struct word){ .text = strdup(text), .p = 0, .uses = 0, .listed = false };
		for (char *c = w->text; *c; c++)
			*c = tolower(*c);
	}
	w->uses += uses;
	rescore(w);
}

static void save_user_words(void)
{
	if (!user_path || !unsaved)
		return;
	char tmp[4096];
	snprintf(tmp, sizeof(tmp), "%s.new", user_path);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return;
	for (size_t i = 0; i < nwords; i++)
		if (words[i].uses > 0)
			fprintf(f, "%s %d\n", words[i].text, words[i].uses);
	if (fclose(f) == 0)
		rename(tmp, user_path);
	unsaved = 0;
}

static void load_user_words(const char *path)
{
	user_path = path;
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	char w[128];
	int uses;
	while (fscanf(f, "%127s %d", w, &uses) == 2)
		if (uses > 0)
			learn(w, uses);
	fclose(f);
}

static size_t resample(const struct point *in, size_t n, struct point *out)
{
	double length = 0;
	for (size_t i = 1; i < n; i++)
		length += hypot(in[i].x - in[i - 1].x, in[i].y - in[i - 1].y);
	if (n == 1 || length == 0) {
		for (size_t i = 0; i < POINTS; i++)
			out[i] = in[0];
		return POINTS;
	}
	double step = length / (POINTS - 1), target = step, walked = 0;
	size_t k = 1, i = 0;
	out[0] = in[0];
	while (k < POINTS - 1 && i + 1 < n) {
		double seg = hypot(in[i + 1].x - in[i].x, in[i + 1].y - in[i].y);
		if (walked + seg >= target && seg > 0) {
			double t = (target - walked) / seg;
			out[k].x = in[i].x + (in[i + 1].x - in[i].x) * t;
			out[k].y = in[i].y + (in[i + 1].y - in[i].y) * t;
			k++;
			target += step;
		} else {
			walked += seg;
			i++;
		}
	}
	while (k < POINTS)
		out[k++] = in[n - 1];
	return POINTS;
}

static int letter(char c)
{
	c = tolower(c);
	return c >= 'a' && c <= 'z' && have_centre[c - 'a'] ? c - 'a' : -1;
}

static void decode(struct point *path, size_t n)
{
	struct point rp[POINTS], tp[POINTS], tmpl[128];
	struct { double cost; size_t i; } best[3] = { { INFINITY, 0 }, { INFINITY, 0 }, { INFINITY, 0 } };

	resample(path, n, rp);
	for (size_t w = 0; w < nwords; w++) {
		const char *s = words[w].text;
		if (!isfinite(words[w].logp))
			continue;	/* a new word used only once */
		size_t len = strlen(s), m = 0;
		int first = letter(s[0]), last = -1;
		for (size_t i = len; i-- > 0;)
			if ((last = letter(s[i])) >= 0)
				break;
		if (first < 0 || last < 0)
			continue;
		double ds = hypot(centre[first].x - path[0].x, centre[first].y - path[0].y);
		double de = hypot(centre[last].x - path[n - 1].x, centre[last].y - path[n - 1].y);
		if (ds > RADIUS || de > RADIUS)
			continue;
		for (size_t i = 0; i < len && m < 128; i++) {
			int l = letter(s[i]);
			if (l < 0)
				continue;	/* the apostrophe */
			if (!m || tmpl[m - 1].x != centre[l].x || tmpl[m - 1].y != centre[l].y)
				tmpl[m++] = centre[l];
		}
		if (m < 2)
			continue;	/* a swipe crosses at least two keys */
		resample(tmpl, m, tp);
		double d = 0;
		for (size_t i = 0; i < POINTS; i++)
			d += hypot(rp[i].x - tp[i].x, rp[i].y - tp[i].y);
		d /= POINTS;
		double cost = d * d / (2 * SIGMA * SIGMA) + (ds * ds + de * de) / (2 * END_SIGMA * END_SIGMA) -
			      words[w].logp;
		for (int b = 0; b < 3; b++) {
			if (cost < best[b].cost) {
				memmove(&best[b + 1], &best[b], (2 - b) * sizeof(best[0]));
				best[b].cost = cost;
				best[b].i = w;
				break;
			}
		}
	}
	printf("words");
	for (int b = 0; b < 3 && isfinite(best[b].cost); b++) {
		const char *s = words[best[b].i].text;
		printf(" %s", strcmp(s, "i") ? s : "I");
	}
	printf("\n");
	fflush(stdout);
}

static void parse_points(char *s, struct point *pts, size_t *n, size_t max)
{
	*n = 0;
	for (char *tok = strtok(s, " "); tok && *n < max; tok = strtok(NULL, " "))
		if (sscanf(tok, "%lf,%lf", &pts[*n].x, &pts[*n].y) == 2)
			(*n)++;
}

static void command(char *line)
{
	char *arg = strchr(line, ' ');
	if (arg)
		*arg++ = 0;
	if (!strcmp(line, "tap") && arg) {
		tap(strtoul(arg, NULL, 10), strstr(arg, "shift") != NULL);
	} else if (!strcmp(line, "mods")) {
		pending_mods = 0;
		if (arg && strstr(arg, "ctrl"))
			pending_mods |= 4;
		if (arg && strstr(arg, "alt"))
			pending_mods |= 8;
	} else if (!strcmp(line, "down") && arg) {
		key(strtoul(arg, NULL, 10), true);
	} else if (!strcmp(line, "up") && arg) {
		key(strtoul(arg, NULL, 10), false);
	} else if (!strcmp(line, "text") && arg) {
		type_text(arg);
	} else if (!strcmp(line, "keys") && arg) {
		memset(have_centre, 0, sizeof(have_centre));
		for (char *tok = strtok(arg, " "); tok; tok = strtok(NULL, " ")) {
			double x, y;
			char c;
			if (sscanf(tok, "%c:%lf,%lf", &c, &x, &y) == 3 && c >= 'a' && c <= 'z') {
				centre[c - 'a'] = (struct point){ x, y };
				have_centre[c - 'a'] = true;
			}
		}
	} else if (!strcmp(line, "learn") && arg) {
		learn(arg, 1);
		if (++unsaved >= 5)
			save_user_words();
	} else if (!strcmp(line, "swipe") && arg) {
		static struct point path[2048];
		size_t n;
		parse_points(arg, path, &n, 2048);
		if (n >= 2)
			decode(path, n);
		else
			printf("words\n"), fflush(stdout);
	}
}

int main(int argc, char **argv)
{
	bool test = false;
	if (argc > 1)
		load_words(argv[1]);
	for (int i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--test"))
			test = true;
		else
			load_user_words(argv[i]);
	}
	if (test) {
		/* Decoder test without Wayland: commands on stdin. */
		char line[65536];
		while (fgets(line, sizeof(line), stdin)) {
			line[strcspn(line, "\n")] = 0;
			command(line);
		}
		save_user_words();
		return 0;
	}
	signal(SIGTERM, on_term);
	display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "vkbd: no Wayland display\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (!seat || !manager) {
		fprintf(stderr, "vkbd: the compositor has no virtual keyboard\n");
		return 1;
	}
	vkbd = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(manager, seat);
	if (upload_keymap() < 0) {
		fprintf(stderr, "vkbd: no keymap\n");
		return 1;
	}
	wl_display_flush(display);

	char buffer[65536];
	size_t used = 0;
	struct pollfd fds[2] = { { wl_display_get_fd(display), POLLIN, 0 }, { 0, POLLIN, 0 } };
	for (;;) {
		wl_display_flush(display);
		if (poll(fds, 2, -1) < 0 && errno != EINTR)
			break;
		if (terminated)
			break;
		if (fds[0].revents & POLLIN && wl_display_dispatch(display) < 0)
			break;
		if (fds[0].revents & (POLLERR | POLLHUP))
			break;
		if (fds[1].revents & (POLLIN | POLLHUP)) {
			ssize_t got = read(0, buffer + used, sizeof(buffer) - 1 - used);
			if (got <= 0)
				break;	/* the keyboard window went away */
			used += got;
			buffer[used] = 0;
			char *start = buffer, *nl;
			while ((nl = strchr(start, '\n'))) {
				*nl = 0;
				command(start);
				start = nl + 1;
			}
			used -= start - buffer;
			memmove(buffer, start, used);
			if (used == sizeof(buffer) - 1)
				used = 0;	/* an overlong line: drop it */
		}
	}
	save_user_words();
	return 0;
}
