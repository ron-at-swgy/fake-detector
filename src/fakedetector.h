/*
 * fakedetector.h -- public API for libfakedetector
 *
 * Crewmate-side rules engine for the Among Them game (an Among Us clone).
 * Ingests per-tick observations from a caller and produces, at vote time,
 * a per-color stance (on-task / off-task / threat / unknown), a per-color
 * status (alive / dead / ejected), a composite summary string suitable
 * for the game's vote-chat slot, and a direct vote-target query.
 *
 * Library form: an Among Them player policy links against this and uses
 * the snprintf-style render API to fill its own buffer.
 *
 * Identity assumption: every observation and query is keyed on a stable
 * per-player integer id (fd_player, 0..FD_MAX_PLAYERS-1). The caller
 * must already know which id each observed player is, and keep that
 * mapping stable across the round. Games with native stable identity
 * (color-keyed games like CrewRift / Among Us clones, seat-keyed games
 * like Diplomacy) map their identity directly onto ids; register the
 * display names with fd_set_player_name so rationales speak the game's
 * language (see fd_crewrift.h for a ready-made CrewRift mapping). A
 * policy whose perception does NOT track identity (e.g. vision that
 * wildcards the suit) must bridge that gap itself before calling in --
 * the library cannot reason about an unidentified player.
 */

#ifndef FAKEDETECTOR_H
#define FAKEDETECTOR_H

/*
 * Library version (semver) and ABI version. FD_ABI_VERSION bumps on any
 * ABI-incompatible change (struct layout, enum reordering, signature
 * change); the shared library's soname tracks it. Bindings should call
 * fd_abi_version() at load time and refuse a mismatch — the macros
 * describe the header they compiled against, the functions report the
 * library actually loaded. See docs/abi.md for the compatibility policy.
 */
#define FD_VERSION_MAJOR  1
#define FD_VERSION_MINOR  0
#define FD_VERSION_PATCH  0
#define FD_VERSION_STRING "1.0.0"
#define FD_ABI_VERSION    1

#include <stddef.h>
#include <stdio.h>   /* FILE, for the telemetry stream API */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A player identity: a plain integer id, 0..FD_MAX_PLAYERS-1, chosen
 * and kept stable by the caller (a color index, a seat number, a join
 * order -- whatever the game supplies). FD_MAX_PLAYERS is a
 * compile-time capacity cap sizing internal per-player state; the
 * live player count for a round comes from fd_observe_round_config.
 * Register display names with fd_set_player_name; unnamed ids render
 * as "p0".."p31".
 */
typedef int fd_player;

#define FD_MAX_PLAYERS 32

typedef enum {
	FD_PHASE_PREGAME = 0,
	FD_PHASE_PLAYING,
	FD_PHASE_VOTING,
	FD_PHASE_RESULTS,
	FD_PHASE_GAMEOVER
} fd_phase;

typedef enum {
	FD_STANCE_UNKNOWN = 0,
	FD_STANCE_ON_TASK,
	FD_STANCE_OFF_TASK,
	FD_STANCE_THREAT
} fd_stance;

/*
 * The kind of claim a player made in vote-chat, as classified by the
 * policy's NLP. Passed to fd_observe_claim. LOCATION ("I was in R") and
 * TASK ("I was doing a task in R") both place the speaker in a room and
 * are checked against the sighting record for contradictions;
 * SELF_REPORT ("I found the body") is a documented impostor tell.
 */
typedef enum {
	FD_CLAIM_LOCATION = 0,
	FD_CLAIM_TASK,
	FD_CLAIM_SELF_REPORT
} fd_claim_kind;

/*
 * Why a color carries its stance — the kind of evidence behind the
 * verdict. Orthogonal to fd_stance: a THREAT may rest on an accusation,
 * an impossible-movement (vent) sighting, or a near-body sighting; a
 * non-threat carries on-task / off-task. FD_EVIDENCE_NONE is a real
 * value, not an error — a freshly seen color with no read yet, or a
 * threat demoted after the impostor was already found this round.
 *
 * Lets a policy threshold votes on evidence quality: e.g. act on
 * NEAR_BODY / VENT / ACCUSATION but skip an OFF_TASK-only target rather
 * than eject on weak evidence. See fd_get_evidence.
 */
typedef enum {
	FD_EVIDENCE_NONE = 0,
	FD_EVIDENCE_ACCUSATION,
	FD_EVIDENCE_VENT,
	FD_EVIDENCE_NEAR_BODY,
	FD_EVIDENCE_OFF_TASK,
	FD_EVIDENCE_ON_TASK
} fd_evidence;

/* Orthogonal to stance: alive / dead / ejected. A non-alive color
 * receives no stance fact and is excluded from fd_pick_vote_target. */
typedef enum {
	FD_PLAYER_STATUS_UNKNOWN = 0,
	FD_PLAYER_STATUS_ALIVE,
	FD_PLAYER_STATUS_DEAD,
	FD_PLAYER_STATUS_EJECTED
} fd_player_status;

