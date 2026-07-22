/*
 * fd_schema.c -- template validation and dump metadata.
 *
 * One table names every template/slot the C side depends on. fd_create
 * validates it once, caches the templates the library reads often, and
 * fd_dump_state reuses the same slot metadata to print working memory.
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "fd_internal.h"

#define FD_SCHEMA_NO_CACHE ((size_t)-1)

enum fd_schema_slot_kind {
	FD_SCHEMA_INTEGER = 0,
	FD_SCHEMA_SYMBOL,
	FD_SCHEMA_STRING
};

struct fd_schema_slot {
	const char         *name;
	enum fd_schema_slot_kind kind;
	const char *const  *allowed;
	int                 n_allowed;
};

struct fd_schema_template {
	const char                  *name;
	size_t                       cache_offset;
	const char                  *dump_section;
	const struct fd_schema_slot *slots;
	int                          n_slots;
};

static const char *const PHASE_ALLOWED[] = {
	"pregame", "playing", "voting", "results", "gameover"
};

static const char *const BOOL_ALLOWED[] = {
	"t", "nil"
};

static const char *const STATUS_ALLOWED[] = {
	"alive", "dead", "ghost", "ejected", "unknown"
};

static const char *const VENT_BASIS_ALLOWED[] = {
	"inferred", "observed"
};

static const char *const CLAIM_KIND_ALLOWED[] = {
	"location", "task", "self-report"
};

static const char *const LOGODDS_SOURCE_ALLOWED[] = {
	"prior", "cast-iron", "near-body", "vent-observed",
	"vent-inferred", "accusation", "off-task", "on-task",
	"contradiction", "self-report", "co-location", "silent-witness",
	"social-accuse-impostor", "social-defend-impostor",
	"social-co-accuse", "social-pack-cleared"
};

static const char *const PRESSURE_ALLOWED[] = {
	"low", "medium", "high", "critical"
};

static const char *const STANCE_ALLOWED[] = {
	"on-task", "off-task", "threat", "unknown"
};

static const char *const EVIDENCE_ALLOWED[] = {
	"none", "accusation", "vent", "near-body", "off-task", "on-task"
};

static const struct fd_schema_slot SELF_COLOR_SLOTS[] = {
	{ "color", FD_SCHEMA_SYMBOL, NULL, 0 }
};

static const struct fd_schema_slot SELF_STATE_SLOTS[] = {
	{ "tick", FD_SCHEMA_INTEGER, NULL, 0 },
	{ "phase", FD_SCHEMA_SYMBOL, PHASE_ALLOWED, 5 },
	{ "room", FD_SCHEMA_SYMBOL, NULL, 0 },
	{ "kill-ready", FD_SCHEMA_SYMBOL, BOOL_ALLOWED, 2 }
};

static const struct fd_schema_slot CREWMEMBER_SLOTS[] = {
	{ "color", FD_SCHEMA_SYMBOL, NULL, 0 },
	{ "status", FD_SCHEMA_SYMBOL, STATUS_ALLOWED, 5 },
	{ "last-seen-tick", FD_SCHEMA_INTEGER, NULL, 0 },
	{ "last-seen-room", FD_SCHEMA_SYMBOL, NULL, 0 },
	{ "visible-tasks", FD_SCHEMA_INTEGER, NULL, 0 }
};

static const struct fd_schema_slot IMPOSTOR_FOUND_SLOTS[] = {
	{ "color", FD_SCHEMA_SYMBOL, NULL, 0 },
	{ "tick", FD_SCHEMA_INTEGER, NULL, 0 }
};

static const struct fd_schema_slot CASE_SLOTS[] = {
	{ "victim", FD_SCHEMA_SYMBOL, NULL, 0 },
	{ "room", FD_SCHEMA_SYMBOL, NULL, 0 },
	{ "body-tick", FD_SCHEMA_INTEGER, NULL, 0 }
};

static const struct fd_schema_slot KILL_WINDOW_SLOTS[] = {
	{ "victim", FD_SCHEMA_SYMBOL, NULL, 0 },
	{ "from-tick", FD_SCHEMA_INTEGER, NULL, 0 },
	{ "to-tick", FD_SCHEMA_INTEGER, NULL, 0 },
	{ "room", FD_SCHEMA_SYMBOL, NULL, 0 }
};

static const struct fd_schema_slot ALIBI_SLOTS[] = {
	{ "color", FD_SCHEMA_SYMBOL, NULL, 0 },
	{ "victim", FD_SCHEMA_SYMBOL, NULL, 0 },
	{ "at-room", FD_SCHEMA_SYMBOL, NULL, 0 },
	{ "at-tick", FD_SCHEMA_INTEGER, NULL, 0 }
};

static const struct fd_schema_slot CAST_IRON_ALIBI_SLOTS[] = {
	{ "color", FD_SCHEMA_SYMBOL, NULL, 0 },
	{ "victim", FD_SCHEMA_SYMBOL, NULL, 0 }
};

static const struct fd_schema_slot SUSPECT_SLOTS[] = {
	{ "color", FD_SCHEMA_SYMBOL, NULL, 0 },
	{ "victim", FD_SCHEMA_SYMBOL, NULL, 0 },
	{ "basis", FD_SCHEMA_STRING, NULL, 0 }
};

static const struct fd_schema_slot VENT_SUSPECTED_SLOTS[] = {
	{ "color", FD_SCHEMA_SYMBOL, NULL, 0 },
	{ "from-room", FD_SCHEMA_SYMBOL, NULL, 0 },
	{ "to-room", FD_SCHEMA_SYMBOL, NULL, 0 },
	{ "delta-tick", FD_SCHEMA_INTEGER, NULL, 0 },
	{ "tick", FD_SCHEMA_INTEGER, NULL, 0 },
	{ "basis", FD_SCHEMA_SYMBOL, VENT_BASIS_ALLOWED, 2 }
};

static const struct fd_schema_slot ROOM_DISTANCE_SLOTS[] = {
	{ "from", FD_SCHEMA_SYMBOL, NULL, 0 },
	{ "to", FD_SCHEMA_SYMBOL, NULL, 0 },
	{ "cost", FD_SCHEMA_INTEGER, NULL, 0 }
};

static const struct fd_schema_slot ACCUSATION_SLOTS[] = {
	{ "victim", FD_SCHEMA_SYMBOL, NULL, 0 },
	{ "color", FD_SCHEMA_SYMBOL, NULL, 0 }
};

static const struct fd_schema_slot SUSPICION_SLOTS[] = {
	{ "color", FD_SCHEMA_SYMBOL, NULL, 0 },
	{ "weight", FD_SCHEMA_INTEGER, NULL, 0 },
	{ "logodds", FD_SCHEMA_INTEGER, NULL, 0 },
	{ "basis", FD_SCHEMA_STRING, NULL, 0 }
};

static const struct fd_schema_slot LOGODDS_TERM_SLOTS[] = {
	{ "color", FD_SCHEMA_SYMBOL, NULL, 0 },
	{ "source", FD_SCHEMA_SYMBOL, LOGODDS_SOURCE_ALLOWED, 16 },
	{ "key", FD_SCHEMA_STRING, NULL, 0 },
	{ "amount", FD_SCHEMA_INTEGER, NULL, 0 }
};

static const struct fd_schema_slot ROSTER_SLOTS[] = {
	{ "n-players", FD_SCHEMA_INTEGER, NULL, 0 },
	{ "n-impostors", FD_SCHEMA_INTEGER, NULL, 0 },
	{ "alive-crew", FD_SCHEMA_INTEGER, NULL, 0 },
	{ "alive-impostors", FD_SCHEMA_INTEGER, NULL, 0 },
	{ "impostors-ejected", FD_SCHEMA_INTEGER, NULL, 0 }
};

static const struct fd_schema_slot GAME_PRESSURE_SLOTS[] = {
	{ "level", FD_SCHEMA_SYMBOL, PRESSURE_ALLOWED, 4 }
};

static const struct fd_schema_slot STANCE_SLOTS[] = {
	{ "color", FD_SCHEMA_SYMBOL, NULL, 0 },
	{ "category", FD_SCHEMA_SYMBOL, STANCE_ALLOWED, 4 },
	{ "evidence", FD_SCHEMA_SYMBOL, EVIDENCE_ALLOWED, 6 },
	{ "rationale", FD_SCHEMA_STRING, NULL, 0 }
};

static const struct fd_schema_slot NEAR_BODY_SLOTS[] = {
	{ "color", FD_SCHEMA_SYMBOL, NULL, 0 },
	{ "room", FD_SCHEMA_SYMBOL, NULL, 0 },
	{ "tick", FD_SCHEMA_INTEGER, NULL, 0 }
};

static const struct fd_schema_slot EDGE_SLOTS[] = {
	{ "a", FD_SCHEMA_SYMBOL, NULL, 0 },
	{ "b", FD_SCHEMA_SYMBOL, NULL, 0 },
	{ "tick", FD_SCHEMA_INTEGER, NULL, 0 }
};

static const struct fd_schema_slot CONTRADICTION_SLOTS[] = {
	{ "color", FD_SCHEMA_SYMBOL, NULL, 0 },
	{ "basis", FD_SCHEMA_STRING, NULL, 0 }
};

static const struct fd_schema_slot CLAIM_SLOTS[] = {
	{ "color", FD_SCHEMA_SYMBOL, NULL, 0 },
	{ "kind", FD_SCHEMA_SYMBOL, CLAIM_KIND_ALLOWED, 3 },
	{ "room", FD_SCHEMA_SYMBOL, NULL, 0 },
	{ "tick", FD_SCHEMA_INTEGER, NULL, 0 }
};

static const struct fd_schema_template SCHEMA[] = {
	{ "roster", offsetof(struct fd_detector, roster_tmpl),
	  "round model", ROSTER_SLOTS, 5 },
	{ "game-pressure", offsetof(struct fd_detector, game_pressure_tmpl),
	  "round model", GAME_PRESSURE_SLOTS, 1 },
	{ "crewmember", offsetof(struct fd_detector, crewmember_tmpl),
	  "dossier", CREWMEMBER_SLOTS, 5 },
	{ "case", offsetof(struct fd_detector, case_tmpl),
	  "case evidence", CASE_SLOTS, 3 },
	{ "near-body", FD_SCHEMA_NO_CACHE, "case evidence", NEAR_BODY_SLOTS, 3 },
	{ "vent-suspected", offsetof(struct fd_detector, vent_suspected_tmpl),
	  "case evidence", VENT_SUSPECTED_SLOTS, 6 },
	{ "accusation", offsetof(struct fd_detector, accusation_tmpl),
	  "case evidence", ACCUSATION_SLOTS, 2 },
	{ "accuses", FD_SCHEMA_NO_CACHE, "social / testimony", EDGE_SLOTS, 3 },
	{ "defends", FD_SCHEMA_NO_CACHE, "social / testimony", EDGE_SLOTS, 3 },
	{ "contradiction", FD_SCHEMA_NO_CACHE, "social / testimony",
	  CONTRADICTION_SLOTS, 2 },
	{ "logodds-term", offsetof(struct fd_detector, logodds_term_tmpl),
	  "belief", LOGODDS_TERM_SLOTS, 4 },
	{ "suspicion", offsetof(struct fd_detector, suspicion_tmpl),
	  "belief", SUSPICION_SLOTS, 4 },
	{ "stance", offsetof(struct fd_detector, stance_tmpl),
	  "verdict", STANCE_SLOTS, 4 },
	{ "self-color", offsetof(struct fd_detector, self_color_tmpl), NULL,
	  SELF_COLOR_SLOTS, 1 },
	{ "self-state", offsetof(struct fd_detector, self_state_tmpl), NULL,
	  SELF_STATE_SLOTS, 4 },
	{ "impostor-found", offsetof(struct fd_detector, impostor_found_tmpl), NULL,
	  IMPOSTOR_FOUND_SLOTS, 2 },
	{ "kill-window", offsetof(struct fd_detector, kill_window_tmpl), NULL,
	  KILL_WINDOW_SLOTS, 4 },
	{ "alibi", offsetof(struct fd_detector, alibi_tmpl), NULL,
	  ALIBI_SLOTS, 4 },
	{ "cast-iron-alibi", offsetof(struct fd_detector, cast_iron_alibi_tmpl), NULL,
	  CAST_IRON_ALIBI_SLOTS, 2 },
	{ "suspect", offsetof(struct fd_detector, suspect_tmpl), NULL,
	  SUSPECT_SLOTS, 3 },
	{ "room-distance", offsetof(struct fd_detector, room_distance_tmpl), NULL,
	  ROOM_DISTANCE_SLOTS, 3 },
	{ "claim", FD_SCHEMA_NO_CACHE, NULL, CLAIM_SLOTS, 4 }
};

static const char *
slot_kind_name(enum fd_schema_slot_kind kind)
{
	switch (kind) {
	case FD_SCHEMA_INTEGER: return "INTEGER";
	case FD_SCHEMA_SYMBOL:  return "SYMBOL";
	case FD_SCHEMA_STRING:  return "STRING";
	default:                return "?";
	}
}

static Deftemplate *
schema_lookup_template(fd_detector *fd, const struct fd_schema_template *tmpl)
{
	if (tmpl->cache_offset != FD_SCHEMA_NO_CACHE) {
		Deftemplate **slot = (Deftemplate **)((char *)fd + tmpl->cache_offset);
		if (*slot != NULL)
			return *slot;
	}
	return FindDeftemplate(fd->env, tmpl->name);
}

static bool
multifield_has_symbol(const Multifield *mf, const char *want)
{
	for (size_t i = 0; i < mf->length; i++) {
		const CLIPSValue *cv = &mf->contents[i];
		if (cv->header != NULL && cv->header->type == SYMBOL_TYPE &&
		    strcmp(cv->lexemeValue->contents, want) == 0)
			return true;
	}
	return false;
}

static bool
validate_slot_types(Deftemplate *tmpl, const struct fd_schema_slot *slot)
{
	CLIPSValue types;
	if (!DeftemplateSlotTypes(tmpl, slot->name, &types))
		return false;
	if (types.header == NULL || types.header->type != MULTIFIELD_TYPE) {
		fprintf(stderr, "fakedetector: %s.%s type metadata unavailable\n",
		    tmpl->header.name->contents, slot->name);
		return false;
	}
	if (types.multifieldValue->length != 1 ||
	    !multifield_has_symbol(types.multifieldValue,
	        slot_kind_name(slot->kind))) {
		fprintf(stderr, "fakedetector: %s.%s expected single %s slot\n",
		    tmpl->header.name->contents, slot->name,
		    slot_kind_name(slot->kind));
		return false;
	}
	return true;
}

static bool
validate_allowed_values(Deftemplate *tmpl, const struct fd_schema_slot *slot)
{
	if (slot->allowed == NULL)
		return true;
	CLIPSValue allowed;
	if (!DeftemplateSlotAllowedValues(tmpl, slot->name, &allowed))
		return false;
	if (allowed.header == NULL || allowed.header->type != MULTIFIELD_TYPE) {
		fprintf(stderr, "fakedetector: %s.%s missing allowed-values set\n",
		    tmpl->header.name->contents, slot->name);
		return false;
	}
	if ((int)allowed.multifieldValue->length != slot->n_allowed) {
		fprintf(stderr, "fakedetector: %s.%s allowed-values length mismatch\n",
		    tmpl->header.name->contents, slot->name);
		return false;
	}
	for (int i = 0; i < slot->n_allowed; i++) {
		if (!multifield_has_symbol(allowed.multifieldValue, slot->allowed[i])) {
			fprintf(stderr,
			    "fakedetector: %s.%s missing allowed value '%s'\n",
			    tmpl->header.name->contents, slot->name, slot->allowed[i]);
			return false;
		}
	}
	return true;
}

static bool
validate_template(fd_detector *fd, const struct fd_schema_template *tmpl)
{
	Deftemplate *dt = FindDeftemplate(fd->env, tmpl->name);
	if (dt == NULL) {
		fprintf(stderr, "fakedetector: missing template '%s'\n", tmpl->name);
		return false;
	}
	if (tmpl->cache_offset != FD_SCHEMA_NO_CACHE) {
		Deftemplate **slot = (Deftemplate **)((char *)fd + tmpl->cache_offset);
		*slot = dt;
	}
	for (int i = 0; i < tmpl->n_slots; i++) {
		const struct fd_schema_slot *slot = &tmpl->slots[i];
		if (!DeftemplateSlotExistP(dt, slot->name)) {
			fprintf(stderr, "fakedetector: %s missing slot '%s'\n",
			    tmpl->name, slot->name);
			return false;
		}
		if (!DeftemplateSlotSingleP(dt, slot->name)) {
			fprintf(stderr, "fakedetector: %s.%s must be single-field\n",
			    tmpl->name, slot->name);
			return false;
		}
		if (!validate_slot_types(dt, slot))
			return false;
		if (!validate_allowed_values(dt, slot))
			return false;
	}
	return true;
}

static void
dump_template(struct fd_sink *out, fd_detector *fd,
    const struct fd_schema_template *tmpl)
{
	Deftemplate *t = schema_lookup_template(fd, tmpl);
	if (t == NULL) {
		fd_sink_printf(out, "  (%s) -- template absent\n", tmpl->name);
		return;
	}
	int count = 0;
	for (Fact *f = GetNextFactInTemplate(t, NULL); f != NULL;
	     f = GetNextFactInTemplate(t, f)) {
		fd_sink_printf(out, "  (%s", tmpl->name);
		for (int i = 0; i < tmpl->n_slots; i++) {
			const struct fd_schema_slot *slot = &tmpl->slots[i];
			if (slot->kind == FD_SCHEMA_INTEGER)
				fd_sink_printf(out, " %s=%lld", slot->name,
				    fd_fact_int(f, slot->name));
			else
				fd_sink_printf(out, " %s='%s'", slot->name,
				    fd_fact_symbol(f, slot->name));
		}
		fd_sink_printf(out, ")\n");
		count++;
	}
	if (count == 0)
		fd_sink_printf(out, "  (%s) -- none\n", tmpl->name);
}

bool
fd_schema_cache(fd_detector *fd)
{
	if (fd == NULL || fd->env == NULL)
		return false;
	for (size_t i = 0; i < sizeof SCHEMA / sizeof SCHEMA[0]; i++) {
		if (!validate_template(fd, &SCHEMA[i]))
			return false;
	}
	return true;
}

void
fd_schema_dump_state(fd_detector *fd, struct fd_sink *sink)
{
	const char *section = NULL;

	fd_sink_printf(sink, "fd_dump_state: working-memory snapshot\n");
	for (size_t i = 0; i < sizeof SCHEMA / sizeof SCHEMA[0]; i++) {
		const struct fd_schema_template *tmpl = &SCHEMA[i];
		if (tmpl->dump_section == NULL)
			continue;
		if (section == NULL || strcmp(section, tmpl->dump_section) != 0) {
			section = tmpl->dump_section;
			fd_sink_printf(sink, "[%s]\n", section);
		}
		dump_template(sink, fd, tmpl);
	}
}
