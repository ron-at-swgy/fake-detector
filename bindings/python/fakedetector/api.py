"""Pythonic wrapper over the raw libfakedetector surface.

One Detector instance wraps one fd_detector handle. Strings cross the
boundary as UTF-8; snprintf-style calls are retried with a grown buffer
when truncated; enum-returning queries come back as IntEnums.
"""

from __future__ import annotations

import ctypes
from enum import IntEnum
from typing import NamedTuple, Optional

from ._native import (
    LIB,
    EXPLAIN_MAX_TERMS,
    MAX_PLAYERS,
    IngestStats,
    RoundStats,
    RunStats,
    SuspicionExplain,
    VoteDecision,
)

__all__ = [
    "Detector", "Phase", "Stance", "ClaimKind", "Evidence", "PlayerStatus",
    "Risk", "Playstyle", "GamePressure", "VoteAction", "VoteResult",
    "ExplainedSuspicion", "CREWRIFT_COLORS", "MAX_PLAYERS",
    "version", "abi_version",
]

CREWRIFT_COLORS = (
    "red", "blue", "green", "pink",
    "orange", "yellow", "purple", "cyan",
    "lime", "brown", "beige", "navy",
    "teal", "rose", "maroon", "gray",
)


class Phase(IntEnum):
    PREGAME = 0
    PLAYING = 1
    VOTING = 2
    RESULTS = 3
    GAMEOVER = 4


class Stance(IntEnum):
    UNKNOWN = 0
    ON_TASK = 1
    OFF_TASK = 2
    THREAT = 3


class ClaimKind(IntEnum):
    LOCATION = 0
    TASK = 1
    SELF_REPORT = 2


class Evidence(IntEnum):
    NONE = 0
    ACCUSATION = 1
    VENT = 2
    NEAR_BODY = 3
    OFF_TASK = 4
    ON_TASK = 5


class PlayerStatus(IntEnum):
    UNKNOWN = 0
    ALIVE = 1
    DEAD = 2
    EJECTED = 3


class Risk(IntEnum):
    UNKNOWN = 0
    NONE = 1
    LOW = 2
    MEDIUM = 3
    HIGH = 4


class Playstyle(IntEnum):
    TRUSTING = 0
    NEUTRAL = 1
    PARANOID = 2


class GamePressure(IntEnum):
    LOW = 0
    MEDIUM = 1
    HIGH = 2
    CRITICAL = 3


class VoteAction(IntEnum):
    SKIP = 0
    ABSTAIN = 1
    CAST = 2


class VoteResult(NamedTuple):
    target: int
    suspicion: int
    confidence: int
    recommendation: VoteAction
    rationale: str


class ExplainedSuspicion(NamedTuple):
    player: int
    terms: list  # [(source, key, amount), ...]
    logodds_total: int
    likelihood: int
    weight: int


def version() -> str:
    return LIB.fd_version().decode()


def abi_version() -> int:
    return LIB.fd_abi_version()


def _enc(s: Optional[str]) -> Optional[bytes]:
    return None if s is None else s.encode()