/* Safety projection of stance + status, for "alone with X" queries. */
typedef enum {
	FD_RISK_UNKNOWN = 0,   /* alive but we have no read on them yet */
	FD_RISK_NONE,          /* confirmed safe: dead, ejected, self, OR
	                          impostor already found this round */
	FD_RISK_LOW,           /* on-task — visible cooperative behavior */
	FD_RISK_MEDIUM,        /* off-task or unknown — baseline suspicion */
	FD_RISK_HIGH           /* threat — strong negative signal */
} fd_risk;

/*
 * Playstyle / suspicion baseline. Modulates the consumer-side
 * projections (fd_alone_risk, fd_pick_vote_target, and the
 * rationale prefix) without changing the underlying deductive
 * reasoning — stance and case-file output stay objective.
 */
typedef enum {
	FD_PLAYSTYLE_TRUSTING = 0,  /* vote only on hard threats; risk dampened */
	FD_PLAYSTYLE_NEUTRAL  = 1,  /* default; matches legacy behavior */
	FD_PLAYSTYLE_PARANOID = 2   /* vote even on weak signal; risk amplified */
} fd_playstyle;

/*
 * Parity-aware vote urgency, derived from the live (crew, impostor)
 * counts. Pressure rises as the alive crew count approaches the loss
 * condition; fd_vote_decision lowers the evidence bar as it climbs (a
 * wrong skip near parity loses the game outright). See fd_game_pressure.
 */
typedef enum {
	FD_PRESSURE_LOW = 0,
	FD_PRESSURE_MEDIUM,
	FD_PRESSURE_HIGH,
	FD_PRESSURE_CRITICAL
} fd_game_pressure_t;

/* What fd_vote_decision recommends doing with the current meeting. */
typedef enum {
	FD_VOTE_SKIP = 0,    /* no candidate beats the 1-in-N prior */
	FD_VOTE_ABSTAIN,     /* distribution too flat / no live pool */
	FD_VOTE_CAST         /* cast a vote for `target` */
} fd_vote_action;

/*
 * The output of fd_vote_decision: an expected-value vote recommendation
 * over the calibrated suspicion distribution (see fd_get_suspicion).
 *   target         - highest-weight alive non-self color.
 *   suspicion      - target's belief weight, 0..1000 per mille.
 *   confidence     - target's weight minus the runner-up's, 0..1000.
 *   recommendation - SKIP / ABSTAIN / CAST.
 *   rationale      - one-line prose; states the skip/abstain reason.
 */
typedef struct {
	fd_player      target;
	int            suspicion;
	int            confidence;
	fd_vote_action recommendation;
	char           rationale[256];
} fd_vote_decision_t;

typedef struct fd_detector fd_detector;

/* ==================================================================
 * API conventions
 *
 * Memory model
 *   - Strings passed IN (room names, clp_dir, rule names) are either
 *     copied or consumed before the call returns. The caller keeps
 *     ownership and may free or reuse them immediately afterward.
 *   - The caller owns every output buffer and array it passes by
 *     pointer; the library writes only within the stated capacity and
 *     frees none of it.
 *   - The only pointers the library hands back are the opaque
 *     fd_detector * (released by fd_destroy) and fd_player_name's
 *     strings (which live until the id is re-registered or the
 *     detector is destroyed). The caller frees neither -- and frees
 *     nothing else from this library.
 *
 * Return values
 *   - Commands -- state changes that cannot meaningfully fail per
 *     call -- return void: the fd_observe_* family, fd_run, fd_set_*,
 *     fd_reset, fd_add_room_link, fd_observe_route_*, fd_destroy.
 *   - Pure getters always have an answer -- "unknown" is a real value,
 *     not an error -- and return it directly: fd_get_stance,
 *     fd_get_evidence, fd_get_status, fd_alone_risk, fd_get_playstyle,
 *     fd_player_name (whose "unknown" is NULL), and fd_room_distance
 *     (whose "unknown / unreachable" is -1).
 *   - Out-parameter queries return a status code in which a NEGATIVE
 *     value always means error (bad arguments): the snprintf-style
 *     long of the fd_render_* / fd_*_rationale family, the 1/0/-1 int
 *     of fd_pick_vote_target and fd_last_seen, and the 0..max element
 *     count of fd_room_occupants and fd_rank_suspects.
 * ================================================================== */

/* ------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------ */

/*
 * Version of the library actually linked/loaded, as opposed to the
 * FD_VERSION_* macros describing the header compiled against. The
 * string is static (lifetime of the program). Bindings must verify
 * fd_abi_version() == FD_ABI_VERSION before any other call.
 */
const char *fd_version(void);
int         fd_abi_version(void);

