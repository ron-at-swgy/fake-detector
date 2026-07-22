"""Raw ctypes surface for libfakedetector.

Loads the shared library, asserts the ABI handshake, and declares
argtypes/restypes plus the Structure mirrors of the public out-structs.
Layouts mirror src/fakedetector.h and are frozen per docs/abi.md; the
ABI_VERSION constant below must match FD_ABI_VERSION in that header.

Library resolution order:
  1. the FD_LIBRARY_PATH environment variable (a full path to the .so),
  2. the system linker via ctypes.util.find_library("fakedetector"),
  3. a bundled copy under fakedetector/_lib/ (package data, if present).
"""

from __future__ import annotations

import ctypes
import ctypes.util
import os
from pathlib import Path

ABI_VERSION = 1
MAX_PLAYERS = 32
EXPLAIN_MAX_TERMS = 24

_BUNDLED_NAMES = (
    "libfakedetector.so.1",
    "libfakedetector.so",
    "libfakedetector.1.dylib",
    "libfakedetector.dylib",
)


def _library_path() -> str:
    env = os.environ.get("FD_LIBRARY_PATH")
    if env:
        return env
    found = ctypes.util.find_library("fakedetector")
    if found:
        return found
    bundled = Path(__file__).parent / "_lib"
    for name in _BUNDLED_NAMES:
        candidate = bundled / name
        if candidate.exists():
            return str(candidate)
    raise OSError(
        "libfakedetector not found: set FD_LIBRARY_PATH, install the "
        "library on the linker path (make install), or bundle it under "
        "fakedetector/_lib/"
    )


class VoteDecision(ctypes.Structure):
    _fields_ = [
        ("target", ctypes.c_int),
        ("suspicion", ctypes.c_int),
        ("confidence", ctypes.c_int),
        ("recommendation", ctypes.c_int),
        ("rationale", ctypes.c_char * 256),
    ]


class RoundStats(ctypes.Structure):
    _fields_ = [
        ("alive", ctypes.c_int),
        ("dead", ctypes.c_int),
        ("ejected", ctypes.c_int),
        ("never_sighted", ctypes.c_int),
        ("open_cases", ctypes.c_int),
        ("vent_suspicions", ctypes.c_int),
        ("impostor_found", ctypes.c_int),
    ]


class ExplainTerm(ctypes.Structure):
    _fields_ = [
        ("source", ctypes.c_char * 32),
        ("key", ctypes.c_char * 64),
        ("amount", ctypes.c_longlong),
    ]


class SuspicionExplain(ctypes.Structure):
    _fields_ = [
        ("player", ctypes.c_int),
        ("n_terms", ctypes.c_int),
        ("terms", ExplainTerm * EXPLAIN_MAX_TERMS),
        ("logodds_total", ctypes.c_longlong),
        ("likelihood", ctypes.c_int),
        ("weight", ctypes.c_int),
    ]


class RunStats(ctypes.Structure):
    _fields_ = [
        ("last_rules_fired", ctypes.c_longlong),
        ("total_rules_fired", ctypes.c_longlong),
        ("run_count", ctypes.c_longlong),
        ("last_run_ms", ctypes.c_double),
        ("max_run_ms", ctypes.c_double),
        ("fact_count", ctypes.c_int),
    ]


class IngestStats(ctypes.Structure):
    _fields_ = [
        ("total_calls", ctypes.c_longlong),
        ("accepted_calls", ctypes.c_longlong),
        ("validation_drops", ctypes.c_longlong),
        ("self_filtered_drops", ctypes.c_longlong),
        ("assert_drops", ctypes.c_longlong),
    ]


