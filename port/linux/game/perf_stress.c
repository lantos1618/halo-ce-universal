/*
PERF_STRESS.C

The perf lab's stress test and tick timing (port/macos/README.md, "Perf lab").

debug.tick_stats (HALO_TICK_STATS=1) times every game tick (game_time.c
around game_tick) and the parts of it (game.c): the units, the AI, the
effects, the objects and the rest. Every debug.tick_stats_seconds it logs
the ticks' average and slowest time and how many objects and actors there
are.

debug.stress (HALO_STRESS) fills the level around the player with more and
more characters while it does:

- "actors:<count>[:<step>[:<seconds>]]": AI actors, the level's own kinds
  (its scenario's actor palette, round robin: on b30 marines, grunts,
  jackals, elites...), without an encounter, in rings around the player, so
  they see each other and fight;
- "bipeds:<count>[:<step>[:<seconds>]]": the same characters' bodies
  without AI (the player's own body on a map without an actor palette, such
  as a multiplayer map): their physics, animation and collision only.

<step> are added every <seconds> (defaults: 16 every 5 seconds), from
debug.stress_start seconds after the level starts, until <count>. The
player cannot die meanwhile (cheat_deathless_player). Before each step the
log gets a line

  stress: kind=actors spawned=64 objects=312 actors=64 ticks=148 tick_ms=3.214 max_ms=7.902 units_ms=... ai_ms=...

of the ticks since the previous step's first second, which
tools/perf_lab/stress_report.py turns into a table. The game's own limits
show where the lines stop growing: 256 actors (MAXIMUM_ACTORS,
source/ai/actors.h), 8192 objects (HALO_PORT_MAXIMUM_OBJECTS_PER_MAP).

It also ends a game without a window (debug.null_renderer) after
debug.exit_after seconds, which the platform layer counts from the window's
opening.

Called from the main loop every frame (main.c).
*/

#include "cseries.h"
#include "main/main.h"
#include "game/game.h"
#include "game/cheats.h"
#include "game/players.h"
#include "objects/objects.h"
#include "objects/object_types.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "ai/actors.h"
#include "ai/actor_definitions.h"
#include "ai/actor_placement.h"
#include "ai/ai_scenario_definitions.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* (hidden by the game's strict ANSI C; Linux's and musl's value) */
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
int clock_gettime(int clock, struct timespec *time);
#endif

/* the platform layer's (port/linux/src/port_config.c) */
const char *config_string(char const *name);
double config_real(char const *name);
int config_boolean(const char *name);
void platform_log(char const *format, ...);

enum
{
	_stress_off,
	_stress_actors,
	_stress_bipeds,
};

enum
{
	_tick_section_units,
	_tick_section_ai,
	_tick_section_effects,
	_tick_section_objects,
	_tick_section_other,
	NUMBER_OF_TICK_SECTIONS
};

static char const *const tick_section_names[NUMBER_OF_TICK_SECTIONS] = {
	"units", "ai", "effects", "objects", "other",
};

static struct
{
	boolean checked;
	boolean timing;
	/* the tick being timed */
	double tick_start;
	double section_start;
	double tick_sections[NUMBER_OF_TICK_SECTIONS];
	/* the ticks since the window began */
	long ticks;
	double total;
	double slowest;
	double sections[NUMBER_OF_TICK_SECTIONS];
	double window_seconds;
	real log_seconds;
} tick_timing;

static struct
{
	short kind;
	long count;
	long step;
	real interval;
	real start;
	real seconds;
	real next_step;
	real settle;
	long spawned;
	long failed;
	boolean started;
	boolean finished;
} stress;

static double clock_milliseconds(
	void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1000000.0;
}

static void perf_read_settings(
	void)
{
	char const *setting = config_string("debug.stress");
	char kind[16] = { 0 };
	long count = 0, step = 0;
	double interval = 0.0;

	tick_timing.checked = TRUE;
	stress.kind = _stress_off;
	if (setting && *setting && sscanf(setting, "%15[a-z]:%ld:%ld:%lf", kind, &count, &step, &interval) >= 2 &&
		count > 0)
	{
		if (!strcmp(kind, "actors"))
			stress.kind = _stress_actors;
		else if (!strcmp(kind, "bipeds"))
			stress.kind = _stress_bipeds;
		else
			platform_log("stress: unknown kind \"%s\" (actors or bipeds)", kind);
		stress.count = count;
		stress.step = step > 0 ? step : 16;
		stress.interval = interval > 0.0 ? (real)interval : 5.0f;
		stress.start = (real)config_real("debug.stress_start");
		if (stress.kind != _stress_off)
		{
			platform_log("stress: %s, %ld, %ld every %.1f seconds from %.1f seconds into the level", kind, stress.count,
				stress.step, stress.interval, stress.start);
		}
	}
	tick_timing.timing = stress.kind != _stress_off || config_boolean("debug.tick_stats");
	tick_timing.log_seconds = (real)config_real("debug.tick_stats_seconds");
	if (tick_timing.log_seconds <= 0.0f)
		tick_timing.log_seconds = 5.0f;
}