/*
 * Create a detector. clp_dir is the directory containing the rule
 * files (templates.clp, dossier.clp, ..., stance.clp). Pass NULL to
 * load the rule set embedded in the library at build time — the
 * default for production consumers, requiring no files on disk (and,
 * on OpenBSD, no rpath pledge window). Pass a directory to load from
 * disk instead, for rule iteration without rebuilding.
 * Returns NULL on failure (file not found, parse error, allocation).
 *
 * (Compatibility note: NULL formerly meant "./clp"; callers that want
 * the old behavior pass "clp" explicitly.)
 */
fd_detector *fd_create(const char *clp_dir);

/*
 * Release a detector and everything it owns -- the CLIPS environment,
 * the interned room-graph names, the last-sighting cache. The handle
 * is invalid after this call. NULL-safe.
 */
void         fd_destroy(fd_detector *fd);

/*
 * Clear all working-memory facts (self-color, observations, dossier,
 * evidence, stances). Templates and compiled rules persist. Call
 * between rounds so the next round starts with a clean slate.
 *
 * Scope, exactly:
 *   - CLEARED: working memory, the route-closure overlay (all closures
 *     reopen), the per-color last-sighting cache, AND the self-color —
 *     so fd_set_self MUST be re-called after every fd_reset.
 *   - PRESERVED: the room-link graph and the playstyle, both detector-
 *     scoped game config rather than round state.
 */
void fd_reset(fd_detector *fd);

/*
 * Tell the detector which color the local player is. Call once at the
 * start of a round (after fd_create, and again after every fd_reset —
 * reset clears the self-color) before any fd_observe_* calls. Asserts a
 * (self-color) fact and caches the value so other APIs can defensively
 * skip self.
 */
void fd_set_self(fd_detector *fd, fd_player self);

/*
 * Set / get the policy's suspicion baseline. Default is
 * FD_PLAYSTYLE_NEUTRAL. The setting persists across fd_reset
 * (game-wide config, not round state) and biases:
 *   fd_alone_risk(...)             — stance-to-risk mapping
 *   fd_pick_vote_target(...)       — threshold for voting at all
 *   fd_alone_risk_rationale(...)   — adds "(paranoid)" / "(trusting)"
 *                                    tag to the prose
 * Stance computation and the case-file render are unaffected.
 * Setter clamps out-of-range values silently.
 */
void         fd_set_playstyle(fd_detector *fd, fd_playstyle style);
fd_playstyle fd_get_playstyle(fd_detector *fd);

/* ------------------------------------------------------------------
 * Per-tick observation ingestion
 * ------------------------------------------------------------------ */

void fd_observe_self(fd_detector *fd, int tick, fd_phase phase,
    const char *room, int x, int y, int kill_ready);

void fd_observe_player(fd_detector *fd, int tick, fd_player who,
    const char *room, int x, int y);

/* The dead player's color. The dossier auto-derives death from this. */
void fd_observe_body(fd_detector *fd, int tick, fd_player color,
    const char *room, int x, int y);

void fd_observe_task_completion(fd_detector *fd, int tick, fd_player who,
    const char *room);

/*
 * Explicit death observation. Use when the policy infers death from a
 * vote_slots delta or (for impostor self) a kill_executed event, and
 * not from a visible body. Asserts (player-dead). Idempotent.
 */
void fd_observe_death(fd_detector *fd, int tick, fd_player who);

/*
 * Ejection result from a player_ejected event. was_impostor mirrors
 * the game's role-reveal flag on ejection. When true, asserts an
 * (impostor-found) fact in addition to marking the color ejected.
 */
void fd_observe_ejection(fd_detector *fd, int tick, fd_player who,
    int was_impostor);

/*
 * Directly observed vent use: the caller witnessed `who` enter or exit
 * a vent in `room` (e.g. a sprite at a known vent coordinate). This is
 * the *confirmed* counterpart to the room-graph's *inferred* vent check
 * (fd_add_room_link) — only impostors vent, so it is near-certain
 * evidence. Promotes `who` to a THREAT stance with FD_EVIDENCE_VENT,
 * the same as an inferred vent but with a stronger, observation-based
 * rationale. Independent of the room graph: works whether or not any
 * fd_add_room_link calls were made. Observing your own color is
 * silently dropped (as with fd_observe_player).
 *
 * Stance rules need the color to be a known alive crewmate, so pair
 * this with the fd_observe_player sighting from the same observation
 * (you saw them to see them vent); a vent fact for a never-sighted
 * color sits inert.
 */
void fd_observe_vent(fd_detector *fd, int tick, fd_player who,
    const char *room);

/*
 * Round configuration: the total player count and impostor count for
 * the round. Both are common knowledge from round start in Among Them.
 * Call once per round, after fd_reset / fd_set_self and before the
 * first fd_run. n_impostors is clamped to >= 1, n_players to >= 0.
 *
 * When this is never called the detector assumes a single impostor.
 * The impostor count drives two things: the round-end demotion only
 * fires once EVERY impostor has been ejected (so a multi-impostor round
 * is not cleared by the first ejection), and the parity-derived
 * fd_game_pressure level. A re-call within a round, or a call after
 * fd_reset, cleanly replaces the prior configuration.
 */
