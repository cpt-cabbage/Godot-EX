# Learning to Walk as the First Ability: Impaired-Locomotion Onboarding

Research notes for a first-person Godot 4.8-dev/Jolt game in which the player starts almost unable to walk and "walking" is the first ability. Scope: shipped precedents and their input maps, quantitative gait data for drunk/ataxic/infant locomotion, techniques to detune PD/feedback controllers so they wobble deterministically, and onboarding lessons. Adjacent notes in this folder already cover active ragdolls, first-person body/camera, controller learning and general biomechanics limits; this file stays on the "can't walk yet" problem.

Access notes: Fandom wikis (Fallout, Metal Gear, GTA, Kingdom Come) returned HTTP 402 and were replaced with GameBanshee/GamerGuides/StrategyWiki/Steam threads or left as gaps. PubMed's web front end served a CAPTCHA; abstracts were pulled through the NCBI E-utilities API instead. Paywalled Springer/Nature/ScienceDirect pages redirected to login walls; abstracts came from Semantic Scholar's API or E-utilities. Two arXiv PDFs were unreadable via WebFetch (binary); the ar5iv HTML renderings were used. Where only a search-result snippet was available and the page itself was not opened, the citation says so.

---

## KQ1. Shipped precedents with deliberately clumsy or per-limb locomotion control: exact input maps and intended feel

### Takeaway
The clumsy-locomotion canon splits into three families: (a) direct per-limb actuation (QWOP, Baby Steps, Ragdoll Runners, Manual Samuel, Mount Your Friends, Toribash), (b) indirect "nudge a self-balancing rig" control (Sumotori Dreams, Human Fall Flat, Octodad legs), and (c) a normal locomotion controller with a separate balance-correction channel (Death Stranding L2/R2). Designers of all three describe the feel as embodiment of a body you must learn, with competence living in the player's hands rather than in an upgrade system; Baby Steps (2025) is the closest shipped analogue and explicitly refuses upgrades.

### Cited Findings