class Detector:
    """One detector instance. Use as a context manager or call close().

    clp_dir=None loads the rule set embedded in the library (the
    production default); a directory path loads .clp files from disk.
    """

    def __init__(self, clp_dir: Optional[str] = None):
        self._h = LIB.fd_create(_enc(clp_dir))
        if not self._h:
            raise RuntimeError(
                "fd_create failed (invalid rules%s)"
                % ("" if clp_dir is None else f" in {clp_dir!r}"))

    # -- lifecycle ---------------------------------------------------

    def close(self) -> None:
        if self._h:
            LIB.fd_destroy(self._h)
            self._h = None

    def __enter__(self) -> "Detector":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def reset(self) -> None:
        LIB.fd_reset(self._h)

    # -- identity / config -------------------------------------------

    def set_self(self, player: int) -> None:
        LIB.fd_set_self(self._h, player)

    def set_player_name(self, player: int, name: str) -> None:
        if LIB.fd_set_player_name(self._h, player, _enc(name)) != 0:
            raise ValueError(
                f"rejected player name {name!r} for id {player} "
                "(symbol-unsafe, duplicate, or id out of range)")

    def player_name(self, player: int) -> str:
        name = LIB.fd_player_name(self._h, player)
        if name is None:
            raise ValueError(f"player id {player} out of range")
        return name.decode()

    def register_crewrift(self) -> None:
        """Name ids 0..15 with the CrewRift color roster (wire order)."""
        for i, color in enumerate(CREWRIFT_COLORS):
            self.set_player_name(i, color)

    def set_playstyle(self, style: Playstyle) -> None:
        LIB.fd_set_playstyle(self._h, style)

    def playstyle(self) -> Playstyle:
        return Playstyle(LIB.fd_get_playstyle(self._h))

    def observe_round_config(self, n_players: int, n_impostors: int) -> None:
        LIB.fd_observe_round_config(self._h, n_players, n_impostors)

    # -- room graph ---------------------------------------------------

    def add_room_link(self, room_a: str, room_b: str, cost_ticks: int) -> None:
        LIB.fd_add_room_link(self._h, _enc(room_a), _enc(room_b), cost_ticks)

    def observe_route_closed(self, room_a: str, room_b: str) -> None:
        LIB.fd_observe_route_closed(self._h, _enc(room_a), _enc(room_b))

    def observe_route_opened(self, room_a: str, room_b: str) -> None:
        LIB.fd_observe_route_opened(self._h, _enc(room_a), _enc(room_b))

    def room_distance(self, room_a: str, room_b: str) -> int:
        return LIB.fd_room_distance(self._h, _enc(room_a), _enc(room_b))

    # -- observations -------------------------------------------------

    def observe_self(self, tick: int, phase: Phase, room: str,
                     x: int = 0, y: int = 0, kill_ready: bool = False) -> None:
        LIB.fd_observe_self(self._h, tick, phase, _enc(room), x, y,
                            1 if kill_ready else 0)

    def observe_player(self, tick: int, who: int, room: str,
                       x: int = 0, y: int = 0) -> None:
        LIB.fd_observe_player(self._h, tick, who, _enc(room), x, y)

    def observe_body(self, tick: int, victim: int, room: str,
                     x: int = 0, y: int = 0) -> None:
        LIB.fd_observe_body(self._h, tick, victim, _enc(room), x, y)

    def observe_task_completion(self, tick: int, who: int, room: str) -> None:
        LIB.fd_observe_task_completion(self._h, tick, who, _enc(room))

    def observe_death(self, tick: int, who: int) -> None:
        LIB.fd_observe_death(self._h, tick, who)

    def observe_ejection(self, tick: int, who: int,
                         was_impostor: bool) -> None:
        LIB.fd_observe_ejection(self._h, tick, who, 1 if was_impostor else 0)

    def observe_vent(self, tick: int, who: int, room: str) -> None:
        LIB.fd_observe_vent(self._h, tick, who, _enc(room))

    def observe_claim(self, tick: int, who: int, kind: ClaimKind,
                      room: str) -> None:
        LIB.fd_observe_claim(self._h, tick, who, kind, _enc(room))

    def observe_vote(self, tick: int, voter: int, target: int) -> None:
        LIB.fd_observe_vote(self._h, tick, voter, target)

    def observe_defense(self, tick: int, defender: int,
                        defended: int) -> None:
        LIB.fd_observe_defense(self._h, tick, defender, defended)

    # -- engine -------------------------------------------------------

    def run(self) -> None:
        LIB.fd_run(self._h)

    # -- queries ------------------------------------------------------

    def stance(self, who: int) -> Stance:
        return Stance(LIB.fd_get_stance(self._h, who))

    def status(self, who: int) -> PlayerStatus:
        return PlayerStatus(LIB.fd_get_status(self._h, who))

    def evidence(self, who: int) -> Evidence:
        return Evidence(LIB.fd_get_evidence(self._h, who))

    def suspicion(self, who: int) -> int:
        """Per-mille belief weight 0..1000, or -1 without a fact."""
        return LIB.fd_get_suspicion(self._h, who)

    def alone_risk(self, who: int) -> Risk:
        return Risk(LIB.fd_alone_risk(self._h, who))

    def game_pressure(self) -> GamePressure:
        return GamePressure(LIB.fd_game_pressure(self._h))

    def win_probability(self) -> int:
        """Crew win probability in per mille."""
        return LIB.fd_win_probability(self._h)

    def vote_decision(self) -> VoteResult:
        out = VoteDecision()
        if LIB.fd_vote_decision(self._h, ctypes.byref(out)) < 0:
            raise RuntimeError("fd_vote_decision failed")
        return VoteResult(
            target=out.target,
            suspicion=out.suspicion,
            confidence=out.confidence,
            recommendation=VoteAction(out.recommendation),
            rationale=out.rationale.decode(errors="replace"),
        )

    def pick_vote_target(self) -> Optional[tuple]:
        """Legacy stance-tier picker: (player, Stance), or None to abstain."""
        who = ctypes.c_int(0)
        sev = ctypes.c_int(0)
        rc = LIB.fd_pick_vote_target(
            self._h, ctypes.byref(who), ctypes.byref(sev))
        if rc < 0:
            raise RuntimeError("fd_pick_vote_target failed")
        return (who.value, Stance(sev.value)) if rc == 1 else None

    def last_seen(self, who: int) -> Optional[tuple]:
        """(room, tick) of the last sighting, or None if never sighted."""
        room = ctypes.create_string_buffer(128)
        tick = ctypes.c_int(0)
        rc = LIB.fd_last_seen(self._h, who, room, len(room),
                              ctypes.byref(tick))
        if rc < 0:
            raise RuntimeError("fd_last_seen failed")
        return (room.value.decode(), tick.value) if rc == 1 else None

    def room_occupants(self, room: str) -> list:
        out = (ctypes.c_int * MAX_PLAYERS)()
        n = LIB.fd_room_occupants(self._h, _enc(room), out, MAX_PLAYERS)
        if n < 0:
            raise RuntimeError("fd_room_occupants failed")
        return list(out[:n])

    def rank_suspects(self, max_n: int = MAX_PLAYERS) -> list:
        """[(player, Stance)] ranked by belief weight, highest first."""
        players = (ctypes.c_int * max_n)()
        stances = (ctypes.c_int * max_n)()
        n = LIB.fd_rank_suspects(self._h, players, stances, max_n)
        if n < 0:
            raise RuntimeError("fd_rank_suspects failed")
        return [(players[i], Stance(stances[i])) for i in range(n)]

    def explain_suspicion(self, who: int) -> Optional[ExplainedSuspicion]:
        out = SuspicionExplain()
        rc = LIB.fd_explain_suspicion(self._h, who, ctypes.byref(out))
        if rc < 0:
            raise ValueError(f"player id {who} out of range")
        if rc == 0:
            return None
        terms = [
            (out.terms[i].source.decode(), out.terms[i].key.decode(),
             out.terms[i].amount)
            for i in range(min(out.n_terms, EXPLAIN_MAX_TERMS))
        ]
        return ExplainedSuspicion(
            player=out.player, terms=terms,
            logodds_total=out.logodds_total,
            likelihood=out.likelihood, weight=out.weight)

    def round_stats(self) -> RoundStats:
        out = RoundStats()
        LIB.fd_round_stats_get(self._h, ctypes.byref(out))
        return out

    def run_stats(self) -> RunStats:
        out = RunStats()
        LIB.fd_run_stats_get(self._h, ctypes.byref(out))
        return out

    def ingest_stats(self) -> IngestStats:
        out = IngestStats()
        LIB.fd_ingest_stats_get(self._h, ctypes.byref(out))
        return out

    # -- prose / telemetry --------------------------------------------

    def _read_snprintf(self, fn, *args) -> str:
        size = 4096
        for _ in range(2):
            buf = ctypes.create_string_buffer(size)
            need = fn(self._h, *args, buf, len(buf))
            if need < 0:
                raise RuntimeError("render call failed")
            if need < size:
                return buf.value.decode(errors="replace")
            size = need + 1
        raise RuntimeError("render buffer kept growing")

    def render_vote_summary(self) -> str:
        return self._read_snprintf(LIB.fd_render_vote_summary)

    def render_case(self) -> str:
        return self._read_snprintf(LIB.fd_render_case)

    def rationale(self, who: int) -> str:
        return self._read_snprintf(LIB.fd_get_rationale, who)

    def alone_risk_rationale(self, who: int) -> str:
        return self._read_snprintf(LIB.fd_alone_risk_rationale, who)

    def dump_state(self) -> str:
        return self._read_snprintf(LIB.fd_dump_state_buf)

    def dump_state_path(self, path: str) -> None:
        if LIB.fd_dump_state_path(self._h, _enc(path)) != 0:
            raise OSError(f"fd_dump_state_path({path!r}) failed")

    def trace_begin(self, path: str) -> None:
        if LIB.fd_trace_begin_path(self._h, _enc(path)) != 0:
            raise OSError(f"fd_trace_begin_path({path!r}) failed")

    def trace_end(self) -> None:
        LIB.fd_trace_end(self._h)