void fd_observe_round_config(fd_detector *fd, int n_players,
    int n_impostors);

/*
 * Testimony: a claim the policy parsed out of vote-chat. The policy
 * does the NLP on vote_chat_text; the library consumes the structured
 * result. `kind` classifies it (see fd_claim_kind); `room` is the
 * claimed room for LOCATION / TASK (ignored for SELF_REPORT).
 *
 * `tick` is the tick the claim is ABOUT -- a playing-phase tick the
 * speaker references ("I was in medbay at tick T") -- NOT the tick the
 * claim was uttered. The detector checks a LOCATION/TASK claim against
 * the sighting record: a claim refuted by geometry becomes a strong
 * suspicion increment; two players vouching for the same room at
 * overlapping ticks weakly clear each other. A SELF_REPORT is a small
 * tell. Observing your own color is silently dropped.
 *
 * This is an opt-in channel: feed no claims and nothing changes.
 */
void fd_observe_claim(fd_detector *fd, int tick, fd_player who,
    fd_claim_kind kind, const char *room);

/*
 * A vote / accusation: `voter` cast a vote for, or accused, `target`.
 * Recorded into the accusation social graph. When an ejection later
 * reveals an impostor, every color's interaction with that impostor is
 * re-scored (a defender of a revealed impostor draws suspicion; an
 * accuser of one is partly cleared). Accusing a provably-cleared color
 * also draws suspicion. A vote by your own color is silently dropped.
 */
void fd_observe_vote(fd_detector *fd, int tick, fd_player voter,
    fd_player target);

/*
 * A defense: `defender` spoke up in support of `defended` in vote-chat
 * (the policy's NLP classified it). Recorded into the social graph;
 * defending a player later revealed as an impostor is a strong
 * suspicion increment. A defense by your own color is silently dropped.
 */
void fd_observe_defense(fd_detector *fd, int tick, fd_player defender,
    fd_player defended);

/* ------------------------------------------------------------------
 * Navigation graph for impossible-movement detection
 *
 * The detector flags impossible movement by comparing observed
 * elapsed ticks against shortest-path traversal costs on a caller-
 * asserted room graph. The graph is opt-in: with no edges, no vent
 * checks fire. The base topology is detector-scoped (persists across
 * fd_reset); the closure overlay is round-scoped (cleared by
 * fd_reset). Game-time unit is ticks (~25 ticks/sec).
 *
 * This is the *inferred* vent path. If the caller can witness a vent
 * use directly, fd_observe_vent feeds that stronger signal and needs
 * no graph at all.
 * ------------------------------------------------------------------ */

/*
 * Assert a bidirectional link with a traversal cost in ticks.
 * Repeat calls overwrite. A cost <= 0, or above the internal
 * per-link ceiling (1000000 ticks -- far beyond any real link), is
 * rejected silently.
 *
 * Cost model — read this; getting it wrong silently breaks vent
 * detection. A link's cost should be the MINIMUM honest time, in
 * ticks, to walk between two DIRECTLY connected rooms (a doorway
 * crossing — typically tens of ticks). Assert a SPARSE adjacency
 * graph: one link per real doorway, nothing more. Costs accumulate
 * via shortest path, so a multi-room journey is derived as the sum of
 * its hops -- you do NOT assert links between rooms that are not
 * directly connected.
 *
 * Do NOT assert an all-pairs graph (a direct link for every room
 * pair). Doing so defeats shortest-path composition: the only path
 * between two rooms becomes that single asserted edge, never a
 * realistic multi-hop sum. Combined with an overstated cost — e.g.
 * straight-line or room-centroid distance, which far exceeds an actual
 * doorway crossing — this makes ordinary movement look faster than
 * "possible" and floods the detector with false (vent-suspected)
 * facts. Verify a freshly built graph with fd_room_distance before
 * trusting it: adjacent rooms should read a few tens of ticks apart,
 * not hundreds.
 *
 * Room names must be CLIPS-symbol-safe: only [A-Za-z0-9_-]. A link
 * naming a room with other characters is rejected silently (such a
 * room could never match a sighting -- see fd_observe_player).
 *
 * The library interns (copies) accepted room names; the caller keeps
 * ownership of room_a / room_b and may free them after the call.
 * Interned names live until fd_destroy.
 *
 * Subsequent fd_observe_player calls compare (tick_now - tick_prev)
 * against the shortest path between the prior room and the new
 * room. If the cost exceeds the elapsed time, the detector asserts
 * (vent-suspected (color ...) (from-room ...) (to-room ...)
 * (delta-tick ...)). If either room is unknown or unreachable, no
 * vent check fires.
 *
 * Persists across fd_reset.
 */
void fd_add_room_link(fd_detector *fd, const char *room_a,
    const char *room_b, int cost_ticks);