def _declare(lib: ctypes.CDLL) -> None:
    h = ctypes.c_void_p
    i = ctypes.c_int
    s = ctypes.c_char_p
    z = ctypes.c_size_t
    buf = ctypes.c_char_p
    void = None

    sigs = {
        "fd_version": ([], ctypes.c_char_p),
        "fd_abi_version": ([], i),
        "fd_create": ([s], h),
        "fd_destroy": ([h], void),
        "fd_reset": ([h], void),
        "fd_set_self": ([h, i], void),
        "fd_set_playstyle": ([h, i], void),
        "fd_get_playstyle": ([h], i),
        "fd_set_player_name": ([h, i, s], i),
        "fd_player_name": ([h, i], ctypes.c_char_p),
        "fd_observe_self": ([h, i, i, s, i, i, i], void),
        "fd_observe_player": ([h, i, i, s, i, i], void),
        "fd_observe_body": ([h, i, i, s, i, i], void),
        "fd_observe_task_completion": ([h, i, i, s], void),
        "fd_observe_death": ([h, i, i], void),
        "fd_observe_ejection": ([h, i, i, i], void),
        "fd_observe_vent": ([h, i, i, s], void),
        "fd_observe_round_config": ([h, i, i], void),
        "fd_observe_claim": ([h, i, i, i, s], void),
        "fd_observe_vote": ([h, i, i, i], void),
        "fd_observe_defense": ([h, i, i, i], void),
        "fd_add_room_link": ([h, s, s, i], void),
        "fd_observe_route_closed": ([h, s, s], void),
        "fd_observe_route_opened": ([h, s, s], void),
        "fd_room_distance": ([h, s, s], i),
        "fd_run": ([h], void),
        "fd_render_vote_summary": ([h, buf, z], ctypes.c_long),
        "fd_render_case": ([h, buf, z], ctypes.c_long),
        "fd_pick_vote_target": (
            [h, ctypes.POINTER(i), ctypes.POINTER(i)], i),
        "fd_get_suspicion": ([h, i], i),
        "fd_game_pressure": ([h], i),
        "fd_win_probability": ([h], i),
        "fd_vote_decision": ([h, ctypes.POINTER(VoteDecision)], i),
        "fd_get_stance": ([h, i], i),
        "fd_get_status": ([h, i], i),
        "fd_get_evidence": ([h, i], i),
        "fd_get_rationale": ([h, i, buf, z], ctypes.c_long),
        "fd_alone_risk": ([h, i], i),
        "fd_alone_risk_rationale": ([h, i, buf, z], ctypes.c_long),
        "fd_last_seen": ([h, i, buf, z, ctypes.POINTER(i)], i),
        "fd_room_occupants": ([h, s, ctypes.POINTER(i), i], i),
        "fd_rank_suspects": (
            [h, ctypes.POINTER(i), ctypes.POINTER(i), i], i),
        "fd_round_stats_get": ([h, ctypes.POINTER(RoundStats)], void),
        "fd_explain_suspicion": (
            [h, i, ctypes.POINTER(SuspicionExplain)], i),
        "fd_run_stats_get": ([h, ctypes.POINTER(RunStats)], void),
        "fd_ingest_stats_get": ([h, ctypes.POINTER(IngestStats)], void),
        "fd_dump_state_buf": ([h, buf, z], ctypes.c_long),
        "fd_dump_state_path": ([h, s], i),
        "fd_trace_begin_path": ([h, s], i),
        "fd_trace_end": ([h], void),
    }
    for name, (argtypes, restype) in sigs.items():
        fn = getattr(lib, name)
        fn.argtypes = argtypes
        fn.restype = restype


def load() -> ctypes.CDLL:
    lib = ctypes.CDLL(_library_path())
    lib.fd_abi_version.restype = ctypes.c_int
    lib.fd_abi_version.argtypes = []
    abi = lib.fd_abi_version()
    if abi != ABI_VERSION:
        raise ImportError(
            f"libfakedetector ABI mismatch: library reports {abi}, "
            f"binding expects {ABI_VERSION} — rebuild/reinstall the "
            "matching pair (see docs/abi.md)"
        )
    _declare(lib)
    return lib


LIB = load()
