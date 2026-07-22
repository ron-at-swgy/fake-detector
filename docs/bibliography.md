# Bibliography

Literature that shaped the probabilistic belief layer (`clp/suspicion.clp`),
the testimony/social feeders (`clp/claims.clp`, `clp/social.clp`), and the
vote-decision API. The unifying lesson, which the design follows:

> Reason with a **normalized probability distribution over who is the
> impostor**, update it with every observation, and take vote decisions by
> **expected utility against the current game state** — not by thresholding a
> label.

| Ref | Paper | What it contributed here |
|-----|-------|--------------------------|
| arXiv:1009.1031 | Migdał, *A Mathematical Model of the Mafia Game* (2013) | parity model, √N balance, the `w(n,m)` win formula behind `fd_win_probability` and `game-pressure` |
| arXiv:1906.02330 | Serrino et al., *Finding Friend and Foe* / **DeepRole** (NeurIPS 2019) | joint belief over roles, inconsistency pruning, renormalization — the `logodds-term` → `suspicion` substrate |
| arXiv:2212.08279 | Lai et al., *Werewolf Among Us* (2022) | persuasion taxonomy, accusation/defense cues — the `accuses`/`defends` social graph |
| arXiv:2405.19946 | Jin et al., *Learning to Discuss Strategically* / ONUW (2024) | incremental Bayesian belief update from discussion — the claims/testimony layer |
| arXiv:2407.16521 | Chi et al., *AmongAgents* (2024) | Among-Us-specific behavior archetypes informing evidence-channel choice |
| ACM Games 2025, art. 3774646 | Wang, *Optimal Strategy in the Werewolf Game* | extensive-form Bayesian game framing, common-knowledge role counts (`roster`) |
| ISBN 978-1-4020-5839-4 | van Ditmarsch et al., *Dynamic Epistemic Logic* (Springer 2007) | public announcement as world pruning — alibi/contradiction as hard eliminations |
| Mgmt. Sci. (2004 reprint) | Harsanyi, *Games with Incomplete Information Played by "Bayesian" Players* | Bayesian type spaces, subjective belief distributions |