/*
 * Mark a previously-asserted link as currently closed (sabotage,
 * locked door, etc.). Shortest paths route around the closure.
 * Idempotent. No-op if the pair has no asserted link.
 *
 * Closures are ROUND-scoped: fd_reset reopens every closure.
 * Closures only affect FUTURE vent checks; vent-suspected facts
 * asserted before the closure stand on their prior evidence.
 */
void fd_observe_route_closed(fd_detector *fd, const char *room_a,
    const char *room_b);

/*
 * Reopen a previously-closed link. Idempotent. No-op for unknown
 * room pairs. The base asserted cost resumes.
 */
void fd_observe_route_opened(fd_detector *fd, const char *room_a,
    const char *room_b);

/*
 * Current shortest-path cost in ticks between two rooms — the same
 * value the vent check compares observed travel against, with any
 * active route closures applied. Returns:
 *    0 if room_a and room_b name the same known room;
 *  > 0 the shortest-path tick cost;
 *   -1 if either room is unknown to the graph, the rooms are not
 *      connected (no path, or a closure severed the last one), or on
 *      bad arguments (NULL fd, NULL room name).
 * A read-only query — no rule firing, no working-memory change.
 * Intended for verifying a freshly asserted room graph at integration
 * time (see fd_add_room_link).
 */
int fd_room_distance(fd_detector *fd, const char *room_a,
    const char *room_b);

/* ------------------------------------------------------------------
 * Rule firing and queries
 * ------------------------------------------------------------------ */

/* Force a fixpoint of rule firing. Caller decides cadence. */
void fd_run(fd_detector *fd);

/*
 * Render the vote-time stance summary into caller's buf, NUL-terminated.
 * Implicitly fd_run()s first. snprintf-style return:
 *   - returns total bytes that would be written, excluding the NUL;
 *   - if return >= bufsize, buf was truncated;
 *   - returns -1 on internal error (NULL fd, NULL buf with bufsize > 0).
 */
long fd_render_vote_summary(fd_detector *fd, char *buf, size_t bufsize);

/*
 * Render a Poirot-style case file into caller's buf. One section per
 * open case (one per discovered body): the kill window, prime suspects
 * with their basis, and exonerated colors with the alibi that cleared
 * them. Falls back to a "no cases yet" line if no body has been seen.
 * Implicitly fd_run()s first. The caller owns buf; output is
 * NUL-terminated whenever bufsize > 0. snprintf-style return: total
 * bytes that would be written excluding the NUL; a value >= bufsize
 * means buf was truncated; -1 on internal error (NULL fd, or NULL buf
 * with bufsize > 0).
 */
long fd_render_case(fd_detector *fd, char *buf, size_t bufsize);

/*
 * Vote-time decision. Selects the highest-severity alive color
 * (threat > off-task > on-task > unknown), tie-broken by fd_player enum
 * order so the result is deterministic. Returns:
 *   1 if a target was selected — *out_color and *out_severity are
 *     written; the caller should cast a vote for *out_color.
 *   0 if no alive color has any actionable stance — caller should skip.
 *  -1 on error (NULL fd or NULL output pointers).
 */
int fd_pick_vote_target(fd_detector *fd, fd_player *out_color,
    fd_stance *out_severity);

/*
 * Calibrated suspicion weight for `who`: a per-mille share, 0..1000, of
 * the normalized joint-belief distribution over who is the impostor.
 * The weights of all alive non-self colors sum to 1000, so clearing one
 * color mechanically raises the others. Implicitly fd_run()s first.
 * Returns -1 for a color with no suspicion fact (self, dead, ejected,
 * or never sighted) or on a NULL fd. This is the scalar a policy
 * thresholds or ranks on; fd_rank_suspects sorts by it.
 */
int fd_get_suspicion(fd_detector *fd, fd_player who);

/*
 * The current parity-derived vote-urgency level. Implicitly fd_run()s
 * first. Defaults to FD_PRESSURE_LOW before any roster is established.
 */
fd_game_pressure_t fd_game_pressure(fd_detector *fd);

/*
 * Crew win probability w(n,m) for the live (crew, impostor) counts, in
 * per mille (0..1000). Instrumentation: a calibrated read on how close
 * the round is to a loss. Implicitly fd_run()s first. Returns -1 on a
 * NULL fd or before any roster exists.
 */
int fd_win_probability(fd_detector *fd);

/*
 * Vote-time decision over the calibrated suspicion distribution. Picks
 * the highest-weight alive color and runs an expected-value test that
 * weighs the gain of a correct ejection against the loss of a wrong
 * one, with the loss scaled by fd_game_pressure. Implicitly fd_run()s
 * first. Writes the verdict (target, suspicion, confidence, a SKIP /
 * ABSTAIN / CAST recommendation, and a prose rationale) into *out.
 * Returns 1 on success, -1 on error (NULL fd or NULL out). Unlike
 * fd_pick_vote_target (stance-tier based), this is the belief-aware
 * picker -- it is the recommended vote entry point.
 */
int fd_vote_decision(fd_detector *fd, fd_vote_decision_t *out);