**QWOP (Foddy, 2008)**
- Keys Q, W, O, P command the runner's thighs and calves, each part of both limbs controlled individually; the event is a 100 m Olympic sprint; the browser version released November 2008; the game is described as ragdoll-based. — [QWOP, Wikipedia (secondary)](https://en.wikipedia.org/wiki/QWOP)
- Foddy on self-set goals: "One of the things I found with QWOP is that people like to set their own goals in a game. Some people would feel like winners if they ran 5 meters, and others would feel like winners if they inched all the way along the track." — quoted in [QWOP, Wikipedia](https://en.wikipedia.org/wiki/QWOP) (secondary; original interview is ref. 14 there)
- Foddy's GDC 2012 Independent Games Summit talk "Learning to QWOPerate" discusses "what he avoids in designing control schemes" and argues "respecting players often involves being hostile to players, or humiliating them." — [GDC Vault, Learning to QWOPerate (2012)](https://www.gdcvault.com/play/1015704/Learning-to)

**Baby Steps (Cuzzillo, Boch, Foddy; Devolver Digital, 23 Sep 2025, PS5/Windows)**
- Input map: each trigger controls one leg; "Pull a trigger tight to lift and bend one of his knees, and release it bit by bit to swing out his lower leg and place his foot precisely where it needs to be"; the left stick supplies momentum; limbs are "incredibly sensitive to small changes in button depression"; Nate "falls over easily, faceplanting in the dirt and tumbling backward over rocky slopes like a ragdoll"; walking "requires so much concentration in the game's first few minutes" but becomes easier with practice. — [Jessica Conditt, Engadget, 28 Mar 2025](https://www.engadget.com/gaming/baby-steps-preview-serious-gameplay-in-a-silly-walking-sim-150008737.html)
- Wikipedia summary: players manually lift and place legs while "shifting his weight side to side while maintaining balance"; no game-over state, missteps produce ragdoll falls and rolling down terrain, with occasional checkpoints; Metacritic 77 (PC) / 75 (PS5). — [Baby Steps (video game), Wikipedia](https://en.wikipedia.org/wiki/Baby_Steps_(video_game))
- Cuzzillo on the origin of the manual-lift control: "At first, your feet would automatically lift as you pressed the stick forward, but pretty quickly it became clear that manually controlling the lifts gave you more control and felt better." and "The walking system has evolved a lot over the five years of development, and there have been hundreds of minor breakthroughs in making it feel good, consistent and controllable." — [Dan Amoroso, Game Rant, 17 Sep 2025](https://gamerant.com/baby-steps-interview-gamble-walking-gameplay-only/)
- Foddy on difficulty structure: "There are no adjustable difficulty levels in Baby Steps... we make the required stuff pretty easy (by our standards, anyway), but then the more optional or hidden stuff can be harder, way up to stuff that's extremely spicy." Boch: "difficulty isn't about trying to harm the player; it's about setting up situations where the player can feel an organic sense of accomplishment, free of systemic inflation." — [Game Rant, 17 Sep 2025](https://gamerant.com/baby-steps-interview-gamble-walking-gameplay-only/)
- Foddy on refusing progression systems: "gathering and crafting, resource management, map unlocks, quest chains, countable collectibles, fast-travel portals, traversal or skill upgrades... We had a very clear sense that we believed the core gameplay was deep enough... a huge amount of the game's overall design flows from a strict refusal to pile up systems." — [Dan Amoroso, Game Rant, 17 Sep 2025](https://gamerant.com/baby-steps-developers-systems-simple-limit-why/)
- Foddy on why the character cannot use his hands (the "problem of good hands"): "We can't polish everything... we have to show good hands in limited ways"; on learning to walk: "It strips away expectations, turning your attention to the process of building it back up"; he says players need roughly 40 hours before noticing the subtle level-design variation, with narrative scaffolding carrying motivation meanwhile. — [Jamin Warren, Kill Screen interview](https://www.killscreen.com/bennett-foddy-baby-steps-interview/) (the fetched page displayed a date of 3 Aug 2026; the game shipped Sep 2025, so verify the date)
- Reviews: Game Informer (Charles Harte, 6/10) says walking "felt impossible" then "oddly satisfying once you get the rhythm down", and complains that "getting off balance... immediately locks Nate into a ragdoll state". — [Game Informer review](https://gameinformer.com/review/baby-steps/unhappy-feet). Eurogamer (Robert Purchese, 4/5): "how you cope with frustration will determine how you cope with Baby Steps" and the game is "more approachable and forgiving than I assume many people will make out"; Polygon (Giovanni Colantonio) reports the "fabled rush that Dark Souls players feed off of". — [PC Gamer review roundup](https://www.pcgamer.com/games/sim/baby-steps-review-roundup-is-it-possible-to-love-and-hate-a-game-at-the-same-time-the-answer-clearly-is-yes/) (quotes from search snippet; the page truncated when fetched)

**Octodad: Dadliest Catch (Young Horses, 2014)**
- Mouse: left click = left leg, right click = right leg; the sticks or mouse pilot a single arm tentacle "in a largely counterintuitive way: one stick moves the tentacle up and down while the other stick moves it forward and backward, relative to the fixed camera"; controls split into Walk Mode and Interact Mode. — search snippets from [Steam control guide](https://steamcommunity.com/sharedfiles/filedetails/?id=352367971) and [Kotaku review](https://kotaku.com/octodad-dadliest-catch-the-kotaku-review-1512040710) (pages not opened)
- Post-mortem (Kevin Zuhn, 10 Feb 2015): the mouse scheme used mode switching between arm and leg control and "players would forget which mode they were in"; late in development the controller version auto-detected the intended mode, which "is a smoother, more pleasant game as a result, but it's a shame we never figured out a version that worked with the mouse"; they tightened feel by "increasing foot speed and reducing weightiness"; multiplayer splits limbs between players "like a floppy Voltron". — [Game Developer post-mortem pt. 3](https://www.gamedeveloper.com/design/octodad-dadliest-catch-post-mortem-pt-3-design)
- Young Horses: "never intended for the game to be overly difficult, and have always looked to create a balance between frustration and fun". — [Push Square interview, Aug 2013](https://www.pushsquare.com/news/2013/08/interview_octodad_dadliest_catch_developer_talks_tentacles_ps4_and_fishy_disguises) (search snippet)

**Surgeon Simulator 2013 (Bossa)**
- A = little finger, W = ring, E = middle, R = index, Space = thumb; mouse moves the hand; left button raises/lowers the hand; right button + mouse rotates it. — search snippets from [Surgeon Simulator Fandom wiki](https://surgeonsimulator.fandom.com/wiki/Surgeon_Simulator) and [Game Developer feature](https://www.gamedeveloper.com/business/the-blissfully-awkward-controls-of-i-surgeon-simulator-2013-i-) (pages not opened)

**Manual Samuel (Perfectly Paranormal, 2016)**
- PlayStation map: L2/R2 = left/right leg, L1/R1 = arms, Square = breathe in, Circle = breathe out, X = blink, right stick = balance; "Even the simple act of walking requires you to move each leg in succession, as otherwise Samuel is going to fall flat on his face". — search snippets from [TheSixthAxis](https://www.thesixthaxis.com/2016/10/12/manual-samuel-review/) and [GodisaGeek](https://godisageek.com/reviews/manual-samuel-review/) reviews (pages not opened)

**Sumotori Dreams (2007)**
- 87 KB executable made for the Breakpoint 2007 96k competition; "your input doesn't control the rig directly, but rather nudges it off-balance", locomotion emerging as the self-balancing rig compensates; rigs are "in constant autonomous motion" whenever off balance, which is why they stagger like drunks; aggressive input is counterproductive and winners use "small, controller movements, or sometimes not move at all". Fun-Motion credits developer Peter Sotesz. — [Fun-Motion review](https://www.fun-motion.com/physics-games/sumotori-dreams/)
- Bullet forum discussion of the controller: the balance goal is to place feet so that the COM projection lies inside the support polygon; such controllers "aren't too tricky to code but are really tricky to tune". — [Bullet physics forum thread](https://pybullet.org/Bullet/phpBB3/viewtopic.php?t=1094) (search snippet; not opened)

**Toribash (Nabi Studios, 2006)**
- Turn-based: each joint is set to one of four states (hold, relax, extend/raise, contract/lower); by default ten frames of simulation advance per turn; "Hold is when a joint becomes stiff... Relax... makes that joint soft and it will move around freely". — search snippets from [Steam guide "From tori noob to tori Pro"](https://steamcommunity.com/sharedfiles/filedetails/?id=291120097) and the [ToriLLE learning-environment paper, arXiv 1807.10110](https://arxiv.org/pdf/1807.10110) (pages not opened)

**Ragdoll Runners (Alex Bourdon)**
- One key per leg (Q left, W right), pressed alternately; a third key leans the athlete for sprint finishes, jumps and hurdles; "You cannot count on button mashing". — [ragdollrunners.com](https://ragdollrunners.com/) (search snippet)

**Human: Fall Flat (No Brakes Games, 2016)**
- Each hand on a mouse button (L2/R2 on console), the look direction raises the arms; Sakalauskas first prototyped independent per-arm stick control and rejected it: "I was using sticks on the controller to move each arm independently and D-pads could work, but that would be really messy controls." — [Dave Aubrey, TheGamer interview, 11 Feb 2021](https://www.thegamer.com/human-fall-flat-interview-tomas-sakalauskas/)
- Design pivot after watching his son ignore the puzzles: puzzles deliberately "not really watertight"; 58 million copies sold by Dec 2025. — [Human: Fall Flat, Wikipedia](https://en.wikipedia.org/wiki/Human:_Fall_Flat)

**Getting Over It with Bennett Foddy (2017)**
- Mouse/trackpad only, controlling the upper body and sledgehammer; Foddy made it "for a certain kind of person, to hurt them"; inspired by Jazzuo's Sexy Hiking (2002); narration delivers "quotations relating to disappointment and perseverance when significant progress is lost"; 2.7 million players by Humble launch metrics. — [Getting Over It, Wikipedia](https://en.wikipedia.org/wiki/Getting_Over_It_with_Bennett_Foddy)

**Mount Your Friends (Stegersaurus Games, 2013)**
- Each arm and leg is tied to a face button (gamepad) or WASD (keyboard); tilt the stick to move the selected limb. — search snippets from [Game Developer](https://www.gamedeveloper.com/design/translating-the-2d-hardbody-towers-of-i-mount-your-friends-i-into-3d) and [Hardcore Gamer](https://hardcoregamer.com/reviews/review-mount-your-friends/96776/) (pages not opened)

**Death Stranding (Kojima Productions, 2019)**
- Balance channel: "If Sam is tilting left you must press R2 to pull him further right. If he's tilting right you must press L2 to push him left"; holding both triggers braces the load; "Anything above 60KG will make Sam tilt sideways"; guides recommend holding both when descending steep slopes; the Power Skeleton (Episode 3) raises capacity to roughly 300 kg. — [PowerPyx, How to Balance Cargo](https://www.powerpyx.com/death-stranding-how-to-balance-cargo/)
- Kojima: "just walking in that world is really fun in the space" (playtesters found this before grasping the concept); "You have to select one bottle when you climb a mountain"; "if you're in the river, you can drift away – and that's in real life as well"; he expected "walking simulator" pushback the way stealth was first misunderstood. — [Joe Juba, Game Informer, 16 Sep 2019](https://gameinformer.com/interview/2019/09/16/hideo-kojima-answers-our-questions-about-death-stranding)

**Skate (EA Black Box, 2007) as analogy**
- Flick-It: every trick is a right-stick flick; "The movement of the stick emulates the movement of the feet of a real skater"; an accessibility toggle makes "any upward or sideways flick" produce a basic flip at max height. — search snippets from [EA accessibility resources](https://www.ea.com/able/resources/skate) and [Operation Sports](https://www.operationsports.com/skate-flick-it-controls-explained-how-to-perfect-your-technique/) (pages not opened)

### Inferences
- Three reusable control grammars fall out of the precedents: analog trigger = knee lift/extension (Baby Steps), stick = COM lean/momentum (Baby Steps, Sumotori), trigger pair = lateral balance correction (Death Stranding). A first-person game could start with the Baby Steps grammar and, as "walking" is learned, collapse it toward a standard stick-driven controller with the Death Stranding trigger channel as an optional expert layer.
- Both Baby Steps and Octodad found that automatic stepping felt worse than manual lifts early, but Octodad also found players lost track of control modes. This argues for a single consistent mapping whose meaning does not switch, and for having competence change the *response* to inputs rather than the inputs themselves.
- Sumotori's insight (input nudges a self-balancing rig; less input is better) is the natural fit for a PD-driven active ragdoll: the player perturbs a balance controller whose gains encode skill.
- Every designer quoted rejects "systemic inflation": skill lives in the player's hands. For a game whose fiction says the *character* is learning, a hybrid is needed (see KQ5) so the player's own learning curve and the character's parameter curve reinforce each other rather than fight.

### Gaps
- GIRP's exact input map (letter keys per hold, shift to flex) was not confirmed by any opened source; only QWOP's mapping was verified.
- No source was found that documents Sumotori Dreams' actual controller math; the Bullet forum thread is community speculation.
- No primary-source description of Baby Steps' internal controller (whether PD ragdoll, IK-plus-physics, etc.) was found; Cuzzillo's quotes describe evolution of feel, not implementation.

---

## KQ2. Openings that diegetically impair movement and then restore it: duration and reception

### Takeaway
Fallout 3 (toddler playpen) and MGSV (hospital crawl) are the canonical "learn to move" openings, but both are cinematic gates rather than motor-skill ramps: Fallout 3's toddler section is a few minutes of normal controls at low height, and MGSV's roughly 45-minute prologue (about 15 minutes with cutscenes skipped, not skippable as a whole) drew sustained complaints for slow crawling that the player cannot influence. Nothing found actually ramps controller competence over time; the closest shipped analogue is Death Stranding's weight-dependent balance, which is a state, not a skill.

### Cited Findings
- Fallout 3, "Baby Steps" (age 1): player wakes in a playpen after a walking tutorial; "You won't be able to do much, so go ahead and just open the playpen door and click on the You're Special book" to set S.P.E.C.I.A.L. stats; "Growing Up Fast" (age 10) grants the Pip-Boy 3000 and teaches shooting on three targets and a radroach with optional V.A.T.S.; "Future Imperfect" (age 16) is the ten-question G.O.A.T. skill test. — [GameBanshee walkthrough](https://www.gamebanshee.com/fallout3/walkthrough/growingup.php); GamerGuides describes the toddler as able to "tottle over" to the book. — [GamerGuides, Baby Steps](https://www.gamerguides.com/fallout-3/guide/fallout-3-walkthrough/prologue-vault-101/baby-steps)
- MGSV: The Phantom Pain prologue: full prologue about 45 minutes, about 15 minutes with cutscenes skipped, and it cannot be skipped as a whole. Player quotes: "I already know the story and I am not crawling on my belly... for 45 minutes". — [Steam discussion, "Can the ridiculous long prologue be skipped?"](https://steamcommunity.com/app/287700/discussions/0/371918937262241404/)
- MGSV player complaints: "First you have to crawl around on the ground for a very long time, slowly... Snake keeps tripping and falling over every little thing and the stuff that he can obviously use to get up, he doesn't use"; "you play for about 3 minutes, then boom, a cutscene. Play for about another 3 minutes, boom, cutscene"; "the prologue does nothing but confuse you". — [Steam discussion, "Anyone else find the prologue annoying?"](https://steamcommunity.com/app/287700/discussions/0/527273983050716645/)
- Leigh Alexander on the same sequence: "You crawl along the hospital floor not for stealth, as is tradition, but because at first you cannot stand, bare skin squeaking helplessly along a blood-slicked floor." — [Vice, 26 Oct 2015](https://www.vice.com/en/article/the-final-word-on-the-phantom-pain-a-video-game-about-video-games-1015/)
- Prey (2017) gates abilities via Neuromods and area access via items and abilities, Metroidvania-style, rather than impairing locomotion itself. — [Prey (2017), Wikipedia](https://en.wikipedia.org/wiki/Prey_(2017_video_game)) (search snippet)
- Kingdom Come: Deliverance drunkenness: comes on gradually, shown by the gold fill of the buff icon; up to the halfway point Strength, Speech, Vitality and Charisma gain up to +2, beyond it they are penalised up to −2; sprinting is disabled while drunk. — [Kingdom Come: Deliverance Fandom wiki, Drunkenness](https://kingdom-come-deliverance.fandom.com/wiki/Drunkenness) (search snippet; page returned 402 when fetched)

### Inferences
- The MGSV reaction is the key cautionary datum: impairment that removes *agency* (slow crawl, scripted trips, unskippable) is read as padding, whereas impairment that demands *skill* (Baby Steps) is read as the game. The design should keep the player's inputs consequential from frame one, even when the body is incompetent.
- Fallout 3 shows the cheapest version of the fiction: the toddler section is normal locomotion at reduced camera height and speed, and it is remembered fondly precisely because it is short and ends in a meaningful choice (stats). A "first ability" opening can borrow the structure (tiny space, one purposeful goal) while replacing scripted limits with a detuned controller.
- No shipped game was found that literally grows controller capability over the opening; that is white space, and the quantitative material in KQ4 and KQ5 supplies the parameters to do it.

### Gaps
- No timing data for Fallout 3's toddler section (minutes) was found in any opened source.
- Kojima's stated intent for the crawl-to-walk prologue ("theory of evolution" framing) appears only in a Fandom wiki summary that could not be opened (HTTP 402) and in search snippets; no primary interview was located.
- BioShock, Half-Life, Dying Light 2 and Cyberpunk 2077 openings were not researched in depth; no source on their opening control restrictions was opened.
- No review that quantifies how long players tolerated MGSV's crawl beyond the forum numbers above.

---

## KQ3. Drunk and stagger simulation in shipped games: what changes

### Takeaway
Rockstar's drunk state in GTA IV is the only shipped case documented as a fully simulated balance controller (Euphoria) with the player's stick reduced to a directional bias; GTA V instead tiers authored locomotion clipsets by intoxication (slightly / moderate / very drunk) plus camera blur and sway. Sea of Thieves adds input-independent lateral sway; Kingdom Come removes sprint and applies stat curves; Yakuza turns drunkenness into a combat buff. The reusable parameter set is: authority of player input over a balance controller, tiered gait clipsets, camera sway/blur, and disabled high-effort actions.

### Cited Findings
- GTA IV: Torsten Reil quoted: "There is a drinking mini-game in very detailed form where you can get drunk and he can actually then stumble around and you have to get home. But all of that is fully simulated." — [TechCrunch, 21 Feb 2008](https://techcrunch.com/2008/02/21/gta-iv-drunkenness-to-use-dynamic-physics-engine/) (the WebFetch summary labelled Reil a Rockstar executive; Reil is NaturalMotion's co-founder, so check the article text before quoting affiliation)
- GTA IV player control while drunk: "Animations under the control of Euphoria does allow control to a limited extent. This is especially noticeable when being drunk. Pushing the directional key to a certain direction will allow the player character to move towards that direction." Characters "flail, flip, attempt to stay standing when forced against the player character or an object". — [Grand Theft Wiki, Euphoria (community wiki)](https://www.grandtheftwiki.com/Euphoria)
- GTA IV preview coverage: "Niko's stance adjusts depending on the grade of the ground he's standing on"; a drunken stumbling scene was described in previews. — [GTA4.net summarising IGN](https://www.gta4.net/news/3966/ign-naturalmotions-euphoria-technology/)
- GTA V drunk locomotion is delivered by tiered movement clipsets: `MOVE_M@DRUNK@SLIGHTLYDRUNK` ("Tipsy 1"), `MOVE_M@DRUNK@MODERATEDRUNK` ("Drunk 1"), `MOVE_M@DRUNK@MODERATEDRUNK_HEAD_UP` ("Drunk 2"), `MOVE_M@DRUNK@VERYDRUNK` ("Very drunk"). — [elsewhat/gtav-mod-scene-director, clipset_movement.cpp (raw GitHub)](https://raw.githubusercontent.com/elsewhat/gtav-mod-scene-director/master/clipset_movement.cpp)
- GTA V's Euphoria (NaturalMotion) behaviour vocabulary exposed to scripts includes BodyBalance, ConfigureBalance ("configure various parameters used on any behavior that uses the dynamic balance"), StayUpright, Teeter, StaggerFall, HighFall, CatchFall, LeanInDirection, LeanRandom, LeanToPosition, HipsLeanRandom, ForceLeanInDirection, PedalLegs, BodyWrithe, BodyFoetal, among 80+ helpers. — [ScriptHookVDotNet docs, GTA.NaturalMotion.Euphoria](https://nitanmarcel.github.io/shvdn-docs.github.io/class_g_t_a_1_1_natural_motion_1_1_euphoria.html)
- GTA V drunk cheat: "blurry camera, unsteady walking, and exaggerated swerves when driving"; "the camera sways". — search snippets from cheat guides ([GTA BOOM](https://www.gtaboom.com/drunk-mode-cheat/), [gta5wiki](https://gta5wiki.com/cheats/drunk-mode/)); pages not opened
- Red Dead Redemption 2: Euphoria makes characters "shift their weight or balance, and dynamically stumble and trip if knocked over"; drunk state has camera shake. — search snippets ([Euphoria (software), Wikipedia](https://en.wikipedia.org/wiki/Euphoria_(software)); Nexus mod pages); pages not opened
- Sea of Thieves grog: "vision becomes slightly blurred and their movements become unstable, swaying left and right even without any player input"; handling sails, wheel, cannons becomes harder "as the player's actions become involuntary"; instruments play off-tune. — [Sea of Thieves wiki, Grog](https://seaofthieves.wiki.gg/wiki/Grog) (search snippet)
- Yakuza 0: intoxication has three phases that raise Heat-gauge gain and decay while fighting; two drunk-only Heat actions ("Essence of Pole Dancing", "Essence of Drunken Thrust"). — [Yakuza Fandom wiki, Intoxication](https://yakuza.fandom.com/wiki/Intoxication) and [GameFAQs guide](https://gamefaqs.gamespot.com/ps4/816306-yakuza-0/faqs/74451/heat-actions) (search snippets)
- Kingdom Come: sprint disabled while drunk; stat curve +2 to −2 across the buff. — [KCD Fandom wiki](https://kingdom-come-deliverance.fandom.com/wiki/Drunkenness) (search snippet)

### Inferences
- The GTA IV model (balance controller owns the body; the stick becomes a low-authority directional bias) is exactly a PD active-ragdoll with a Sumotori-style nudge input; competence can be expressed as the *authority* scalar of player input over the balancer, which is a single number to animate over the opening.
- GTA V's clipset tiers show the animation-blended alternative: author 3–4 gait tiers and cross-fade by a competence scalar. For a hybrid controller this maps to the blend weight toward authored animation (KQ5).
- Sea of Thieves' input-independent lateral sway is the simplest first-person cue and matches the medial-lateral bias in alcohol posturography (KQ4, Noda 2004); a filtered lateral COM-target offset is the physics equivalent.
- None of these systems documents input smoothing/lag numerically; the sensorimotor-delay literature (KQ4) is the substitute.

### Gaps
- No Rockstar or NaturalMotion primary document (GDC talk, paper) with drunk-behaviour parameters (balance gains, lean amplitudes, stick authority) was found; the Euphoria helper names are the closest thing to a parameter list.
- Skyrim's drunk effect was not researched; no source opened.
- RDR2 drunk specifics beyond camera shake were not found in an opened source.

---

## KQ4. Biomechanics of drunk, ataxic and infant gait: numbers to parameterise the wobble

### Takeaway
Three distinct "clumsy" signatures emerge. Alcohol at legal-limit levels barely changes forward gait speed or step width but multiplies static sway (area, path length, velocity, medial-lateral biased) once BAC passes roughly 0.5–0.8 mg/mL. Cerebellar ataxia is the clinical model for "cannot coordinate": speed down ~25%, stride length down ~18% of stature, stride-to-stride variability 2–3× normal, wide base, prolonged double support, arms held stiff. Infant walking is the "learning" model: novice walkers fall ~30 times per hour (about one fall per 70 steps), step width roughly halves over the first 6 months while speed range triples, hip flexion and arm elevation drop sharply in 15 weeks, and the mature pattern is set by about 3 years.

### Cited Findings

**Alcohol: gait**
- Randomised study of 100 adults (50 F, 50 M, 20–35 y) at 0.00 vs 0.11% BrAC on a Zebris pressure walkway (100 Hz). Forward gait (mean ± SD): women velocity 4.17 ± 0.65 → 4.33 ± 0.68 km/h, stride 128.4 ± 12.4 → 135.0 ± 14.0 cm, cadence 107.6 → 106.8 steps/min, step width 9.26 ± 2.16 → 9.57 ± 2.49 cm, foot rotation 3.24 → 3.75°, none significant. Men velocity 4.14 → 4.22 km/h, stride 135.1 → 138.5 cm, cadence 102.2 → 101.6, step width 12.09 → 11.96 cm, foot rotation 7.87 → 7.80° (p = 0.001, decreased). Backward gait in women: velocity 3.13 → 3.29 km/h (p < 0.001), stride 99.2 → 104.5 cm (p < 0.001), double stance 28.49 → 27.99% (p = 0.002). Paper cites prior work showing forward-gait changes "from 0.4 mg alcohol/L" and "a significant decrease in stability at a blood alcohol concentration of 0.10%". — [Sci Rep 2022, DOI 10.1038/s41598-022-23621-y (PMC9637089)](https://pmc.ncbi.nlm.nih.gov/articles/PMC9637089/)
- Treadmill study, 16 adults, increasing doses: "Ataxia or unsteadiness of gait was found to decrease during a blood alcohol concentration (BAC) of less than 0.4 mg/ml. Stride length was found to increase by increasing BAC." — Jansen, Thyssen, Brynskov, Z Rechtsmed 1985;94(2):103-7, [DOI 10.1007/BF00198678](https://doi.org/10.1007/BF00198678) (abstract via NCBI E-utilities)
- Smartphone IMU study (10 participants, 4 weekends): 24 time/frequency features from 3-axis acceleration and angular velocity over 5-step walks predicted estimated BAC with R² = 0.998 (test), mean eBAC 0.04, peak 0.23; no per-feature dose-response reported. — [Sensors 2017, DOI 10.3390/s17122897 (PMC5751642)](https://pmc.ncbi.nlm.nih.gov/articles/PMC5751642)

**Alcohol: standing sway (posturography)**
- 11 young adults, 540 mL sake in 10 min, Romberg stance 60 s at 20 Hz: "Parameters for distance, velocity, and area of body-sway significantly changed after alcohol intake, but the mean center of foot pressure and frequency of body-sway were unchanged... body-sway tends to increase in the medial/lateral direction as compared with the anterior/posterior direction." — Noda, Demura, Yamaji, Kitabayashi, Percept Mot Skills 2004;98(3):873-87, [DOI 10.2466/pms.98.3.873-887](https://doi.org/10.2466/pms.98.3.873-887) (abstract via E-utilities)
- 27 volunteers tracked 8 h: "The BAC threshold for increased body sway was estimated to be somewhere between 0.5 and 0.8 mg/mL", while positional alcohol nystagmus begins at 0.23 mg/mL. — Kubo et al., Am J Otolaryngol 1990;11(6):416-9, [DOI 10.1016/0196-0709(90)90121-b](https://doi.org/10.1016/0196-0709(90)90121-b) (abstract via E-utilities)
- Search-result snippet (page not opened): posturogram area rose to 3.8× control at 60 min after 3.5 mL/kg whisky (mean BAC 1.4 mg/mL), eyes open. — attributed to [Kubo et al., Acta Otolaryngol Suppl 468 (1989)](https://www.tandfonline.com/doi/abs/10.3109/00016488909139056)
- 31 participants at target 0.05% BAC (achieved mean 0.07 ± 0.018%, range 0.05–0.12%): stability scores fell in all conditions; the paper cites prior thresholds that "postural control was affected by BAC level higher than 0.08%" and that "movement pattern, stability, sensorimotor adaptation changes by BAC level higher than 0.06%". — [IJERPH 2022, DOI 10.3390/ijerph19073911 (PMC8997842)](https://pmc.ncbi.nlm.nih.gov/articles/PMC8997842/)

**Cerebellar ataxia**
- Meta-analysis of 21 studies, 14 spatiotemporal parameters: "compared with healthy controls, Cerebellar Ataxia patients walk with a reduced walking speed and cadence, reduced step length, stride length, and swing phase, increased walking base width, stride time, step time, stance phase and double limb support phase with increased variability of step length, stride length, and stride time." — Buckley, Mazzà, McNeill, Gait & Posture 2018, [DOI 10.1016/j.gaitpost.2017.11.024](https://doi.org/10.1016/j.gaitpost.2017.11.024) (abstract via Semantic Scholar API)
- 12 cerebellar patients vs 12 controls on treadmill: "significantly reduced step frequency with a prolonged stance and double limb support duration... All gait measurements were highly variable... step width and foot rotation angles were increased"; joint range of motion "almost normal... (with increased variability)"; tandem gait shows dysmetria, hypo/hypermetria and "inappropriate timing of foot placement". — Stolze et al., J Neurol Neurosurg Psychiatry 2002;73:310-2, [DOI 10.1136/jnnp.73.3.310](https://doi.org/10.1136/jnnp.73.3.310) (abstract via E-utilities)
- 13 cerebellar patients: "increased temporal variability of intra-limb coordination is a specific characteristic of cerebellar dysfunction", whereas vestibular and Parkinson patients show comparable balance-parameter abnormalities but lower temporal variability. — Ilg, Golla, Thier, Giese, Brain 2007;130:786-98, [DOI 10.1093/brain/awl376](https://doi.org/10.1093/brain/awl376) (abstract via E-utilities)
- IMU cohort (SCA n = 10, healthy elderly n = 67), comfortable speed: walking speed 1.10 ± 0.28 vs 1.43 ± 0.14 m/s; stride length 70.8 ± 14.4 vs 85.9 ± 5.2 % stature; CoV stride velocity 0.069 ± 0.069 vs 0.028 ± 0.008; CoV stride length 0.046 ± 0.037 vs 0.019 ± 0.006; arm-swing RoM 20.6 ± 10.6 vs 32.0 ± 13.9°; peak arm-swing velocity 173.6 vs 233.5 °/s; frontal trunk RoM 8.33 ± 1.99 vs 8.71 ± 2.77° (no group difference); lower speed predicted higher CoV (SCA R² = 0.64 for stride length). — Kroneberg et al., Sensors 2024;24(11):3476, [DOI 10.3390/s24113476 (PMC11174553)](https://pmc.ncbi.nlm.nih.gov/articles/PMC11174553/)

**Infant walking**
- 151 infants (11.8–19.3 months; 5–289 days of walking experience) in free play: walkers averaged 2,367.6 steps/h, 701.2 m/h and 17.4 falls/h; 12-month novice walkers averaged 1,456.1 steps/h, 296.9 m/h and 31.5 falls/h (in motion 33.1% of the time) vs expert crawlers 635.9 steps/h and 17.4 falls/h; walkers took 69.2 steps (12.5 m) per fall vs crawlers 54.7 steps (8.6 m); 46% of walking bouts were 1–3 steps and 23% single steps; a 6-hour multiplier gives "about 14,000 steps, 46 football fields, and 100 falls" per day; walking age correlated with step length (r = .74), step width (r = −.68), steps/h (r = .48), distance/h (r = .68) and falls/h (r = −.33). — Adolph et al., Psychological Science 2012, [DOI 10.1177/0956797612446346 (PMC3591461)](https://pmc.ncbi.nlm.nih.gov/articles/PMC3591461/)
- 5 children followed 2 years from walking onset: "Their range of speed increased threefold in the first 6 months of independent walking and then remained constant. In contrast, step width decreased approximately twofold"; early velocity gains come mostly from step length, and "After 5 months of independent walking, the pattern reverses, and increase in velocity is due primarily to increased cadence"; first ~5 months are "integration of postural constraints", followed by a "tuning phase". — Bril & Brenière, J Mot Behav 1992;24(1):105-16, [DOI 10.1080/00222895.1992.9941606](https://doi.org/10.1080/00222895.1992.9941606) (abstract via E-utilities)
- 20 infants, IMUs over 6 months from onset: "developmental shift at 2 months of walking experience"; after one month "characteristics of the pendulum mechanism were present in each examined toddler". — Bisi & Stagni, Gait & Posture 2015;41(2):574-9, [DOI 10.1016/j.gaitpost.2014.11.017](https://doi.org/10.1016/j.gaitpost.2014.11.017) (abstract via E-utilities)
- 26 toddlers grouped by 1–5, 6–10, 11–15 weeks of walking: step width 0.19 ± 0.04 → 0.15 ± 0.05 → 0.12 ± 0.02 m (p = 0.005); hip flexion RoM (left) 42.5 → 33.2 → 29.1° (p = 0.037); upper-limb elevation left 78.3 → 61.1 → 48.7° (p = 0.026), right 86.6 → 65.7 → 46.9° (p = 0.008); stride time ~1.02–1.05 s and step length 0.18 m unchanged. — Gimunová et al., IJERPH 2021, [DOI 10.3390/ijerph19010058 (PMC8744759)](https://pmc.ncbi.nlm.nih.gov/articles/PMC8744759/)
- 186 normal children aged 1–7: "At the inception of independent walking, the toddler steps with a wide base and hyperflexion of the hips and knees, holds the arms in abduction and the elbows in extension, and moves in a staccato manner"; then base narrows, movements smooth, reciprocal arm swing appears; the five determinants of mature gait are single-limb-stance duration, walking velocity, cadence, step length and pelvic-span/ankle-spread ratio; "As maturity advances, cadence decreases while walking velocity and step length increase"; a mature pattern "is well established at the age of three years"; sagittal joint rotations from age two resemble adults. — Sutherland, Olshen, Cooper, Woo, J Bone Joint Surg Am 1980;62:336-53, [PDF mirror](http://www.analisedemarcha.com/papers/historia/The%20development%20of%20mature%20gait%20-%201980.pdf) (text extracted locally; the age-by-age numeric tables are in image form and were not recoverable)
- 97 toddlers ~1–3 y: five gait parameters each correlate moderately or better with age; a regression on them estimates age (training R² = 0.683, test R² = 0.82). — Tsuyuki et al., Sci Rep 2023, [DOI 10.1038/s41598-023-30039-7](https://doi.org/10.1038/s41598-023-30039-7) (abstract via Semantic Scholar API)

**Sensorimotor delay (for "reaction time" detuning)**
- Robotic balance simulator with imposed delays of 20, 100, 200, 300, 400, 500 ms: at 20 ms all stable; at 100 ms minimal difficulty; at ≥200 ms "every participant exceeded the virtual limits at least once"; at 500 ms only 54 ± 9% of trial time within limits; with 400 ms, participants stayed upright 64 ± 9% of the first minute and 97 ± 3% by 100 minutes (time constant ~28–32.5 min); total compensated delay 560 ms (400 imposed + natural "~100–160 ms"); 60.8% retention after 3 months. — Rasman et al., eLife 2021, [DOI 10.7554/eLife.65085](https://elifesciences.org/articles/65085)
- Search-summary figures (pages not opened): proprioceptive delay about 50–60 ms, visual roughly 40–50 ms slower; healthy standing relies ~70% somatosensory, 20% vestibular, 10% visual. — [PLOS One 2014, visual reliance with artificial feedback delays](https://journals.plos.org/plosone/article?id=10.1371%2Fjournal.pone.0091554) and related results

### Parameter tables (values from the sources above)

Table 1. Adult alcohol effects (design reading: forward-gait spatiotemporals are nearly invariant; sway is where the signal is)

| Measure | Sober | Intoxicated | Level | Source |
|---|---|---|---|---|
| Forward gait velocity (F) | 4.17 km/h (1.16 m/s) | 4.33 km/h | 0.11% BrAC | Sci Rep 2022 |
| Step width (F / M) | 9.26 / 12.09 cm | 9.57 / 11.96 cm | 0.11% BrAC | Sci Rep 2022 |
| Cadence (F / M) | 107.6 / 102.2 spm | 106.8 / 101.6 spm | 0.11% BrAC | Sci Rep 2022 |
| Foot rotation (M) | 7.87° | 7.80° (p = 0.001) | 0.11% BrAC | Sci Rep 2022 |
| Backward double stance (F) | 28.49% | 27.99% | 0.11% BrAC | Sci Rep 2022 |
| Stride length | — | increases with BAC | up to study max | Jansen 1985 |
| Static sway distance/velocity/area | baseline | significantly increased; ML > AP; frequency unchanged | 540 mL sake | Noda 2004 |
| Sway onset threshold | — | 0.5–0.8 mg/mL BAC | — | Kubo 1990 |
| Sway area | 1× | 3.8× (eyes open) | 1.4 mg/mL | Kubo 1989 (snippet) |
| Stability thresholds cited | — | changes >0.06%; postural control affected >0.08% | — | IJERPH 2022 |

Table 2. Cerebellar ataxia vs healthy (design reading: the "cannot coordinate" signature)

| Measure | Healthy | Ataxia | Source |
|---|---|---|---|
| Walking speed | 1.43 ± 0.14 m/s | 1.10 ± 0.28 m/s | Kroneberg 2024 |
| Stride length | 85.9 % stature | 70.8 % stature | Kroneberg 2024 |
| CoV stride velocity | 0.028 | 0.069 | Kroneberg 2024 |
| CoV stride length | 0.019 | 0.046 | Kroneberg 2024 |
| Arm-swing RoM | 32.0° | 20.6° | Kroneberg 2024 |
| Frontal trunk RoM | 8.71° | 8.33° (n.s.) | Kroneberg 2024 |
| Cadence, step/stride length, swing | — | reduced | Buckley 2018 meta-analysis |
| Base width, stride/step time, stance, double support | — | increased | Buckley 2018; Stolze 2002 |
| Step width, foot rotation | — | increased | Stolze 2002 |
| Temporal variability of intra-limb coordination | — | increased, cerebellar-specific | Ilg 2007 |

Table 3. Infant walking development (design reading: the "learning" signature)

| Measure | Novice | Experienced | Timescale | Source |
|---|---|---|---|---|
| Falls per hour | 31.5 (12-mo walkers) | 17.4 (pooled) ; r = −.33 with walking age | months | Adolph 2012 |
| Steps per fall | 69.2 | — | — | Adolph 2012 |
| Steps per hour | 1,456 | 2,368 pooled | months | Adolph 2012 |
| Bout structure | 46% of bouts 1–3 steps; 23% single steps | — | — | Adolph 2012 |
| Step width | 0.19 m (weeks 1–5) | 0.15 m (6–10) → 0.12 m (11–15) | 15 weeks | Gimunová 2021 |
| Step width | 1× | ~0.5× | 6 months | Bril & Brenière 1992 |
| Speed range | 1× | ~3× | 6 months | Bril & Brenière 1992 |
| Velocity driver | step length | cadence (after ~5 months) | 5 months | Bril & Brenière 1992 |
| Hip flexion RoM | 42.5° | 29.1° | 15 weeks | Gimunová 2021 |
| Upper-limb elevation | 78–87° (abducted, elbows extended) | 47–49° → reciprocal swing | 15 weeks → months | Gimunová 2021; Sutherland 1980 |
| Stride time | ~1.02–1.05 s | unchanged over 15 weeks | — | Gimunová 2021 |
| Pendulum (inverted-pendulum) mechanics | absent in some | present in all after 1 month; trend shift at 2 months | 1–2 months | Bisi & Stagni 2015 |
| Mature pattern | — | established by ~3 years; adult-like sagittal rotations from 2 | years | Sutherland 1980 |

Table 4. Feedback-delay tolerance for standing balance (Rasman 2021)

| Added delay | Effect |
|---|---|
| 20 ms | all participants stable |
| 100 ms | minimal difficulty |
| 200–300 ms | everyone exceeds balance limits at least once |
| 400 ms | 64% of time upright in minute 1 → 97% after 100 min of practice |
| 500 ms | 54% of time within limits |
| natural delay | ~100–160 ms |

### Inferences
- "Drunk" and "toddler" are different looks. Drunk: near-normal step geometry with large, slow, ML-biased sway and delayed corrections. Toddler: wide base (0.19 m → 0.12 m), short staccato steps, high hip flexion (~42°), arms up and out (~80°), frequent short bouts and one fall per ~70 steps. The brief's "RL agent early in training" is closer to the toddler description with an added erratic, high-torque quality (KQ5).
- A single competence scalar s ∈ [0,1] can drive: step width 0.19 → 0.12 m (toddler data) then → ~0.09–0.12 m (adult sober widths); fall probability per step ~1/70 → ~0 ; stride-time CoV 0.07 → 0.03 (ataxia → healthy); speed 1.10 → 1.43 m/s or, for the truly novice phase, a 3× speed-range expansion; arm elevation 80° → 30° swing; hip flexion RoM 42° → 29°; added feedback delay 300 ms → 0 ms. These endpoints are all sourced above, which satisfies the reproducibility constraint.
- The Bril & Brenière two-phase result (postural integration first, then tuning of cadence) suggests two design phases: phase 1 unlocks *not falling* (balance gains, base width), phase 2 unlocks *speed* via cadence, which lines up with the later "running faster" superpower.
- Adolph's bout statistics are a strong argument that novice locomotion should be sold as *many short bouts with frequent stops*, not slow continuous walking; the character should be allowed to stop and re-balance often.

### Gaps
- Sutherland's numeric velocity/cadence/step-length values by age (1, 1.5, 2, 3 years) were not extractable (tables are images in the scanned PDF; secondary sources only paraphrased).
- No quantified drunk *gait* variability (stride-time CoV vs BAC) was found; most gait studies at ≤0.11% report null forward-gait effects, so the "drunk walk" of games is exaggerated relative to the literature.
- Toddler cadence and velocity in absolute units for the first weeks (Bril & Brenière give ratios only) were not obtained.
- Kubo 1989's 3.8× sway figure rests on a search snippet; the abstract was not opened.

---

## KQ5. Techniques to make a PD/feedback or animation-blended character wobble like an early RL policy, and whether shipping RL checkpoints as skill levels is viable

### Takeaway
Graphics research already exposes the knobs: SIMBICON's two balance-feedback gains (cd = 0.5, cv = 0.2 nominal), PD gains and torque limits (Learning to Get Up uses kp equal to the torque limit, kd = kp/10, 40 Hz control, 800 Hz sim, and a strong-to-weak torque curriculum in 0.95× steps toward 40–60% of human strength), control frequency, survival-bonus size and initial-state spread (Reda et al. 2020). RL papers describe untuned or early gaits as "highly dynamic and erratic", "entertaining but visually unsatisfactory", and looking "nothing like a real runner". No shipped game was found that ships a sequence of RL checkpoints as player progression; the closest are Ubisoft's difficulty via a penalty multiplier and Rocca et al. 2025's linear interpolation of policy network weights, which does show that interpolating between trained policies is stable.

### Cited Findings

**Detunable balance/PD controllers**
- SIMBICON joint torque τ = kp(θd − θ) − kd·θ̇; swing-hip target θd = θd0 + cd·d + cv·v where d is the horizontal COM-to-stance-ankle distance and v the COM velocity; the hip midpoint is "a simple and effective proxy" for the COM; "cd, cv are usually within the range of [0,1]. For our basic 3D walk controller we use cd = 0.5 and cv = 0.2 for the swing hip in all states, in both the coronal and sagittal planes"; cd "is important for providing balance during low-speed gaits or in-place stepping"; the torso "may exhibit a somewhat unnatural bobbing motion" from servoing against hip motion; the walk withstands a 350 N, 0.2 s push; feedback-error learning reduced torso oscillation from 5° to 0.5°; a generalised θd = θd0 + F·[d v]ᵀ adds stance-ankle feedback for quiet stance. — Yin, Loken, van de Panne, SIGGRAPH 2007, [PDF](https://www.cs.ubc.ca/~van/papers/2007-siggraph-simbicon.pdf) (text extracted locally)
- Learning to Get Up (Tao, Wilson, Gou, van de Panne, SIGGRAPH 2022): starts at default (1.0×) torque limits "designed according to documented values for humans"; when the accumulated minimum test reward reaches ω = 60 the limits become β^i × T with β = 0.95; final limits fall "below 60% of the default torque limit"; strategies "ending with torque limits ranging from 40% to 60% usually perceived as more natural than those terminating with high torque limits (above 70%)"; full-strength training yields "highly dynamic and erratic get-up motions, which do not resemble human get-up strategies"; PD gains kp = T, kd = kp/10; control 40 Hz, simulation 800 Hz; a slow-to-fast retiming stage uses κ ∈ [0.2, 0.8] (up to 5× slower). — [arXiv 2205.00307 (ar5iv)](https://ar5iv.labs.arxiv.org/html/2205.00307)
- Reda, Tao, van de Panne (MIG 2020): action repeat/control frequency has body-specific optima (Walker/Hopper AR = 1, Humanoid AR = 3–4, Ant AR = 2) and "controlling the actions at frequencies that are too high or too low is usually harmful"; higher torque limits aid exploration but yield unnatural behaviour, lower limits promote natural motion but local minima, hence "a torque limit curriculum during training, as a form of continuation method"; broader initial-state distributions hurt sample efficiency but generalise; a survival bonus that is too small or too large causes "falling forward or standing still". — [arXiv 2010.04304 (ar5iv)](https://ar5iv.labs.arxiv.org/html/2010.04304)

**What early/untuned RL gaits look like**
- Heess et al. 2017: bodies are a 9-DoF planar walker (6 actuated), 12-DoF quadruped (8 actuated), 28-DoF humanoid (21 actuated); reward is forward velocity plus torque penalty, plus upright bonus for the humanoid; terrain curricula "improve learning speed compared to stationary terrain with random obstacle heights"; humanoid results "were indeed much more diverse than for the other two bodies, with significant variations across seeds", sometimes "entertaining but visually unsatisfactory gaits", sensitive to "algorithm, exploration strategy, reward function, termination condition, and weight initialization"; video https://youtu.be/hx_bgoTF7bs. — [arXiv 1707.02286 (ar5iv)](https://ar5iv.labs.arxiv.org/html/1707.02286)
- DeepMind blog on the same work (10 Jul 2017): emergent behaviours are "very robust, but because the movements must emerge from scratch, they often do not look human-like." — [DeepMind blog](https://deepmind.google/discover/blog/producing-flexible-behaviours-in-simulated-environments/)
- Yu, Turk, Liu (SIGGRAPH 2018): a mirror-symmetry loss term "encourages symmetric actions"; a curriculum gives "modulated physical assistance to help the character with left/right balance and forward movement" and "gradually relaxes this assistance"; standard DRL motions "look nothing like a real runner". — [arXiv 1801.08093](https://arxiv.org/abs/1801.08093)

**Interpolating or tiering policies**
- Rocca, Andrews, Erleben (PACMCGIT 2025): controllers combined "through linear interpolation of network parameters" with "a graph-based weight regularization strategy to ensure that similar motions generate similar policy weights during training"; results "visually indistinguishable" from output blending; single policy evaluation per step; weight perturbation yields novel variations. — [Policy-space Interpolation project page](https://michelerocca.github.io/projects/policy-space_linterp/)
- Ubisoft Roller Champions RL bots (Iskander, Simoni, Alonso, Peter, NeurIPS 2020 workshop): "Lower difficulty models are preferred for Classic game mode, while any difficulty level can be used in Training with Bots. The penalty multiplier (section 3.3) can be used to obtain different difficulty levels"; decision interval 15 FixedUpdates; self-play. No mention of shipping earlier checkpoints. — [arXiv 2012.06031](https://arxiv.org/abs/2012.06031) (PDF text extracted locally)

**Human motor learning as noise reduction (for scheduling the ramp)**
- Cohen & Sternad 2009: performance variability decomposes into Tolerance, Noise and Covariation costs; in 6- or 15-day throwing practice "Changes in T-Cost were considerable at the beginning of practice; C-Cost and N-Cost diminished more slowly, with N-Cost remaining the highest." — [Exp Brain Res 2009, DOI 10.1007/s00221-008-1596-1](https://link.springer.com/article/10.1007/s00221-008-1596-1) (search snippet; page not opened)
- Practice "reduces task relevant variance modulation" and trajectories converge to stereotyped nominal paths; a model with time-jitter noise on the desired trajectory plus signal-dependent noise on motor commands reproduces the variance pattern. — [Sci Rep 2015, srep17659](https://www.nature.com/articles/srep17659) (search snippet; page not opened)

**Animation-blended route**
- GTA V ships four intoxication-tiered locomotion clipsets (KQ3), demonstrating the authored-animation tiering approach in a AAA title. — [clipset_movement.cpp](https://raw.githubusercontent.com/elsewhat/gtav-mod-scene-director/master/clipset_movement.cpp)

### Inferences (a concrete, sourced detuning recipe)
- Balance-feedback gain schedule (SIMBICON form): nominal cd = 0.5, cv = 0.2. Start well below (e.g. 0.15–0.25 / 0.05–0.1, values to tune) so foot placement under-corrects COM error, and add a low-pass-filtered offset to the COM proxy (hip midpoint) with medial-lateral bias, matching the ML-dominant sway of Noda 2004; anneal both toward nominal with the competence scalar.
- Torque/PD schedule: the Tao et al. curriculum is a ready-made competence curve in reverse. For the *toddler* look, begin around 40–50% of human torque limits with kp = T, kd = kp/10 (weak, floppy, falls under load); for the *early-RL* look, instead run at 1.0× with reduced damping and coarse control frequency to get "dynamic and erratic" corrections. Pick one signature deliberately; do not mix.
- Deterministic sensory delay: buffer the balance controller's state input by 200–300 ms at s = 0 (Rasman: everyone loses balance at ≥200 ms) and shrink to 0 by s = 1; because the delay is a ring buffer, the wobble is fully reproducible for a given input stream.
- Control-frequency detune: run the balance/step decision at a low action-repeat early (Reda et al. show frequency far from optimum is "usually harmful"), raising to the tuned rate with competence; this yields late, chunky steps without stochastic noise.
- Step-timing variance: inject deterministic pseudo-random jitter (seeded) on step-cycle duration with CoV ~0.07 at s = 0 → ~0.03 at s = 1 (ataxia → healthy figures), and on step width around 0.19 → 0.12 m.
- Animation blend route: author 3–4 gait tiers (as GTA V does) or a single motion-matching set, and lower the physics-vs-animation blend weight as competence grows; Yu et al.'s "assistance that is gradually relaxed" is the training-time analogue and can be reused verbatim at runtime as a virtual stabilising force whose gain decays with s.
- Shipping RL checkpoints: no precedent found. Viability concerns follow from the sources: early checkpoints are seed-sensitive and "visually unsatisfactory" in uncontrolled ways (Heess), and difficulty in production RL was tuned by reward shaping (Ubisoft) rather than checkpoint choice. The defensible path is Rocca-style weight interpolation between a deliberately weak-but-stable policy and a competent one, or the detuned classical controller above, which is deterministic and tunable per parameter.
- Human motor-learning literature says gross location errors (Tolerance) vanish first and execution noise last; mirror this by unlocking "aim your step" early and "steady your step" late.

### Gaps
- No paper or talk explicitly models motor learning/skill acquisition as a runtime parameter of a physics character (search terms below).
- DeepMimic-style tracking controllers under weak actuation were not researched here (covered by the adjacent controller-learning notes).
- No quantitative description of what an intermediate RL checkpoint looks like (e.g. fall rate vs training steps) was found in the opened sources; Heess's paper describes only final-policy diversity.
- Rocca et al. 2025's limits (how far apart policies can be before interpolation breaks) were not stated on the project page.

---

## KQ6. Onboarding theory: making impairment feel like growth rather than padding

### Takeaway
The successful clumsy-control games share a design stance that Foddy summarises as disobedience with respect: controls are consistent and honest, the *required* path is easy while optional paths are hard, progress is legible in the body itself, and tutorials are learned by doing rather than read. The failure mode is MGSV's: long, scripted incapacity with no skill expression. George Fan's ten tutorial rules and Foddy's "Eleven Flavors of Frustration" give a checklist to audit the opening against.

### Cited Findings
- Foddy, "Eleven Flavors of Frustration" (16 Jan 2017): flavours include Nearly There But Not Quite; Start Over; There And Back Again; Are We There Yet?; Going Nowhere; Going There Is Not Allowed; Others Can Get There But I Can't; I Don't Know What The First Step Is; A Draw; We've Been Here Before; "frustration is an essential ingredient in many (all?) of the most famous and influential designs"; "Games that are perfectly obedient are mere software." — [foddy.net](https://www.foddy.net/blog/2017/01/eleven-flavors-of-frustration/)
- Foddy at GDC 2012: "More than anything else, what I love about games, what I love about making games, is griefing the player. And I think players like being griefed, too."; he will not force tutorials, cutscenes or explanations that cannot be communicated through play, treating them as design failure. — [Cameron Kunzelman, Vice, 28 Sep 2017](https://www.vice.com/en/article/heres-why-the-designer-of-qwop-likes-making-people-mad/) and [Vice, "U MAD?"](https://www.vice.com/en/article/gvvj99/bennett-foddy-is-trolling-you-how-abusive-games-bring-out-the-best-in-us) (second article from search snippet)
- Getting Over It trailer text: "I could have made something you would have liked, a game that was empowering, that would save your progress and inch you steadily forward. Instead, I must confess: this isn't nice." — quoted in [Vice, 2017](https://www.vice.com/en/article/heres-why-the-designer-of-qwop-likes-making-people-mad/)
- Baby Steps: no difficulty settings; "required stuff pretty easy... optional or hidden stuff can be harder"; difficulty as "organic sense of accomplishment, free of systemic inflation". — [Game Rant, 17 Sep 2025](https://gamerant.com/baby-steps-interview-gamble-walking-gameplay-only/)
- Baby Steps: Foddy says learning to walk "strips away expectations, turning your attention to the process of building it back up"; narrative scaffolding carries motivation until (~40 h) players read the level design. — [Kill Screen](https://www.killscreen.com/bennett-foddy-baby-steps-interview/)
- Baby Steps reception: walking "felt impossible" then "oddly satisfying once you get the rhythm down" (Game Informer 6/10); "more approachable and forgiving than I assume many people will make out" (Eurogamer 4/5); Metacritic 77/75. — [Game Informer](https://gameinformer.com/review/baby-steps/unhappy-feet); [PC Gamer roundup](https://www.pcgamer.com/games/sim/baby-steps-review-roundup-is-it-possible-to-love-and-hate-a-game-at-the-same-time-the-answer-clearly-is-yes/); [Wikipedia](https://en.wikipedia.org/wiki/Baby_Steps_(video_game))
- George Fan, GDC 2012, ten tutorial tips: blend the tutorial into the game; better to have the player do than read; spread out teaching ("players don't need to understand everything right away"); just get the player to do it once; use fewer words (max eight per screen); unobtrusive messaging; adaptive messaging only for those struggling; don't create noise; use visuals to teach; leverage what people already know. — [Tom Curtis, Game Developer, 9 Mar 2012](https://www.gamedeveloper.com/design/gdc-2012-10-tutorial-tips-from-i-plants-vs-zombies-i-creator-george-fan)
- Octodad post-mortem: mode-switching confused players; auto-detection fixed it; controls tightened by faster feet and less weight; Young Horses never intended the game to be "overly difficult". — [Game Developer, 2015](https://www.gamedeveloper.com/design/octodad-dadliest-catch-post-mortem-pt-3-design); [Push Square, 2013](https://www.pushsquare.com/news/2013/08/interview_octodad_dadliest_catch_developer_talks_tentacles_ps4_and_fishy_disguises)
- Human: Fall Flat: Sakalauskas made sure that if a player tried something "either I would make it unattractive to try if it would be too difficult to implement, or I would make sure that it works as expected by the player." — [TheGamer, 2021](https://www.thegamer.com/human-fall-flat-interview-tomas-sakalauskas/)
- MGSV counter-example: ~45 min unskippable prologue; "crawl around on the ground for a very long time, slowly"; scripted trips "he doesn't use" obvious supports. — [Steam threads](https://steamcommunity.com/app/287700/discussions/0/527273983050716645/), [Steam threads](https://steamcommunity.com/app/287700/discussions/0/371918937262241404/)
- Death Stranding: Kojima found in playtests that walking itself was fun before players grasped the concept; he expected "walking simulator" criticism. — [Game Informer, 2019](https://gameinformer.com/interview/2019/09/16/hideo-kojima-answers-our-questions-about-death-stranding)
- Infant data for pacing: novice walkers fall ~31 times/hour yet keep walking, with 46% of bouts only 1–3 steps. — [Adolph 2012](https://pmc.ncbi.nlm.nih.gov/articles/PMC3591461/)

### Inferences
- Audit the opening against Foddy's flavours: aim for "Nearly There But Not Quite" (falls that are recoverable and clearly caused by the player), avoid "Start Over" and "We've Been Here Before" (no long walk-backs after a fall, no repeated scripted sequences), and avoid "Going Nowhere" (input must always visibly move the body, even badly). MGSV's crawl violates the last two.
- Make progress legible in the body, not a UI bar: the sourced signals are narrowing step width, arms coming down from ~80° abduction to swing, fewer falls per hundred steps, smoother (less staccato) cadence and a steadier first-person camera, all of which the KQ4 tables parameterise.
- Keep the impaired phase short on the *required* path (Fallout 3 and Foddy's "required stuff easy") and put the long tail of clumsy-skill mastery in optional content, exactly as Baby Steps structures difficulty.
- Use Fan's rules: teach one leg-lift by doing, eight-word prompts at most, adaptive prompts only if the player has fallen N times, and lean on the "toddler" cultural script so players already know what the body is trying to do.
- Frequent falls are acceptable if recovery is cheap and quick: the infant baseline of a fall every ~70 steps with immediate get-up is a defensible target for the very first minutes, tapering to near zero as walking is "learned".

### Gaps
- No study quantifying how many minutes players tolerate deliberately clumsy controls before quitting was found; the only quantitative proxies are MGSV's 45-minute complaints and Baby Steps' Metacritic 75–77 with "first few minutes" impossibility.
- No talk or paper was found that specifically frames impaired-then-restored control as a growth mechanic; commentary is spread across interviews rather than a single design source.

---

## Better search terms and venues

- Baby Steps internals: search "Baby Steps" with "Cuzzillo" "controller" "IK" "physics" on YouTube (Noclip, GDC 2026 talks if any), the Devolver Digital press site, and the Ape Out/Baby Steps GDC Vault entries; look for a 2026 GDC or Nordic Game postmortem titled with "walking" or "Nate".
- Foddy primary sources: foddy.net blog archive (search "control", "frustration", "QWOP"), GDC Vault "Learning to QWOPerate" (2012) video, "My Perfect Console" podcast (Simon Parkin) episode with Foddy.
- Euphoria/drunk parameters: search "NaturalMotion behaviour" "bodyBalance" "staggerFall" "configureBalance" parameters in the OpenIV/ScriptHookV modding docs and the fivem natives reference; GDC Vault "Euphoria" "Rockstar" "RAGE animation" talks (2009–2013); Torsten Reil's GDC 2008/2009 talks.
- Alcohol gait numbers: Gait & Posture, Journal of Biomechanics, Alcoholism: Clinical and Experimental Research, Forensic Science International; terms "ethanol" "spatiotemporal gait" "stride time variability" "mediolateral sway" "0.08" "BrAC"; the Zebris/GAITRite alcohol studies by Nieschalk (1999) and the Sci Rep 2022 group's earlier male-only paper cited there.
- Ataxia norms: Gait & Posture (Buckley 2018 supplementary tables give pooled means per parameter), Cerebellum journal, "SARA score" "gait variability" "step width cm" "double support %".
- Infant gait: Karen Adolph's NYU Infant Action Lab (Databrary videos), Sutherland 1980 tables via JBJS, Hallemans et al. 2005–2006 "3D joint dynamics of walking in toddlers", Ivanenko et al. 2004 "Development of pendulum mechanism", terms "newly walking toddlers" "step width normalized" "walking experience weeks".
- Controller detuning: ACM TOG / SIGGRAPH / SCA / MIG for "torque limit curriculum", "action repeat locomotion", "balance feedback gain", "assistive force curriculum", "policy interpolation"; van de Panne lab publications page; search "skill level" "novice policy" "sub-optimal demonstration" "learning curve visualization" in RL-for-animation papers; Rocca et al. 2025 full paper (PACMCGIT) for interpolation limits.
- Shipping RL tiers: Ubisoft La Forge blog and NeurIPS/AAAI workshop papers ("Roller Champions", "For Honor"), EA SEED "imitation learning" talks, Sony Gran Turismo Sophy difficulty-tier papers (Nature 2022 and follow-ups) with terms "difficulty levels" "checkpoint" "penalty multiplier".
- Onboarding: GDC Vault "How I Got My Mom to Play Through Plants vs. Zombies" (2012), "Tutorials 101", Celia Hodent's "gamer's brain" onboarding talks; Game UX Summit; terms "onboarding" "tutorialization" "agency" "diegetic tutorial" "restricted controls opening".
- MGSV/Fallout 3 openings: Metal Gear Wiki and Fallout Wiki pages (open in a browser; they blocked automated fetch), Kojima interviews in Famitsu/Edge 2015 with "prologue" "hospital" "crawl"; review archives at Eurogamer/Polygon for "prologue" mentions.
