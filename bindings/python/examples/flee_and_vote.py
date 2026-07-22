"""A stylized detector session: collect evidence, flee a threat, get
tailed, hit the emergency button, and argue the case in vote chat.

Every quoted line of prose below is real library output — run this
against a built libfakedetector to regenerate it:

    FD_LIBRARY_PATH=../../build/libfakedetector.so.1 python3 flee_and_vote.py

The story: we are red. Green is murdered in storage. Purple was there
inside the kill window — and got there faster than the map allows.
Purple then corners us in medbay, follows us room to room, and we run
for the cafeteria button.
"""

from fakedetector import Detector, Phase, Risk, Stance, ClaimKind, VoteAction

RED, BLUE, GREEN, PINK = 0, 1, 2, 3
PURPLE, CYAN = 6, 7


def beat(title):
    print(f"\n--- {title} ---")


def main():
    fd = Detector()                      # embedded rules, no files needed
    fd.register_crewrift()               # ids 0..15 = CrewRift colors
    fd.set_self(RED)
    fd.observe_round_config(8, 2)        # 8 players, 2 impostors

    # The map: sparse doorway adjacency, honest walk costs in ticks.
    fd.add_room_link("cafeteria", "admin", 60)
    fd.add_room_link("cafeteria", "medbay", 50)
    fd.add_room_link("medbay", "storage", 70)
    fd.add_room_link("admin", "storage", 80)

    beat("early game: unremarkable evidence accumulates")
    fd.observe_self(10, Phase.PLAYING, "cafeteria")
    fd.observe_player(10, BLUE, "cafeteria")
    fd.observe_player(10, PINK, "cafeteria")
    fd.observe_task_completion(40, BLUE, "cafeteria")
    fd.observe_player(40, BLUE, "cafeteria")
    fd.observe_task_completion(75, BLUE, "cafeteria")
    fd.observe_player(75, BLUE, "cafeteria")
    fd.observe_player(90, GREEN, "storage")      # green alive in storage
    fd.observe_player(100, PURPLE, "medbay")     # purple in medbay
    fd.observe_player(140, CYAN, "admin")
    print("blue is doing tasks; green heads to storage; purple in medbay")

    beat("tick 140: purple is in storage — impossibly fast")
    # medbay->storage costs 70 honest ticks; purple did it in 40.
    fd.observe_player(140, PURPLE, "storage")
    beat("tick 150: we find green's body in storage")
    fd.observe_body(150, GREEN, "storage")
    fd.run()

    print(f"purple stance:    {fd.stance(PURPLE).name}"
          f" (evidence: {fd.evidence(PURPLE).name})")
    print(f"purple suspicion: {fd.suspicion(PURPLE)} / 1000")
    print(f"blue suspicion:   {fd.suspicion(BLUE)} / 1000")

    beat("tick 170: alone in medbay with purple — do we stay?")
    fd.observe_self(170, Phase.PLAYING, "medbay")
    fd.observe_player(170, PURPLE, "medbay")
    risk = fd.alone_risk(PURPLE)
    print(f"alone_risk(purple) = {risk.name}")
    print(f'  "{fd.alone_risk_rationale(PURPLE)}"')
    if risk == Risk.HIGH:
        print("policy: FLEE toward the button")

    beat("tick 200-230: we run for cafeteria; purple follows")
    fd.observe_self(200, Phase.PLAYING, "cafeteria")
    fd.observe_player(230, PURPLE, "cafeteria")   # still behind us
    print(f"purple is here too; alone_risk still "
          f"{fd.alone_risk(PURPLE).name} — hit the button")

    beat("tick 240: emergency meeting")
    fd.observe_self(240, Phase.VOTING, "cafeteria")
    # Purple defends itself with a location claim for tick 140 — refuted
    # by our own storage sighting at that tick.
    fd.observe_claim(140, PURPLE, ClaimKind.LOCATION, "admin")
    fd.run()

    d = fd.vote_decision()
    print(f"vote decision: {d.recommendation.name}"
          f" -> {fd.player_name(d.target)}"
          f" (suspicion {d.suspicion}, confidence {d.confidence})")
    print(f'  rationale: "{d.rationale}"')

    beat("what we paste into vote chat")
    print(fd.render_vote_summary())
    print(fd.render_case())

    fd.close()


if __name__ == "__main__":
    main()