/* Per-color introspection. Each implicitly fd_run()s first. */
fd_stance        fd_get_stance(fd_detector *fd, fd_player who);
fd_player_status fd_get_status(fd_detector *fd, fd_player who);

/*
 * The evidence kind behind `who`'s current stance (see fd_evidence).
 * Implicitly fd_run()s first. Returns FD_EVIDENCE_NONE for a color
 * with no stance fact (dead, ejected, or never sighted) — a real
 * answer, not an error. Pair with fd_pick_vote_target so a policy can
 * skip a target whose only evidence is FD_EVIDENCE_OFF_TASK.
 */
fd_evidence      fd_get_evidence(fd_detector *fd, fd_player who);

/*
 * Render the one-line rationale behind `who`'s current stance into the
 * caller-owned buf. Implicitly fd_run()s first. Output is
 * NUL-terminated whenever bufsize > 0. snprintf-style return: total
 * bytes that would be written excluding the NUL; >= bufsize means
 * truncated; -1 on internal error (NULL fd, or NULL buf with
 * bufsize > 0). An empty string is written if `who` has no stance.
 */
long             fd_get_rationale(fd_detector *fd, fd_player who, char *buf,
                   size_t bufsize);

/*
 * Safety query: how dangerous is it to be alone in a room with `who`?
 * Caller decides what "alone" means in their perception; this answers
 * only the per-color part of the question. Implicitly fd_run()s first.
 * See docs/model.md for the input → output mapping.
 */
fd_risk fd_alone_risk(fd_detector *fd, fd_player who);

/*
 * Composes a single-line prose justification matching the level
 * fd_alone_risk would return for the same color. Example:
 *   "alone with pink: HIGH risk - near body in storage at tick 178
 *    without reporting"
 * Implicitly fd_run()s first. The caller owns buf; output is
 * NUL-terminated whenever bufsize > 0. snprintf-style return: total
 * bytes that would be written excluding the NUL; >= bufsize means
 * truncated; -1 on internal error (NULL fd, or NULL buf with
 * bufsize > 0).
 */
long fd_alone_risk_rationale(fd_detector *fd, fd_player who,
    char *buf, size_t bufsize);

/*
 * Register a display name for a player id. The name becomes the CLIPS
 * symbol identifying the player in every fact and the word rationales
 * use, so it must be symbol-safe: [A-Za-z0-9_-], non-empty, at most 63
 * bytes, and unique across ids (two players sharing a symbol would
 * silently alias in working memory). Returns 0 on success, -1 on a
 * rejected name (also counted in fd_ingest_stats.validation_drops).
 *
 * Names are detector-scoped game config, like the room graph: they
 * persist across fd_reset and default to "p0".."p31" when never set.
 * Register names BEFORE the first observation of a round — renaming an
 * id mid-round orphans the facts already asserted under the old symbol.
 */
int fd_set_player_name(fd_detector *fd, fd_player id, const char *name);

/*
 * The display name for an id: the registered name, or the "p<id>"
 * default. NULL for an out-of-range id or NULL fd. The returned string
 * lives until the id is re-registered or the detector is destroyed.
 */
const char *fd_player_name(fd_detector *fd, fd_player id);

/* ------------------------------------------------------------------
 * Mid-round situational queries
 *
 * These getters surface the dossier's current view without forcing
 * the caller to maintain their own observation log. Each implicitly
 * runs the rule engine to fixpoint if there are pending observations
 * (cheap when the agenda is already drained). All are O(n_colors).
 * ------------------------------------------------------------------ */

/*
 * Write the most recent (room, tick) for `color` into the caller's
 * buffers. Returns 1 if the color has been sighted at least once
 * this round, 0 if never sighted, -1 on bad args. The room name is
 * NUL-terminated even on truncation.
 */
int fd_last_seen(fd_detector *fd, fd_player color,
    char *out_room, size_t bufsize, int *out_tick);

/*
 * Fill out_colors with every alive non-self color whose last-known
 * room is `room`. The caller supplies out_colors with space for at
 * least `max` elements; the library writes at most `max`. Returns the
 * number of colors written (0..max), or -1 on bad arguments (NULL fd,
 * NULL out_colors, max <= 0). A NULL or empty `room` is not an error
 * -- it returns 0 (no room named, no occupants). Order is ascending
 * player id (deterministic). Colors never sighted, dead, or ejected
 * are excluded — this is "who is in this room right now, as best the
 * detector can tell". Useful for "should I follow them?" decisions.
 */
int fd_room_occupants(fd_detector *fd, const char *room,
    fd_player *out_colors, int max);

/*
 * Top-N suspects ranked by calibrated belief weight (see
 * fd_get_suspicion), highest first. The caller supplies out_colors and
 * out_stances with space for at least `max` elements each; the library
 * fills them in parallel, at most `max`, out_stances carrying each
 * ranked color's fd_get_stance. Returns the number filled (0..max), or
 * -1 on bad arguments (NULL fd, NULL out_colors, NULL out_stances,
 * max <= 0). Ties break by ascending player id.
 *
 * Note this ranks by the probabilistic distribution, NOT by stance
 * tier -- so its head may differ from fd_pick_vote_target's pick (which
 * stays stance-tier + playstyle based). fd_vote_decision is the
 * belief-aware vote picker built on the same distribution.
 */