/* ---------- tick timing (game_time.c, game.c) */

void perf_tick_begin(
	void)
{
	if (!tick_timing.checked)
		perf_read_settings();
	if (!tick_timing.timing)
		return;
	tick_timing.tick_start = tick_timing.section_start = clock_milliseconds();
	memset(tick_timing.tick_sections, 0, sizeof(tick_timing.tick_sections));
}

/* the part of the tick since the last ended */
void perf_tick_section(
	long section)
{
	double now;

	if (!tick_timing.timing || section < 0 || section >= NUMBER_OF_TICK_SECTIONS)
		return;
	now = clock_milliseconds();
	tick_timing.tick_sections[section] += now - tick_timing.section_start;
	tick_timing.section_start = now;
}

void perf_tick_end(
	void)
{
	double now, elapsed;
	int section;

	if (!tick_timing.timing)
		return;
	now = clock_milliseconds();
	tick_timing.tick_sections[_tick_section_other] += now - tick_timing.section_start;
	elapsed = now - tick_timing.tick_start;
	tick_timing.ticks++;
	tick_timing.total += elapsed;
	if (elapsed > tick_timing.slowest)
		tick_timing.slowest = elapsed;
	for (section = 0; section < NUMBER_OF_TICK_SECTIONS; section++)
		tick_timing.sections[section] += tick_timing.tick_sections[section];
}

static void tick_timing_reset(
	void)
{
	tick_timing.ticks = 0;
	tick_timing.total = 0.0;
	tick_timing.slowest = 0.0;
	tick_timing.window_seconds = 0.0;
	memset(tick_timing.sections, 0, sizeof(tick_timing.sections));
}

static long unit_count(
	void)
{
	struct object_iterator iterator;
	long count = 0;

	object_iterator_new(&iterator, _object_mask_unit, 0);
	while (object_iterator_next(&iterator))
		count++;
	return count;
}

/* the window's ticks as one log line */
static void tick_timing_log(
	char const *prefix)
{
	char sections[256];
	int length = 0, section;
	double ticks = tick_timing.ticks ? (double)tick_timing.ticks : 1.0;

	for (section = 0; section < NUMBER_OF_TICK_SECTIONS; section++)
	{
		length += snprintf(sections + length, sizeof(sections) - length, " %s_ms=%.3f", tick_section_names[section],
			tick_timing.sections[section] / ticks);
	}
	platform_log("%s objects=%d actors=%d units=%ld ticks=%ld tick_ms=%.3f max_ms=%.3f%s", prefix,
		object_header_data ? object_header_data->actual_count : 0, actor_data ? actor_data->actual_count : 0,
		object_header_data ? unit_count() : 0L, tick_timing.ticks, tick_timing.total / ticks, tick_timing.slowest,
		sections);
}

/* ---------- the stress test */

/* the kth place around the center: a sunflower's seeds, a metre and a bit
apart */
static void stress_place(
	long k,
	real_point3d const *center,
	real_point3d *place)
{
	real angle = (real)k * 2.39996323f;
	real radius = 3.0f + 1.1f * (real)sqrt((double)k);

	place->x = center->x + radius * (real)cos(angle);
	place->y = center->y + radius * (real)sin(angle);
	place->z = center->z + 0.5f;
}

static long stress_palette_count(
	void)
{
	struct scenario *scenario = global_scenario_get();

	return scenario ? scenario->ai_actor_palette.count : 0;
}

/* the actor variant of palette entry index, or NONE */
static long stress_palette_variant(
	long index)
{
	struct scenario *scenario = global_scenario_get();
	struct tag_reference *entry;

	if (!scenario || index < 0 || index >= scenario->ai_actor_palette.count)
		return NONE;
	entry = TAG_BLOCK_GET_ELEMENT(&scenario->ai_actor_palette, index, struct tag_reference);
	return entry->index;
}

