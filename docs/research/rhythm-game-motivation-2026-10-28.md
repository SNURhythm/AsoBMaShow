# Rhythm-game motivation and progression

Research date: 2026-10-28. This is desk research, not interviews or an
experiment with AsoBMaShow players. The user authorized moving from research
into implementation once the research supported a reasonable first direction.

## Findings and evidence boundaries

Rhythm-game progression supports several different intentions: harder clears,
more accurate scores, collection completion, pattern-specific practice,
course preparation, discovery, and participation with friends. These are
working product distinctions, not validated population segments.

### Documented ecosystem mechanics

- BMS difficulty tables are both difficulty references and curated collections.
  The [beatoraja guide](https://github.com/wcko87/beatoraja-english-guide/wiki/Difficulty-Tables)
  distinguishes collections suited to folder lamps from broader discovery.
  [Stellaverse](https://stellabms.xyz/introduce) explicitly rates its tables
  against EASY gauge and distinguishes the pattern preferences of
  Satellite/Stella from Solar/Supernova. A level is not a universal estimate
  of HARD-clear or AAA difficulty.
- [IIDX score graphs](https://p.eagate.573.jp/game/2dx/33/howto/play/score_graph.html)
  provide personal-best, grade, custom-rate, and next-higher-rival targets.
  [DJ TRAINING](https://p.eagate.573.jp/game/2dx/33/howto/new/training.html)
  offers progressive chart folders; [dan courses](https://p.eagate.573.jp/game/2dx/33/howto/mode/game_mode.html)
  test four stages with restricted options.
- [osu! performance points](https://osu.ppy.sh/wiki/en/Performance_points)
  weight stronger performances more heavily.
  [Beatmap Spotlights](https://osu.ppy.sh/wiki/en/Beatmap_Spotlights) couples
  curated discovery with seasonal competition.
- [Etterna](https://github.com/etternagame/etterna/blob/master/Themes/_fallback/metrics.ini)
  exposes skillset sorting, and its [release notes](https://github.com/etternagame/etterna/releases)
  document rate-adjustable practice.
- maimai distinguishes [DX Rating](https://maimai.sega.com/play/newfunction2/)
  from [recommendations based on rating, taste, and popularity](https://maimai.sega.com/play/newfunction3/).
  [Circle challenges](https://maimai.sega.com/play/newfunction/) provide shared goals.
- [Tachi v2.2](https://github.com/zkldi/Tachi/releases/tag/v2.2.0) documents
  chart/folder goals, rival comparisons, and thematic quests.

These examples establish precedent, not motivational effectiveness.

### Community observations, not prescriptions

The [IIDX practice guide](https://iidx.org/intermediate/tips) recommends varied
charts around the player's level, separate normal/hard tier references, and
RANDOM for pattern variety rather than favorable-pattern fishing.
The [Etterna improvement guide](https://community.etternaonline.com/t/improving-in-etterna/485)
warns about noisy short-term results and imperfect skill classifications,
especially for beginners.

A small purposive sample of first-person BMS accounts illustrates disagreement:

- [Uya (2024)](https://note.com/uya_otoge/n/ncfe62e8acf18) describes frustration
  from only pushing EASY clears, switching to other objectives, and the
  satisfaction of EXH/FC completion despite uncertainty about transfer.
- [Qooma (2020)](https://qooma1024.blogspot.com/2020/03/bms.html) uses ordered
  selections to reduce decision time, revisits older charts for visible
  improvement, and includes freely chosen charts.
- [Eco (2025)](https://note.com/eco_iidx/n/n52af89fe5528) distinguishes new
  lamps obtained by exploring an unplayed library from increased capability,
  and describes the discouragement when easy new lamps run out.

These are selected accounts, not a representative survey. Their training
claims, exact schedules, and causal interpretations are not adopted as facts.
The useful product signal is the need to retain different personally meaningful
goals without declaring one playing style correct.

### Psychological evidence

- [Harkin et al. (2016)](https://www.apa.org/pubs/journals/releases/bul-bul0000025.pdf):
  138 randomized studies, 19,951 participants; progress-monitoring
  interventions improved goal attainment on average (d = 0.40).
  This does not identify weekly recaps as the optimal format or prove
  rhythm-game enjoyment/retention effects.
- [Patall et al. (2008)](https://pubmed.ncbi.nlm.nih.gov/18298272/): a
  41-study meta-analysis found benefits of choice, moderated by context.
  It does not prescribe a particular recommendation-list size.
- [Sheeran et al. (2024, author manuscript)](https://www.researchgate.net/publication/378870694_The_When_and_How_of_Planning_Meta-Analysis_of_the_Scope_and_Components_of_Implementation_Intentions_in_642_Tests):
  642 experimental tests support if-then planning; a useful aid to executing
  an existing intention, not evidence that reminders create interest.
- [Breines and Chen (2012)](https://pubmed.ncbi.nlm.nih.gov/22645164/): four
  experiments found self-compassion can support improvement motivation after
  setbacks. Supportive UI copy alone was not the tested intervention.
- [Murayama and Elliot (2012)](https://motivationsciencelab.com/wp-content/uploads/2022/02/Murayama_Elliot_2012_The-competition%E2%80%93performance-relation.pdf):
  competition did not have a noteworthy overall performance relationship;
  approach and avoidance processes can work in opposite directions.
- [Ryan, Rigby, and Przybylski (2006)](https://selfdeterminationtheory.org/SDT/documents/2006_RyanRigbyPrzybylski_MandE.pdf):
  game studies associate autonomy, competence, and relatedness with enjoyment
  and future play. These associations do not causally validate our feature.

## Product decision

Implement persistent personal **chart goals** first: a chosen lamp and/or
score target, current best, and an exact remaining score gap. This is a product
hypothesis supported by familiar rhythm-game objectives and general progress
monitoring/choice evidence. It is not a claim of proven motivational benefit.

Combo already provides recurrence, points, attendance protection, and stopping
cues. The app also already has folder lamps, filtered random selection,
section practice, loops, rate adjustment, presets, and timing analysis. The
first addition should connect a lasting intention to those capabilities.

Tradeoffs: explicit targets may encourage excessive retrying; collection
completion is not proof of skill transfer; a best-ever record is not a current
capability estimate. Preserve player choice, no deadlines, no automatic
escalation, no new points, no promise of a near-clear based on total misses.

Later candidates: explainable curated practice selections, then optional
chosen rivals/shared chart sets. Table-wide goals, universal skill ratings,
automatic weakness diagnosis, and a weekly skill recap remain unselected.

## Validation questions

Observe chart selection and a disappointing session across clear-focused,
score-focused, newcomer, returning, and music-first players. Does the main
friction involve remembering goals, choosing charts, interpreting progress,
or companionship? Do goals remain satisfying without creating retry pressure?
Evaluate voluntary return and session satisfaction separately from playtime;
evaluate performance on other comparable charts separately from a trained PB.