int fd_rank_suspects(fd_detector *fd, fd_player *out_colors,
    fd_stance *out_stances, int max);

/* Round-level aggregates. Overwrites *out. impostor_found is 1 if
 * an ejection this round revealed the impostor (gates threat
 * stance). open_cases counts (case) facts; vent_suspicions counts
 * (vent-suspected) facts. */
typedef struct {
	int alive;             /* status == alive */
	int dead;              /* status == dead */
	int ejected;           /* status == ejected */
	int never_sighted;     /* configured roster members with no dossier;
	                          0 when fd_observe_round_config was never
	                          called (the universe is then unknown) */
	int open_cases;        /* count of case facts */
	int vent_suspicions;   /* count of vent-suspected facts */
	int impostor_found;    /* 0 or 1 */
} fd_round_stats;

void fd_round_stats_get(fd_detector *fd, fd_round_stats *out);

/* ------------------------------------------------------------------
 * Telemetry & observability
 *
 * Stable introspection surface for developing and tuning a policy:
 * WHY a suspicion weight is what it is (fd_explain_suspicion), HOW hard
 * the rule engine worked producing it (fd_run_stats_get), and WHAT is
 * in working memory right now (fd_dump_state / fd_trace_*). Unlike the
 * diagnostic block further down, these are committed library surface a
 * policy may depend on.
 * ------------------------------------------------------------------ */

/* Upper bound on the per-color evidence terms fd_explain_suspicion
 * reports — comfortably above the count any one color accumulates (one
 * (logodds-term) fact per (source, key) pair). A color with more terms
 * than this has its breakdown truncated; n_terms then caps here while
 * logodds_total still reflects every term. */
#define FD_EXPLAIN_MAX_TERMS 24

/*
 * The decomposition of one color's calibrated suspicion weight into the
 * evidence that produced it — the answer to "why is this number what it
 * is". fd_explain_suspicion fills *out.
 *   color         - the queried color, echoed back.
 *   n_terms       - contributing terms in terms[], 0..FD_EXPLAIN_MAX_TERMS.
 *   terms[]       - each evidence contribution: the `source` channel
 *                   (e.g. "vent-observed", "near-body", "on-task"), its
 *                   `key` discriminator (may be ""), and the signed
 *                   `amount` in milli-nats (1000 = 1.0 nat; positive is
 *                   more suspicious). Untruncated, the amounts sum to
 *                   logodds_total.
 *   logodds_total - the summed log-odds accumulator (== suspicion.logodds).
 *   likelihood    - logistic(logodds_total) in per mille (0..1000), the
 *                   belief BEFORE normalization across the alive pool.
 *   weight        - the normalized per-mille weight (== fd_get_suspicion).
 */
typedef struct {
	fd_player player;
	int       n_terms;
	struct {
		char      source[32];
		char      key[64];
		long long amount;
	} terms[FD_EXPLAIN_MAX_TERMS];
	long long logodds_total;
	int       likelihood;
	int       weight;
} fd_suspicion_explain;

/*
 * Decompose `who`'s suspicion weight into its evidence terms. Implicitly
 * fd_run()s first. Overwrites *out. Returns:
 *    1  *out filled with the breakdown.
 *    0  `who` carries no suspicion fact (self, dead, ejected, or never
 *       sighted) — *out is zeroed (n_terms 0).
 *   -1  bad arguments (NULL fd, NULL out, or an out-of-range color).
 */
int fd_explain_suspicion(fd_detector *fd, fd_player who,
    fd_suspicion_explain *out);

/*
 * Rule-engine instrumentation accumulated since the last fd_reset.
 *   last_rules_fired  - rule activations fired by the most recent fd_run.
 *   total_rules_fired - cumulative across the round.
 *   run_count         - fd_run calls so far (explicit, plus the implicit
 *                       one every query makes).
 *   last_run_ms       - wall-clock of the most recent fd_run, in ms.
 *   max_run_ms        - the slowest fd_run this round, in ms.
 *   fact_count        - live facts in working memory, read at call time.
 */
typedef struct {
	long long last_rules_fired;
	long long total_rules_fired;
	long long run_count;
	double    last_run_ms;
	double    max_run_ms;
	int       fact_count;
} fd_run_stats;

/*
 * Read the run-engine instrumentation into *out (overwrites it). Unlike
 * the query family this does NOT implicitly fd_run() — it reports runs
 * already performed, so calling it never disturbs last_rules_fired.
 * fact_count is the one live reading. NULL-safe in both arguments.
 */
void fd_run_stats_get(fd_detector *fd, fd_run_stats *out);