static boolean stress_spawn_one(
	long k,
	real_point3d const *center)
{
	long palette_count = stress_palette_count();
	long variant = palette_count > 0 ? stress_palette_variant(k % palette_count) : NONE;
	real_point3d place;

	stress_place(k, center, &place);
	if (stress.kind == _stress_actors)
	{
		struct actor_starting_location location;

		if (variant == NONE)
			return FALSE;
		memset(&location, 0, sizeof(location));
		location.position = place;
		location.facing = (real)k * 2.39996323f + 3.14159265f;
		location.cluster_index = NONE;
		location.noncombat_sequence_id = NONE;
		location.actor_variant_index = NONE;
		location.command_list_index = NONE;
		return actor_place(variant, NONE, NONE, &location, FALSE, 0) != NONE;
	}
	else
	{
		struct object_placement_data data;
		long definition_index = NONE;

		if (variant != NONE)
		{
			definition_index = actor_variant_definition_get(variant)->unit_reference.index;
		}
		else
		{
			long player_index = local_player_get_player_index(0);
			long unit_index = player_index != NONE ? player_get(player_index)->unit_index : NONE;
			struct object_datum *unit = unit_index != NONE ?
				object_try_and_get_and_verify_type(unit_index, _object_mask_unit) : NULL;

			if (unit)
				definition_index = unit->definition_index;
		}
		if (definition_index == NONE)
			return FALSE;
		object_placement_data_new(&data, definition_index, NONE);
		data.position = place;
		data.forward.i = -(real)cos((real)k * 2.39996323f);
		data.forward.j = -(real)sin((real)k * 2.39996323f);
		data.forward.k = 0.0f;
		return object_new(&data) != NONE;
	}
}

static void stress_step(
	void)
{
	long player_index = local_player_get_player_index(0);
	long unit_index = player_index != NONE ? player_get(player_index)->unit_index : NONE;
	struct object_datum *unit = unit_index != NONE ?
		object_try_and_get_and_verify_type(unit_index, _object_mask_unit) : NULL;
	real_point3d center;
	long index, placed = 0;

	if (!unit)
		return;
	center = unit->object.position;
	for (index = 0; index < stress.step && stress.spawned + stress.failed < stress.count; index++)
	{
		if (stress_spawn_one(stress.spawned + stress.failed, &center))
		{
			stress.spawned++;
			placed++;
		}
		else
		{
			stress.failed++;
		}
	}
	platform_log("stress: placed %ld (%ld in all, %ld could not be)", placed, stress.spawned, stress.failed);
}

/* debug.exit_after without a window (debug.null_renderer): the platform
layer counts from the window's opening, and there is none */
static void perf_exit_without_window(
	void)
{
	static double exit_time = -1.0;

	if (exit_time < 0.0)
	{
		double seconds = config_boolean("debug.null_renderer") ? config_real("debug.exit_after") : 0.0;

		exit_time = seconds > 0.0 ? clock_milliseconds() + seconds * 1000.0 : 0.0;
	}
	if (exit_time > 0.0 && clock_milliseconds() >= exit_time)
	{
		platform_log("exiting after debug.exit_after");
		exit(0);
	}
}

void perf_stress_update(
	boolean main_menu_loaded,
	real seconds)
{
	if (!tick_timing.checked)
		perf_read_settings();
	perf_exit_without_window();
	if (!tick_timing.timing)
		return;
	if (!game_in_progress() || main_menu_loaded)
	{
		stress.seconds = 0.0f;
		return;
	}
	stress.seconds += seconds;
	tick_timing.window_seconds += seconds;
	if (stress.kind == _stress_off || stress.finished)
	{
		if (tick_timing.window_seconds >= tick_timing.log_seconds)
		{
			tick_timing_log("tick stats:");
			tick_timing_reset();
		}
		return;
	}
	if (stress.seconds < stress.start)
		return;
	if (!stress.started)
	{
		stress.started = TRUE;
		stress.next_step = stress.seconds;
		cheat.deathless_player = TRUE;
		tick_timing_reset();
	}
	/* the ticks from a second after a step (the new characters landed and
	woken) to the next */
	if (stress.settle > 0.0f && stress.seconds >= stress.settle)
	{
		stress.settle = 0.0f;
		tick_timing_reset();
	}
	if (stress.seconds >= stress.next_step)
	{
		char prefix[96];

		snprintf(prefix, sizeof(prefix), "stress: kind=%s spawned=%ld",
			stress.kind == _stress_actors ? "actors" : "bipeds", stress.spawned);
		if (tick_timing.ticks > 0)
			tick_timing_log(prefix);
		if (stress.spawned + stress.failed >= stress.count)
		{
			platform_log("stress: done, %ld placed, %ld could not be", stress.spawned, stress.failed);
			stress.finished = TRUE;
			tick_timing_reset();
			return;
		}
		stress_step();
		stress.next_step = stress.seconds + stress.interval;
		stress.settle = stress.seconds + 1.0f;
		tick_timing_reset();
	}
}
