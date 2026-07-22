"""fakedetector — Python binding for libfakedetector.

A detective brain for social-deception games: feed observations, read
back calibrated suspicion, stances, case files, and vote decisions.
See the repository README and docs/policy-guide.md for the model.
"""

from .api import (
    CREWRIFT_COLORS,
    MAX_PLAYERS,
    ClaimKind,
    Detector,
    Evidence,
    ExplainedSuspicion,
    GamePressure,
    Phase,
    PlayerStatus,
    Playstyle,
    Risk,
    Stance,
    VoteAction,
    VoteResult,
    abi_version,
    version,
)

__all__ = [
    "CREWRIFT_COLORS", "MAX_PLAYERS", "ClaimKind", "Detector", "Evidence",
    "ExplainedSuspicion", "GamePressure", "Phase", "PlayerStatus",
    "Playstyle", "Risk", "Stance", "VoteAction", "VoteResult",
    "abi_version", "version",
]
