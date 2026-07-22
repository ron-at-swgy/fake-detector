"""Binding contract test: replays the synthetic round the C contract
tests use and asserts the same behavior through Python.

Needs the shared library findable (FD_LIBRARY_PATH, or `make shared`
then FD_LIBRARY_PATH=../../build/libfakedetector.so.1). Runs under
plain `python3 -m unittest` — no pytest dependency.
"""

import unittest

from fakedetector import (
    Detector, Evidence, GamePressure, Phase, PlayerStatus, Stance,
    VoteAction, abi_version, version,
)

RED, YELLOW, PINK, BLUE, GREEN = 0, 2, 4, 6, 13


def feed_weighted_round(fd: Detector) -> None:
    """Mirror of the C suspicion-contract round (test_suspicion_contract.c)."""
    fd.add_room_link("cafeteria", "admin", 50)
    fd.add_room_link("cafeteria", "medbay", 40)
    fd.add_room_link("medbay", "storage", 50)
    fd.add_room_link("medbay", "admin", 100)

    fd.set_self(RED)
    fd.observe_round_config(5, 1)
    fd.observe_self(0, Phase.PREGAME, "cafeteria")
    fd.observe_self(10, Phase.PLAYING, "cafeteria")

    for who in (BLUE, PINK, YELLOW, GREEN):
        fd.observe_player(10, who, "cafeteria")

    fd.observe_player(40, BLUE, "cafeteria")
    fd.observe_task_completion(40, BLUE, "cafeteria")
    fd.observe_player(70, BLUE, "admin")
    fd.observe_task_completion(70, BLUE, "admin")

    fd.observe_player(80, YELLOW, "medbay")
    fd.observe_player(100, YELLOW, "admin")

    fd.observe_player(90, GREEN, "storage")
    fd.observe_player(110, PINK, "storage")
    fd.observe_body(120, GREEN, "storage")
    fd.run()


class BindingContract(unittest.TestCase):
    def test_version_handshake(self):
        self.assertEqual(abi_version(), 1)
        self.assertRegex(version(), r"^\d+\.\d+\.\d+$")

    def test_weighted_round_matches_c_contract(self):
        with Detector() as fd:
            fd.register_crewrift()
            feed_weighted_round(fd)

            # Suspicion is a normalized per-mille distribution.
            total = sum(
                w for w in (fd.suspicion(c) for c in range(32)) if w >= 0)
            self.assertEqual(total, 1000)

            # Verdicts that follow from the fed observations.
            self.assertEqual(fd.status(GREEN), PlayerStatus.DEAD)
            self.assertEqual(fd.stance(PINK), Stance.THREAT)
            self.assertEqual(fd.evidence(PINK), Evidence.NEAR_BODY)

            # BLUE (cafeteria@40 -> admin@70 vs a 50-tick edge) and
            # YELLOW (medbay@80 -> admin@100 vs a 100-tick edge) both
            # moved faster than the cheapest legal walk: inferred vents.
            self.assertEqual(fd.stance(BLUE), Stance.THREAT)
            self.assertEqual(fd.evidence(BLUE), Evidence.VENT)
            self.assertEqual(fd.stance(YELLOW), Stance.THREAT)
            self.assertEqual(fd.evidence(YELLOW), Evidence.VENT)

            # Ranked suspects lead with a threat; the vote decision
            # returns a well-formed recommendation with prose.
            ranked = fd.rank_suspects()
            self.assertGreater(len(ranked), 0)
            self.assertEqual(ranked[0][1], Stance.THREAT)
            d = fd.vote_decision()
            self.assertIn(d.recommendation,
                          (VoteAction.SKIP, VoteAction.ABSTAIN,
                           VoteAction.CAST))
            self.assertTrue(d.rationale)

            # Attribution decomposes the winner's weight.
            ex = fd.explain_suspicion(ranked[0][0])
            self.assertIsNotNone(ex)
            self.assertEqual(ex.weight, fd.suspicion(ranked[0][0]))
            self.assertGreater(len(ex.terms), 0)

            # Prose surfaces speak the registered color names.
            summary = fd.render_vote_summary()
            self.assertIn("red", summary)
            self.assertEqual(fd.player_name(YELLOW), "green")

    def test_name_registry_round_trip(self):
        with Detector() as fd:
            fd.set_player_name(0, "holmes")
            self.assertEqual(fd.player_name(0), "holmes")
            self.assertEqual(fd.player_name(1), "p1")
            with self.assertRaises(ValueError):
                fd.set_player_name(1, "holmes")  # duplicate
            with self.assertRaises(ValueError):
                fd.set_player_name(1, "no spaces")
            fd.reset()
            self.assertEqual(fd.player_name(0), "holmes")

    def test_dump_and_pressure(self):
        with Detector() as fd:
            fd.register_crewrift()
            feed_weighted_round(fd)
            dump = fd.dump_state()
            self.assertIn("working-memory snapshot", dump)
            self.assertIsInstance(fd.game_pressure(), GamePressure)
            self.assertTrue(0 <= fd.win_probability() <= 1000)
            # 4 dossiered players (self is never dossiered): 3 alive + 1 dead.
            stats = fd.round_stats()
            self.assertEqual(stats.alive + stats.dead + stats.ejected, 4)


if __name__ == "__main__":
    unittest.main()