/*
 * Observation-ingest accounting accumulated since fd_create. These
 * counters are detector-scoped, so fd_reset leaves them intact.
 *   total_calls        - fd_set_self, fd_observe_*, fd_add_room_link, and
 *                        fd_observe_route_* calls seen by a live detector.
 *   accepted_calls     - calls that mutated detector state or working
 *                        memory (including overwriting a prior fact).
 *   validation_drops   - calls rejected for bad arguments or invalid
 *                        names/ranges before their primary mutation landed.
 *   self_filtered_drops - observations of the local player's own color
 *                        dropped by the library's self-filter.
 *   assert_drops       - CLIPS fact assertions rejected or overflowed by
 *                        formatting. A call may be accepted and still bump
 *                        this if a secondary internal assertion failed.
 */
typedef struct {
	long long total_calls;
	long long accepted_calls;
	long long validation_drops;
	long long self_filtered_drops;
	long long assert_drops;
} fd_ingest_stats;

/* Read the detector-scoped ingest counters into *out. NULL-safe in both
 * arguments. Unlike fd_run_stats_get, these counters are NOT cleared by
 * fd_reset. */
void fd_ingest_stats_get(fd_detector *fd, fd_ingest_stats *out);

/*
 * Write a structured, one-fact-per-line snapshot of working memory —
 * dossier, evidence, social, probabilistic, and verdict facts — to the
 * caller-supplied stream. Implicitly fd_run()s first. The line format is
 * the library's own (not the CLIPS REPL's) and is stable surface;
 * intended for a policy author's per-round development log. NULL-safe
 * (NULL fd or NULL stream is a no-op).
 *
 * FILE*-based; for C consumers. FFI callers (Python ctypes, etc.)
 * cannot construct a C FILE* portably — use fd_dump_state_buf or
 * fd_dump_state_path instead.
 */
void fd_dump_state(fd_detector *fd, FILE *stream);

/*
 * Same snapshot, snprintf-style into a caller buffer: writes at most
 * bufsize-1 bytes plus a terminating NUL and returns the full length
 * the snapshot wants (excluding the NUL), so ret >= bufsize means
 * truncation — retry with a bigger buffer. Returns -1 on NULL fd.
 * A NULL buf with bufsize 0 is a pure size query.
 */
long fd_dump_state_buf(fd_detector *fd, char *buf, size_t bufsize);

/*
 * Same snapshot written to a freshly created/truncated file at `path`.
 * Returns 0 on success, -1 on error (NULL arguments, open or write
 * failure).
 */
int fd_dump_state_path(fd_detector *fd, const char *path);

/*
 * Mirror CLIPS rule / fact / activation activity to `stream` for every
 * subsequent fd_run, until fd_trace_end. A development trace: capture a
 * round to a file and diff it between rule-set versions. Opt-in and off
 * by default; a NULL stream is a no-op. The caller owns the stream and
 * must call fd_trace_end before closing it. fd_reset does not stop an
 * active trace. While a trace is active, CLIPS watch output is routed to
 * `stream` instead of being discarded.
 *
 * fd_trace_begin is FILE*-based, for C consumers. fd_trace_begin_path
 * is the FFI-safe variant: the library opens (creates/truncates) the
 * file itself and closes it at fd_trace_end / fd_destroy; returns 0 on
 * success, -1 on error. Beginning a new trace ends any active one.
 */
void fd_trace_begin(fd_detector *fd, FILE *stream);
int  fd_trace_begin_path(fd_detector *fd, const char *path);
void fd_trace_end(fd_detector *fd);

/* ------------------------------------------------------------------
 * Diagnostic / instrumentation API
 *
 * These are intended for the perf harness only. They are NOT part of
 * the stable library surface, MAY change, and should NOT be wired
 * into game-policy code. They thin-wrap the CLIPS `(profile)`,
 * `(profile-info)`, and `(matches)` REPL commands so the harness can
 * report per-rule timing and partial-match counts without taking a
 * dependency on internal CLIPS headers.
 * ------------------------------------------------------------------ */

/* Enable per-construct profiling. After this call, every rule firing
 * accumulates call count + self/total time. No-op if already on. */
void fd_profile_begin(fd_detector *fd);

/* Disable profiling. Accumulated profile data is preserved (and can
 * still be dumped) until fd_reset or fd_destroy. */
void fd_profile_end(fd_detector *fd);

/* Print the CLIPS (profile-info) table to the process's stdout
 * (via the CLIPS default STDOUT router). One row per construct that
 * has been profiled, with self/total time and percentage. */
void fd_profile_dump(fd_detector *fd);

/* Print the CLIPS (matches <rule>) report for one rule. Shows the
 * rule's alpha/beta partial-match counts and instantiations — useful
 * for spotting Rete cross-product blowups in a specific rule.
 * Lifetime of rule_name need only span the call. */
void fd_profile_matches(fd_detector *fd, const char *rule_name);

#ifdef __cplusplus
}
#endif

#endif /* FAKEDETECTOR_H */
