# Human biomechanics limits and injury thresholds for a physics-gated superhuman movement system

Research date: 2026-09-27. Scope: human performance ceilings and injury thresholds for jumping, falling, running collisions, hanging/grabbing and striking, plus the physics formulas linking them and how the limits scale for a superhuman. All numbers carry a citation. Where a primary source could not be opened, the note says so and names what was used instead. Derived tables (marked "computed") use the formulas in the "Physics formulas and derived tables" section with m = 70 kg and g = 9.81 m/s^2.

Access notes (apply throughout):
- The web-search budget for this session ran out part-way through; later lookups were done with PubMed E-utilities, the Semantic Scholar API, and direct page fetches instead.
- Rosén & Sander 2009 was read from the authors' preprint PDF hosted by NACTO (text extracted locally); the ScienceDirect page was not opened.
- NHTSA "Development of Improved Injury Criteria" (rev_criteria.pdf), the eCFR page for 49 CFR 571.208, the FAA Snyder 1963 PDF, the Cureus full text, the Springer tibia off-axis paper and the SAGE abstract for Smith et al. 2017 all returned 403/redirects. Substitutes: the govinfo XML of 49 CFR 571.208 (2023 edition), PubMed abstracts, and search-result snippets (flagged inline as "snippet only").

## Key question 1: Human performance ceilings (sprint speed, acceleration, jump, deceleration, reaction time, stopping distance)

### Takeaway
An elite sprinter peaks at about 12.3 m/s; a trained team-sport athlete sprints at roughly 7.4 m/s and can brake at about 4-8.5 m/s^2 (peak), needing about 1.5 s and 7 m to stop from 7.35 m/s; simple visual reaction time is about 0.21-0.23 s. A good adult vertical jump (centre-of-mass rise) is 0.4-0.7 m, and a standing long jump is 2.2-3.7 m.

### Cited Findings

Sprint speed and acceleration

| Quantity | Value | Source |
|---|---|---|
| Usain Bolt maximum 10 m-segment velocity, Berlin 2009 (9.58 s) | 12.34 m/s (70-80 m segment); segment means 5.26 (0-10 m), 10.20 (10-20 m), 10.87 (20-30 m), 12.05 (30-40 m), 11.90, 12.19, 12.19, 12.34, 12.05, 12.05 m/s | Maćkała K, Mero A (2013) "A kinematics analysis of three best 100 m performances ever", J Human Kinetics 36:149-160, doi:10.2478/hukin-2013-0015 — [PMC3661886](https://pmc.ncbi.nlm.nih.gov/articles/PMC3661886/) |
| Bolt instantaneous peak velocity (laser measurement, Berlin 2009) | 12.32 m/s at the 52 m mark; maximum-velocity phase held over 50-100 m | "Multicomponent Velocity Measurement for Linear Sprinting: Usain Bolt's 100 m World-Record Analysis", Bioengineering 10(11):1254 (2023) — [MDPI](https://www.mdpi.com/2306-5354/10/11/1254) / [PMC10669785](https://www.ncbi.nlm.nih.gov/pmc/articles/PMC10669785/) (snippet only; full text not opened) |
| Theoretical model of Bolt's 100 m (acceleration, drag, power) | Paper exists; numeric values not extractable from the abstract | Hernández Gómez JJ, Marquina V, Gómez RW (2013) "On the performance of Usain Bolt in the 100 metre sprint", Eur J Phys 34:1227-1233 — [arXiv:1305.3947](https://arxiv.org/abs/1305.3947) |
| Trained (university) team-sport athletes: approach velocity in maximal sprint-to-stop tests | 7.35 m/s | Harper DJ et al. (2022) "Biomechanical and Neuromuscular Performance Requirements of Horizontal Deceleration", Sports Medicine 52:2321-2354, doi:10.1007/s40279-022-01693-0 — [PMC9474351](https://pmc.ncbi.nlm.nih.gov/articles/PMC9474351/) |

Jumping

| Quantity | Value | Source |
|---|---|---|
| Simulated maximal countermovement jump of an 82 kg human musculoskeletal model | 40 cm centre-of-mass rise; peak power 49 W/kg | Bobbert MF (2013) "Effects of isometric scaling on vertical jumping performance", PLOS ONE 8(8):e71209, doi:10.1371/journal.pone.0071209 — [PLOS](https://journals.plos.org/plosone/article?id=10.1371/journal.pone.0071209) |
| Competitive female handball/basketball players, force-plate CMJ height | 0.26 m (weaker group) to 0.30 m (stronger group) | García-Sánchez C et al. (2026) Int J Sports Physiol Perform 21(4):521-530, doi:10.1123/ijspp.2025-0182 — [PubMed 41764994](https://pubmed.ncbi.nlm.nih.gov/41764994/) |
| Adult vertical-jump rating scale (non-primary; author states "based on my observations") | Men: excellent >70 cm, very good 61-70, above average 51-60, average 41-50, below average 31-40, poor 21-30. Women: excellent >60, very good 51-60, above avg 41-50, average 31-40, below avg 21-30, poor 11-20 cm | Topend Sports "Vertical Jump Height Norms" — [topendsports.com](https://www.topendsports.com/testing/norms/vertical-jump.htm) |
| Adult standing long jump rating scale (non-primary; "adapted from personal experience and various sources") | Men: excellent >250 cm, very good 241-250, above avg 231-240, average 221-230, below avg 211-220, poor 191-210. Women: excellent >200, average 171-180 cm | Topend Sports "Standing Long Jump Test" — [topendsports.com](https://www.topendsports.com/testing/tests/longjump.htm) |
| Standing long jump world record (non-primary) | 3.71 m (Arne Tvervaag, Norway); NFL Combine record 3.73 m (Byron Jones, 2015) | same Topend Sports page — [topendsports.com](https://www.topendsports.com/testing/tests/longjump.htm) |

Deceleration on foot (self-arrest)

| Quantity | Value | Source |
|---|---|---|
| Peak horizontal deceleration, university team-sport players (20 m sprint-to-stop) | -8.48 m/s^2 | Harper et al. 2022 review — [PMC9474351](https://pmc.ncbi.nlm.nih.gov/articles/PMC9474351/) |
| Peak deceleration, elite Australian-rules footballers from high-speed running | -5.3 m/s^2 | same |
| Peak deceleration, youth soccer players in a 505 change-of-direction test (fast group) | -11.02 m/s^2 | same |
| Average deceleration, university players | -4.44 m/s^2 overall; -3.88 early phase to -5.99 late phase | same |
| Time to stop / distance to stop from 7.35 m/s | 1.50 s / 6.86 m | same |
| Distance to stop across 5-20 m shuttle runs | 2.9-7.93 m | same |
| Peak resultant GRF in the first 50 ms of braking steps | 40-60 N/kg (about 4-6 body weights) | same |
| Early (first three steps) deceleration at 70 % vs 100 % of max sprint velocity | -4.25 +/- 0.79 m/s^2 vs -5.07 +/- 0.79 m/s^2 | Hitchens et al. (2026) "Early Horizontal Deceleration Ability Across Multiple Steps and Approach Speeds", Eur J Sport Sci, doi:10.1002/ejsc.70224 — [PMC13408429](https://pmc.ncbi.nlm.nih.gov/articles/PMC13408429/) (snippet only) |

Reaction time

| Quantity | Value | Source |
|---|---|---|
| Simple visual reaction time, 1,469 adults aged 18-65 | mean 231 ms (213 ms after hardware-delay correction); slows 0.55 ms/year of age | Woods DL, Wyma JM, Yund EW, Herron TJ, Reed B (2015) "Factors influencing the latency of simple reaction time", Front Hum Neurosci 9:131, doi:10.3389/fnhum.2015.00131 — [PMC4374455](https://pmc.ncbi.nlm.nih.gov/articles/PMC4374455/) |
| Replication sample, 189 adults 18-82 | 237.8 ms (220 ms corrected) | same |

### Inferences
- Bolt's 10 m-segment means (5.26 m/s in 0-10 m, 10.20 m/s in 10-20 m) imply he covers 0-20 m in about 2.9 s, i.e. a mean acceleration of roughly 3.5 m/s^2 over the first 20 m with the initial steps far higher (computed from Maćkała & Mero splits; the model paper that gives the instantaneous peak could not be read).
- For game tiers: "ordinary fit adult" sprint about 7-7.5 m/s (Harper's cohort), "elite human" 12.3 m/s (Bolt). Anything above 12.5 m/s is superhuman.
- Stopping distance with reaction: d = v*t_react + v^2/(2a). With t_react = 0.22 s and a = 5 m/s^2 (average braking) or 8.5 m/s^2 (peak), computed distances are: 5 m/s: 3.6 / 2.6 m; 7.35 m/s: 7.0 / 4.8 m; 10 m/s: 12.2 / 8.1 m; 12.34 m/s: 17.9 / 11.7 m; 15 m/s: 25.8 / 16.5 m; 20 m/s: 44.4 / 27.9 m; 30 m/s: 96.6 / 59.5 m. (Harper's measured 6.86 m from 7.35 m/s sits between the two model rows, validating the constants.) Human braking is friction- and strength-limited, so a superhuman runner who has not also unlocked a stronger "self-arrest" will run out of room in ordinary rooms and corridors.
- Jump tiers: 0.4 m CoM rise = fit adult; 0.7 m = elite; the design's "superhuman" tiers begin above roughly 1 m.

### Gaps
- No primary, population-based normative table for adult vertical jump or standing long jump was reached; the Topend Sports scales are self-declared observational. Recommended primary sources to pull: NFL Combine and NBA Combine datasets, and force-plate reference-value papers (see search terms).
- Bolt's instantaneous peak acceleration (often quoted near 9.5 m/s^2) was not verified; the Eur J Phys 2013 abstract carries no numbers.
- No measurement of an untrained adult's maximum sprint speed was reached.

## Key question 2: Falls — injury/fatality vs height, surface and orientation, landing forces and technique, bone fracture thresholds

### Takeaway
Fall mortality rises steeply above about 6 m and is near 50 % at roughly 10-15 m depending on the cohort (10.5 m with head/chest injury, 14.8-20 m in modern US trauma-centre data, 22 m without head/chest injury); hard surfaces roughly triple the odds of death and head- or trunk-first landings raise it 10-17-fold versus feet-first. Two-leg landing peak force follows PvGRF ≈ 0.49*sqrt(h_cm) + 0.37 body weights; parkour roll or precision technique cuts peak force by about 40 % and loading rate by 54-63 %; tibia axial fracture starts around 4-8 kN (50 % risk) and 7.5-11 kN outright.

### Cited Findings

Fatality vs height (clinical series)

| Study / cohort | Finding | Source |
|---|---|---|
| Taiwan trauma registry, 8,699 fall patients | Mortality 2.5 % (<1 m, n=7,001), 3.5 % (1-6 m, n=1,588), 5.5 % (>6 m, n=110); after adjusting for age, sex, comorbidity and ISS, falls >6 m had adjusted OR 10.0 (95 % CI 2.22-33.3) for death; >6 m is treated as "high-energy trauma" | Hsieh T-M et al. (2020) "Effect of Height of Fall on Mortality in Patients with Fall Accidents", Int J Environ Res Public Health 17(11):4163, doi:10.3390/ijerph17114163 — [PMC7312001](https://pmc.ncbi.nlm.nih.gov/articles/PMC7312001/) |
| Paris-region prehospital, 287 falls >3 m | 34 % died; median height 5.0 floors (dead) vs 2.0 floors (survivors), OR 1.24 per floor; hard surface OR 2.7 (39 % vs 22 %); first contact head 44 % mortality (OR 16.7), anterior trunk 57 % (OR 10.6), lateral 32 % (OR 11.1) vs feet-first | Lapostolle F et al. (2005) "Prognostic factors in victims of falls from height", Crit Care Med 33(6):1239-1242, doi:10.1097/01.CCM.0000164564.11989.C3 — [PubMed 15942337](https://pubmed.ncbi.nlm.nih.gov/15942337/) |
| London HEMS, 117 falls | 29 % died; mean height 9.9 m overall, 16.7 m in those who died; 50 % mortality estimated at 10.5 m with head and/or chest injury vs 22.4 m without | Dickinson A et al. (2012) "Falls from height: injury and mortality", J R Army Med Corps 158(2):123-127, doi:10.1136/jramc-158-02-11 — [PubMed 22860503](https://pubmed.ncbi.nlm.nih.gov/22860503/) |
| Milan trauma centre, 948 falls | 8.6 % mortality (5.2 % accidental, 20.5 % intentional); median height 3 m survivors vs 6 m non-survivors; death risk rises about 16 % per metre; intentional jumpers from low heights mostly land feet-first and had fewer critical head injuries | Casati A et al. (2020) "Falls from Height. Analysis of Predictors of Death", J Clin Med 9(10):3175, doi:10.3390/jcm9103175 — [PMC7601239](https://pmc.ncbi.nlm.nih.gov/articles/PMC7601239/) |
| Japan, 179 falls ≥15 y | Falls ≥5 m: survival OR 0.10; soft ground (soil/lawn) significantly lowered ISS; lateral and dorsal first-impact had survival OR 0.11 and 0.17 vs feet-first | Fujii M et al. (2021) "Factors influencing the injury severity score and the probability of survival in patients who fell from height", Sci Rep 11:15561, doi:10.1038/s41598-021-95226-w — [PMC8324820](https://pmc.ncbi.nlm.nih.gov/articles/PMC8324820/) |
| US trauma centre, 287 vertical-fall victims (snippet only; SAGE abstract page returned 403) | LD50 = 48 ft (14.8 m) with storeys standardised at 10 ft; 68 ft (20.1 m) if 12 ft per storey; falls from ≥8 storeys were 100 % fatal; authors note LD50 has risen versus older studies because of modern imaging, interventional radiology and critical care | Smith MR, Amberger MA, Pavalonis AG, Onursal EM, Lee S, Phillips PA, Edwards JS (2017) "Some patients live after trauma: Re-examining the lethality of vertical deceleration injuries", Trauma 19(4), doi:10.1177/1460408616689807 — [SAGE](https://journals.sagepub.com/doi/abs/10.1177/1460408616689807) |
| Case series of 137 free-fall survivors (out of ~12,000 falls collected) | Survived falls up to 275 ft (84 m) with calculated impact velocities up to 116 ft/s (35 m/s, 79 mph); impacts in all body-axis orientations; ages 1.5-91 | Snyder RG (1963) "Human survivability of extreme impacts in free-fall", FAA CARI Report 63-15 (PubMed 14131267) and "Human tolerances to extreme impacts in free-fall", Aerospace Med 34:695-709 (PubMed 14048676) — [FAA report page](https://www.faa.gov/data_research/research/med_humanfacs/oamtechreports/1960s/1963/196315) (PDF returned 403; numbers from the FAA/PubMed abstracts as surfaced in search) |

Landing ground reaction forces (GRF) vs drop height

| Condition | Peak vertical GRF | Source |
|---|---|---|
| Pooled model from 26 studies, two-leg drop landings 10-103 cm (1,000-1,200 Hz sampling) | PvGRF [BW] = 0.49*sqrt(DH_cm) + 0.37; at 60 cm individual studies ranged 2.38-4.91 BW; men > women; barefoot > shod; surface stiffness not significant | Niu W, Feng T, Jiang C, Zhang M (2014) "Peak Vertical Ground Reaction Force during Two-Leg Landing: A Systematic Review and Mathematical Modeling", BioMed Res Int 2014:126860, doi:10.1155/2014/126860 — [PMC4160626](https://pmc.ncbi.nlm.nih.gov/articles/PMC4160626/) |
| Gymnasts vs recreational athletes, 30/60/90 cm drops, second (main) peak | Gymnasts 27.1 / 40.3 / 56.0 N/kg (≈2.8 / 4.1 / 5.7 BW); recreational 21.5 / 27.0 / 37.4 N/kg (≈2.2 / 2.8 / 3.8 BW) | Seegmiller JG, McCaw ST (2003) "Ground Reaction Forces Among Gymnasts and Recreational Athletes in Drop Landings", J Athl Train 38(4):311-314 — [PMC314389](https://pmc.ncbi.nlm.nih.gov/articles/PMC314389/) |
| Gymnasts, 0.32 / 0.72 / 1.28 m drops (snippet only; as summarised in the Niu 2014 review) | 3.9 to 11 BW across heights | McNitt-Gray JL (1991) "Kinematics and impulse characteristics of drop landings from three heights", Int J Sport Biomech 7:201-224, as cited by Niu et al. 2014 — [Wiley](https://onlinelibrary.wiley.com/doi/10.1155/2014/126860) |
| Paratroopers, parachute landing fall with 2.3 m/s horizontal drift | 6.1 BW at 2.1 m/s vertical; 13.7 BW at 4.6 m/s vertical (≈1.08 m equivalent free fall) | Whitting JW, Steele JR, Jaffrey MA, Munro BJ (2007) "Parachute landing fall characteristics at three realistic vertical descent velocities", Aviat Space Environ Med 78(12):1135-1142, doi:10.3357/asem.2108.2007 — [PubMed 18064918](https://pubmed.ncbi.nlm.nih.gov/18064918/) |
| Army parachutists PLF, estimated peak vGRF (snippet only; page not opened) | Professionals 3,129 N vs amateurs 4,380 N; professionals pre-flex hips and knees before foot strike | "Relationship between angle and peak vertical ground reaction force estimation in parachute landing fall among army parachutists", Alexandria Engineering Journal (2021) — [ScienceDirect](https://www.sciencedirect.com/science/article/pii/S111001682100733X) |

Landing technique effect

| Technique (0.75 m drop, 10 male traceurs) | Peak vGRF | Loading rate | Reduction vs traditional | Source |
|---|---|---|---|---|
| Traditional two-foot landing | 5.2 BW | 154.3 +/- 96.3 BW/s | — | Puddle DL, Maulder PS (2013) "Ground Reaction Forces and Loading Rates Associated with Parkour and Traditional Drop Landing Techniques", J Sports Sci Med 12:122-129 — [JSSM](https://www.jssm.org/jssm-12-122.xml%3EFulltext) |
| Parkour precision landing | 3.2 BW | 83.3 +/- 80.1 BW/s | -38 % force, -54 % loading rate | same |
| Parkour roll | 2.9 BW | 64.1 +/- 59.8 BW/s | -43 % force, -63 % loading rate | same |
| Parkour landings from 0.9 / 1.8 / 2.7 m, four techniques (kinematics only) | Roll landing has the longest landing time and the smallest early change in vertical velocity; stiff landing the shortest time and largest velocity change ("may increase injury risk") | — | — | Dai B, Layer JS, Hinshaw TJ, Cook RF, Dufek JS (2020) "Kinematic Analyses of Parkour Landings from as High as 2.7 Meters", J Hum Kinet 72:15-28, doi:10.2478/hukin-2019-0123 — [PubMed 32269644](https://pubmed.ncbi.nlm.nih.gov/32269644/) |

Lower-limb bone fracture thresholds

| Structure / condition | Threshold | Source |
|---|---|---|
| Tibia, axial and combined axial+bending, 6 studies / 72 cadaver specimens | Fracture from about 7.5 kN (female specimens) to 11.3 kN (combined loading); fibula adds about 10 % to axial tolerance; multi-axial loading lowers tolerance | Zain Ul Abidin M et al. (2026) "Biomechanical Fracture Thresholds of the Tibia and Fibula Under Axial and Multi-axial Loading: A Systematic Review", Cureus 18(1):e102362, doi:10.7759/cureus.102362 — [PubMed 41769460](https://pubmed.ncbi.nlm.nih.gov/41769460/) |
| Foot/ankle complex under blunt axial impact, 43 isolated legs, Weibull survival model | Axial tibial force at 50 % fracture risk: 3.7 kN (65-year-old 5th-percentile female) to 8.3 kN (45-year-old 50th-percentile male), no Achilles tension; primary fracture mode calcaneus, then distal tibia | Funk JR et al. (2002) "The axial injury tolerance of the human foot/ankle complex and the effect of Achilles tension", J Biomech Eng 124(6):750-757, doi:10.1115/1.1514675 — [PubMed 12596644](https://pubmed.ncbi.nlm.nih.gov/12596644/) |
| Tibia off-axis impact (snippet only; Springer page redirected to login) | Mean fracture force 7.5 kN at 15 deg off-axis vs 5.8 kN at 30 deg | "The Injury Tolerance of the Tibia Under Off-Axis Impact Loading", Ann Biomed Eng (2017), doi:10.1007/s10439-017-1824-6 — [Springer](https://link.springer.com/article/10.1007/s10439-017-1824-6) |
| Femur, regulatory axial limit for the 50th-percentile male crash dummy | 2,250 lbf (10.0 kN) per upper leg | 49 CFR 571.208 S6.5 (FMVSS 208), 2023 edition — [govinfo XML](https://www.govinfo.gov/content/pkg/CFR-2023-title49-vol6/xml/CFR-2023-title49-vol6-sec571-208.xml) |

### Inferences
- Free-fall impact speed v = sqrt(2 g h) (computed): 1 m → 4.4 m/s; 2 m → 6.3; 3 m → 7.7; 4 m → 8.9; 5 m → 9.9; 6 m → 10.9; 10 m → 14.0; 12 m → 15.3; 15 m → 17.2; 20 m → 19.8; 26 m → 22.6 m/s. Because the pedestrian curves below are expressed in impact speed, a 10 m fall (14 m/s, 50 km/h) and being hit by a car at 50 km/h are the same order of energy, and the clinical fall data (50 % death near 10-15 m) agree with the pedestrian 50 % death speed (40.6 mph = 65 km/h = 18 m/s, i.e. a 16.6 m fall) to within a factor that plausibly reflects body orientation and surface.
- Extrapolating Niu's square-root fit beyond its 1.03 m data range (flagged as extrapolation): 2 m → 7.3 BW, 3 m → 8.9 BW, 5 m → 11.3 BW, 10 m → 15.9 BW. With a 70 kg body that is 5.0 kN at 2 m and 7.8 kN at 5 m spread over two legs, which is why 5-6 m is where fracture (Funk 50 % risk 3.7-8.3 kN per tibia) and mortality (Hsieh >6 m OR 10) both start climbing.
- A rigid "no-technique" landing from height h that stops the centre of mass over s = 0.5 m of knee flexion gives mean deceleration a = g*h/s: 1 m → 2 g (2.1 kN on 70 kg), 3 m → 6 g (4.8 kN), 5 m → 10 g (7.6 kN), 10 m → 20 g (14.4 kN), 20 m → 40 g (28 kN). Two tibiae at about 8 kN each are exceeded somewhere between 10 and 20 m even with perfect symmetric loading; real peak forces are 1.5-2.5x the mean.
- Technique multipliers for the game: roll/precision landing -40 % peak force, -55 to -65 % loading rate (Puddle & Maulder); a hard surface roughly x2.7 odds of death vs soft (Lapostolle); head-first x16.7 odds vs feet-first (Lapostolle); dorsal/lateral impact cuts survival odds to 0.11-0.17 (Fujii).
- Gating logic supported by the data: unlock jump height first, but make landings above ~3 m (7.7 m/s) risk fractures and above ~6 m (10.9 m/s) risk death unless the player rolls, lands on a soft surface, or has an "impact tolerance" tier that raises the effective tibia/femur limits.

### Gaps
- The classic "LD50 about 4 storeys" figure (usually attributed to Warner & Demling 1986, J Trauma) was not verified from a primary source; the modern LD50 (14.8 m, Smith 2017) is quoted from snippets because the SAGE page was blocked.
- Whole-femur axial fracture force from cadaver impact tests (Kress 1995; Yamada 1970) was not reached; only the FMVSS 208 regulatory 10 kN limit is cited. The "4,000 N femur" figure circulating online (arhfoundation.org) is not primary and was excluded.
- McNitt-Gray 1991 could not be located in PubMed (Int J Sport Biomech is not indexed); its 3.9-11 BW figures come from the Niu 2014 review.
- No quantitative GRF for judo breakfalls (ukemi) was found.

## Key question 3: Frontal impacts — whole-body g tolerance, head (HIC, concussion), chest, and pedestrian-speed curves as a proxy for running into a wall

### Takeaway
Restrained, well-supported humans have survived 46 g (voluntary, 1.4 s stop) and a 214 g peak (crash, with multiple fractures); regulatory head/chest limits are HIC15 ≤ 700 (about 5 % risk of AIS 4+), chest deflection ≤ 63 mm and chest acceleration ≤ 60 g; concussions in football cluster around 95-105 g and 5,600-7,200 rad/s^2. Rosén & Sander's curve gives death risk P = 1/(1+exp(6.9-0.090 v_kmh)): 1.5 % at 30 km/h, 8 % at 50, 35 % at 70, 57 % at 80, 89 % at 100 km/h; Tefft's US curve reaches 50 % death at 40.6 mph (65 km/h).

### Cited Findings

Whole-body deceleration

| Case | Value | Source |
|---|---|---|
| Col. John P. Stapp, Sonic Wind No. 1 rocket sled, 10 Dec 1954 | 632 mph reached in 5 s; stopped in 1.4 s; peak 46.2 g, "more than anyone had yet endured voluntarily"; near-total retinal capillary haemorrhage, vision partly recovered next day; earlier runs (1947-48) up to 35 g | New Mexico Museum of Space History, "John P. Stapp" — [nmspacemuseum.org](https://nmspacemuseum.org/inductee/john-p-stapp/); Smithsonian NASM (2017) "Record-Breaking Rocket Sled Created Modern Safety Standards" (632 mph, 1.4 s; broke ribs and wrists in the programme) — [airandspace.si.edu](https://airandspace.si.edu/stories/editorial/record-breaking-rocket-sled-created-modern-safety-standards) |
| Kenny Bräck, IndyCar crash, Texas Motor Speedway, 12 Oct 2003 | 214 g peak recorded by in-car crash recorder at about 220 mph; fractures of right femur, sternum, lumbar vertebra and ankles; survived | Guinness World Records, "Highest g force endured - non-voluntary" — [guinnessworldrecords.com](https://www.guinnessworldrecords.com/world-records/67617-highest-g-force-endured-non-voluntary) |

Regulatory injury criteria (Hybrid III 50th-percentile male, FMVSS 208)

| Criterion | Limit | Source |
|---|---|---|
| HIC36 (36 ms window) | ≤ 1,000 | 49 CFR 571.208 S6.2(a) — [govinfo XML](https://www.govinfo.gov/content/pkg/CFR-2023-title49-vol6/xml/CFR-2023-title49-vol6-sec571-208.xml) |
| HIC15 (15 ms window) | ≤ 700 | S6.2(b), same |
| Chest resultant acceleration | ≤ 60 g except intervals totalling ≤ 3 ms | S6.3, same |
| Chest (sternum-to-spine) compression | ≤ 63 mm (2.5 in) [option (b)]; ≤ 76 mm (3.0 in) [option (a)] | S6.4, same |
| Femur axial force | ≤ 2,250 lbf (10 kN) | S6.5, same |
| Neck Nij critical values | Fzc 6,806 N tension / 6,160 N compression; Myc 310 N m flexion / 135 N m extension; Nij ≤ 1.0 | S6.6, same |
| HIC-to-injury probability | HIC15 = 700 ≈ 5 % probability of AIS 4+ head injury; HIC36 = 1,000 ≈ 17 % AIS 4+; HIC36 = 1,500 ≈ 55 % (Prasad-Mertz sigmoid curves) | "Head Injury Criterion: Mini Review", Am J Biomed Sci & Res (2019) — [biomedgrid.com](https://biomedgrid.com/fulltext/volume5/head-injury-criterion-mini-review.000957.php) (NHTSA rev_criteria.pdf, the primary, returned 403) |

Concussion (head kinematics)

| Population | Concussive impact kinematics | Source |
|---|---|---|
| NFL reconstructions (Hybrid III) | 98 +/- 28 g linear, 6,432 +/- 1,813 rad/s^2 rotational | Rowson S, Duma SM (2013) "Brain Injury Prediction: Assessing the Combined Probability of Concussion Using Linear and Rotational Head Acceleration", Ann Biomed Eng 41(5):873-882, doi:10.1007/s10439-012-0731-0 — [PMC3624001](https://pmc.ncbi.nlm.nih.gov/articles/PMC3624001/) |
| Instrumented college players (HITS), 63,011 impacts, 37 concussions | 104 +/- 30 g, 4,726 +/- 1,931 rad/s^2; combined probability CP = 1/(1+exp(-(-10.2 + 0.0433 a + 0.000873 alpha - 0.00000092 a*alpha))) with a in g, alpha in rad/s^2 | same |
| High-school football, 54,247 impacts, 13 concussions | 105.0 +/- 18.0 g, 7,229.5 +/- 1,157.6 rad/s^2; CART thresholds >96.1 g, >5,582.3 rad/s^2 and front/top/back location → 13.4 % concussion chance | Broglio SP et al. (2010) "The Biomechanical Properties of Concussions in High School Football", Med Sci Sports Exerc 42(11):2064-2071, doi:10.1249/MSS.0b013e3181dd9156 — [PMC2943536](https://pmc.ncbi.nlm.nih.gov/articles/PMC2943536/) |
| NFL struck players, 25 reconstructions | 94 +/- 28 g, 6,432 +/- 1,813 rad/s^2, head delta-V 7.2 +/- 1.8 m/s; neck tension 1,704 +/- 432 N at 20 ms; HIC ∝ deltaV^4 / d^1.5 | Viano DC, Casson IR, Pellman EJ (2007) "Concussion in professional football: biomechanics of the struck player - part 14", Neurosurgery 61(2):313-327, doi:10.1227/01.NEU.0000279969.02685.D0 — [PubMed 17762744](https://pubmed.ncbi.nlm.nih.gov/17762744/) |
| Youth vs adult concussion tolerance (snippet only) | Youth 62.4 +/- 29.7 g and 2,609 +/- 1,591 rad/s^2 vs adults 102.5 +/- 32.7 g and 4,412 +/- 2,326 rad/s^2 | "Development of a Concussion Risk Function for a Youth Population" — [PMC6928097](https://pmc.ncbi.nlm.nih.gov/articles/PMC6928097/) |
| Wayne State FE-model thresholds (snippet only; abstract has no numbers) | Commonly quoted 25/50/80 % MTBI thresholds 66 g / 4,600 rad/s^2, 82 g / 5,900 rad/s^2, 106 g / 7,900 rad/s^2 | Zhang L, Yang KH, King AI (2004) "A proposed injury threshold for mild traumatic brain injury", J Biomech Eng 126(2):226-236, doi:10.1115/1.1691446 — [PubMed 15179853](https://pubmed.ncbi.nlm.nih.gov/15179853/); numbers as repeated in secondary search text, not verified in the paper |
| Olympic boxers' straight punch to Hybrid III face | Head 58 +/- 13 g, 6,343 +/- 1,789 rad/s^2; neck shear 994 N | Walilko TJ, Viano DC, Bir CA (2005) Br J Sports Med 39(10):710-719, doi:10.1136/bjsm.2004.014126 — [PubMed 16183766](https://pubmed.ncbi.nlm.nih.gov/16183766/) |

Pedestrian fatality/severe-injury risk vs impact speed (proxy for running into a rigid wall)

| Speed | Rosén & Sander P(death), adults ≥15 y hit by car front (computed from eq. 2) | Tefft severe injury (AIS 4+) | Tefft death |
|---|---|---|---|
| 30 km/h (8.3 m/s, 18.6 mph) | 1.5 % | — | — |
| 40 km/h (11.1 m/s, 24.9 mph) | 3.6 % | 25 % at 24.9 mph | 10 % at 24.1 mph |
| 50 km/h (13.9 m/s, 31.1 mph) | 8.3 % | — | — |
| 53 km/h (33.0 mph) | — | 50 % | — |
| 60 km/h (16.7 m/s, 37.3 mph) | 18.2 % | — | — |
| 65 km/h (40.6-40.8 mph) | — | 75 % at 40.8 mph | 50 % at 40.6 mph |
| 70 km/h (19.4 m/s, 43.5 mph) | 35.4 % | — | — |
| 77 km/h (48.0-48.1 mph) | — | 90 % at 48.1 mph | 75 % at 48.0 mph |
| 80 km/h (22.2 m/s, 49.7 mph) | 57.4 % | — | — |
| 88 km/h (54.6 mph) | — | — | 90 % |
| 100 km/h (27.8 m/s, 62 mph) | 89.1 % | — | — |

- Rosén & Sander: GIDAS 1999-2007, 490 adult pedestrians (36 fatal), weighted logistic regression; P(v) = 1/(1 + exp(6.9 - 0.090 v)), v in km/h (95 % CI on intercept 5.3-8.5, slope 0.060-0.12); age model P(v, age) = 1/(1 + exp(9.1 - 0.095 v - 0.040 age)); risk at 50 km/h is >2x that at 40 km/h and >5x that at 30 km/h; median impact speed of fatalities 57 km/h; at 75 km/h the estimated risk is about 46 % (computed) — Rosén E, Sander U (2009) "Pedestrian fatality risk as a function of car impact speed", Accid Anal Prev 41(3):536-542, doi:10.1016/j.aap.2009.02.002 — [author preprint via NACTO](https://nacto.org/wp-content/uploads/pedestrian_fatality_risk_function_car_impact_speed_rosen.pdf); abstract [PubMed 19393804](https://pubmed.ncbi.nlm.nih.gov/19393804/).
- Tefft: US PCDS 1994-1998 data, weighted and standardised to 2007-2009; AIS 4+ risk 10/25/50/75/90 % at 17.1/24.9/33.0/40.8/48.1 mph; death 10/25/50/75/90 % at 24.1/32.5/40.6/48.0/54.6 mph; a 70-year-old at speed v has the death risk of a 30-year-old at v + 11.8 mph — Tefft BC (2013) "Impact speed and a pedestrian's risk of severe injury or death", Accid Anal Prev 50:871-878, doi:10.1016/j.aap.2012.07.022 — [PubMed 22935347](https://pubmed.ncbi.nlm.nih.gov/22935347/); rounded values also on the [AAA Foundation summary](https://aaafoundation.org/impact-speed-pedestrians-risk-severe-injury-death/).
- Earlier, biased curves (for comparison only): Anderson 1997 8/85/100 % at 30/50/70 km/h; Ashton 1982 5/45/95 %; Pasanen 1992 6/40/94 %; Hannawald & Kauer 2004 4/14/39 % — Table 1 of Rosén & Sander 2009 — [NACTO preprint](https://nacto.org/wp-content/uploads/pedestrian_fatality_risk_function_car_impact_speed_rosen.pdf).

### Inferences
- A car-front strike is a softer, more compliant impact than a flat rigid wall (the pedestrian is thrown rather than stopped), so pedestrian curves are a lower bound on the lethality of running head-on into concrete at the same speed; but they are the best-populated speed-vs-death dataset available and are already in units the game uses (m/s).
- Mean deceleration when a runner stops against a wall over s metres of body compression, a = v^2/(2 s) (computed): at 7.35 m/s, 27.5 g over 0.1 m (rigid, chest first; 18.9 kN on 70 kg; 27 ms) or 9.2 g over 0.3 m (arms-first "crumple"; 6.3 kN; 82 ms). At 12.34 m/s: 78 g / 53 kN over 0.1 m, 26 g / 18 kN over 0.3 m. At 20 m/s: 204 g over 0.1 m, 68 g over 0.3 m. Comparing with Stapp's 46 g (survivable, restrained, 1.4 s) and the 60 g chest and 10 kN femur limits, an ordinary human sprinting into a wall at 7-8 m/s is at fracture level if the impact is rigid and at bruising level if arms absorb it; at Bolt speed the rigid case is far past every regulatory limit.
- Concussion from a wall: with a head stopping over ~2 cm of scalp/skull compliance from 7.35 m/s, a = 1,350 m/s^2 = 138 g (computed), i.e. above the 96-105 g concussion cluster; so unprotected head-first wall contact at sprint speed should concuss by default, and helmets/"hardened skull" tiers can be modelled by lengthening the stopping distance.
- Rowson-Duma CP (computed): 82 g & 5,900 rad/s^2 → 12.5 %; 98 g & 6,432 → 28.5 %; 105 g & 7,229 → 49 %; 120 g & 8,000 → 75 %; 150 g & 10,000 → 97.5 %. Useful as a smooth probability for "dazed/knocked out" states.

### Gaps
- The NHTSA report that defines the HIC-to-AIS curves, chest-deflection risk curves and Nij derivations was not accessible; probabilities are from a secondary mini-review.
- Chest deflection vs rib-fracture probability (Kroell/Mertz curves) was not retrieved.
- Zhang 2004's 66/82/106 g thresholds are quoted from secondary text, not from the paper.
- No study of humans running into fixed barriers exists in the literature reached here; the pedestrian data is a proxy, as the brief expected.

## Key question 4: Grabbing and hanging — grip and finger strength, hang time, fall-arrest forces, shoulder/tendon limits, catching a ledge at speed

### Takeaway
Adult grip maxima are about 480 N (men) / 290 N (women) per hand; experienced climbers hang two-handed for about 77 s on average; A2 finger pulleys see ~3x fingertip force in a crimp and fail near 400 N; climbing falls on dynamic rope put 2.5-4 kN on the climber (EN 892 caps rope impact force at 12 kN; OSHA caps harness arrest force at 8 kN). Catching a ledge is therefore feasible only at low downward speed: a 70 kg body arriving at 3 m/s needs about 1.3-1.7 kN average from both arms, already near two maximal grips; at 5 m/s (1.3 m of free fall) it needs 2.4-3.6 kN.

### Cited Findings

Grip and finger strength

| Quantity | Value | Source |
|---|---|---|
| Maximum grip strength, healthy adults 20-40 y (NIH Toolbox norming sample, Jamar Plus, n=558) | Men 108.0 +/- 22.6 lb (≈480 N); women 65.8 +/- 14.6 lb (≈293 N) | Bohannon RW, Magasi S (2015) "Identification of dynapenia in older adults through the use of grip strength t-scores", Muscle Nerve 51(1):102-105, doi:10.1002/mus.24264 — [PubMed 24729356](https://pubmed.ncbi.nlm.nih.gov/24729356/) |
| Life-course grip centiles, 49,964 British participants | Centile curves ages 4-90 (peak in early adulthood; values not extracted here) | Dodds RM et al. (2014) "Grip strength across the life course: normative data from twelve British studies", PLOS ONE 9(12):e113637 — [PubMed 25474696](https://pubmed.ncbi.nlm.nih.gov/25474696/) |
| Climbers (recreational to elite, n=36) baseline before a 24 h event | Two-handed hang time 76.8 +/- 32.6 s; grip 99.2 +/- 25.5 lb right (≈441 N), 97.0 +/- 23.3 lb left | Yu E, Lowe J, Millon J, Tran K, Coffey C (2023) "Change in grip strength, hang time, and knot tying speed after 24 hours of endurance rock climbing", Front Sports Act Living 5:1224581, doi:10.3389/fspor.2023.1224581 — [PMC10433161](https://pmc.ncbi.nlm.nih.gov/articles/PMC10433161/) |
| Physiology of hard climbing (review) | Climbers have unremarkable absolute strength but high strength-to-mass ratio; finger-flexor maximal strength and local endurance are the two most predictive components; handgrip endurance drops more than grip strength with severe climbing; VO2max 52-55 ml/kg/min | Watts PB (2004) "Physiology of difficult rock climbing", Eur J Appl Physiol 91(4):361-372, doi:10.1007/s00421-003-1036-7 — [PubMed 14985990](https://pubmed.ncbi.nlm.nih.gov/14985990/) |
| Crimp grip, in vivo, 16 fingers of 4 climbers | Flexor tendon bowstringing over the A2 pulley loaded at about 3x the fingertip force; up to 116 N measured over the A2 pulley; bowstringing distance +0.6 mm (30 %) after warm-up | Schweizer A (2001) "Biomechanical properties of the crimp grip position in rock climbers", J Biomech 34(2):217-223, doi:10.1016/S0021-9290(00)00184-6 — [PubMed 11165286](https://pubmed.ncbi.nlm.nih.gov/11165286/) |
| A2 pulley ultimate strength (snippet only; secondary climbing-medicine sites citing cadaver work) | ≈407 N; recreational climbers' finger-flexor force can exceed 400 N, hence pulley ruptures; A2 load computed 36x higher in crimp than slope grip | Schweizer A (2008) "Biomechanics of the interaction of finger flexor tendons and pulleys in rock climbing", Sports Technology 1(6), doi:10.1002/jst.68 — [Wiley](https://onlinelibrary.wiley.com/doi/full/10.1002/jst.68); The Climbing Doctor summary — [theclimbingdoctor.com](https://theclimbingdoctor.com/climbing-pulley-injury-anatomy-biomechanics-and-research/) |

Fall-arrest forces (what a body can be stopped with)

| Scenario | Force | Source |
|---|---|---|
| Real lead-climbing falls, 80 kg climber and belayer, 9.2 mm dynamic rope | Fall factor 0.3 (2 m fall): 2.5 kN on climber, 1.5 kN belayer, 4 kN anchor. FF 0.7 (2 m): 3 / 2 / 5 kN. FF 1.0 (3.6 m): 4 / 2 / 6 kN; FF 1.0 described as "impressive" but "quite bearable" | Petzl, "Forces at work in a real fall" — [petzl.com](https://www.petzl.com/US/en/Sport/Forces-at-work-in-a-real-fall) |
| Fall factor definition | f = fall length / rope length (0-2 in climbing); higher factor → higher force | Petzl, "Fall factor and impact force - theory" — [petzl.com](https://www.petzl.com/US/en/Sport/Fall-factor-and-impact-force---theory) |
| Dynamic rope standard (EN 892 / UIAA 101) | 80 kg mass (55 kg for half ropes), fall factor 1.77 on a fixed point; maximum impact force 12 kN single rope, 8 kN half rope, 12 kN twin (two strands) | Petzl, "Impact force - standards" — [petzl.com](https://www.petzl.com/US/en/Sport/Impact-force---standards) |
| Occupational fall-arrest limits | Max arresting force 1,800 lb (8 kN) with harness, 900 lb (4 kN) with body belt (belts banned for arrest since 1998); free fall ≤ 6 ft (1.8 m); deceleration distance ≤ 3.5 ft (1.07 m); anchorage ≥ 5,000 lb (22.2 kN) | OSHA 29 CFR 1926.502(d) — [osha.gov](https://www.osha.gov/laws-regs/regulations/standardnumber/1926/1926.502) |

Shoulder and tendon limits

| Quantity | Value | Source |
|---|---|---|
| Force to dislocate an intact cadaveric shoulder anteriorly, arm at 90 deg abduction and maximal external rotation, 14 shoulders (no muscle tone) | 123.57 N average; 325.7 N after classical Latarjet, 327.1 N after congruent-arc Latarjet | Prinja A, Raymond A, Pimple M (2020) "A Biomechanical Comparison of Two Techniques of Latarjet Procedure in Cadaveric Shoulders", Adv Orthop 2020:7496492, doi:10.1155/2020/7496492 — [PMC7077050](https://pmc.ncbi.nlm.nih.gov/articles/PMC7077050/) |
| Rotator-cuff (infraspinatus) suture-repair construct ultimate load, cadaver | 294.6 +/- 84.1 N (2 mattress stitches) vs 519.4 +/- 150.8 N with patch augmentation | Hochreiter B et al. (2026) JSES Rev Rep Tech 6(4):100844, doi:10.1016/j.xrrt.2026.100844 — [PubMed 42751040](https://pubmed.ncbi.nlm.nih.gov/42751040/) (repair constructs, not intact tendon) |

### Inferences
- Ledge catch model (computed): arms bring the body to rest over d metres of shoulder/elbow give; average force F = m v^2/(2 d) + m g. For 70 kg: v = 2 m/s (0.2 m drop) → 0.97-1.15 kN (1.4-1.7 BW); 3 m/s (0.46 m) → 1.3-1.7 kN; 4 m/s (0.8 m) → 1.8-2.6 kN; 5 m/s (1.3 m) → 2.4-3.6 kN; 7 m/s (2.5 m) → 4.1-6.4 kN; 10 m/s (5.1 m) → 7.7-12.4 kN (ranges are d = 0.5 m to 0.3 m). Two maximal adult grips total about 0.9-1.0 kN (Bohannon), and eccentric arm/shoulder capacity is on the order of 1-2 BW for trained people, so the human ceiling for a ledge catch is roughly 3-4 m/s of downward velocity (a drop of about 0.5-0.8 m below the ledge). Above that the hands slip or the pulleys/shoulders fail. The Petzl real-fall data (2.5-4 kN "bearable" through a harness) and the OSHA 8 kN harness cap show that a whole-body harness can tolerate 3-8x what hands can hold.
- Finger-pulley failure (~400 N per finger, 3x fingertip load in crimp) is the first tissue limit for crimped ledges, before grip or shoulder; a design "grip tier" can raise this.
- A hang timer of ~75 s (two hands) is a reasonable baseline stamina; halve it for one arm as a placeholder until a one-arm hang source is found.

### Gaps
- Intact supraspinatus/biceps tendon ultimate loads and glenohumeral dislocation force with active muscle were not found; the 123.57 N cadaver figure is for a passive joint in the most vulnerable position and grossly understates in-vivo tolerance.
- No study measured hand/arm forces in a parkour "cat leap/arm jump"; the ledge-catch table is derived, not measured.
- No one-arm hang time norm was found.

## Key question 5: Strength — breaking building materials, hand-bone limits when punching, measured punch forces

### Takeaway
Elite boxers deliver 3.4-4.1 kN peak punch force at 8.9-9.1 m/s with an effective mass of about 3 kg (roughly 120-140 J per punch). Half-inch gypsum board fails at 110 lbf (489 N) in flexure and is penetrated by a 22.7 kg bag dropped 300 mm (≈67 J); a loadbearing concrete block must exceed 13.8 MPa net-area compressive strength and extruded clay brick averages 78 MPa, so unaided human punches break drywall and hollow doors but not masonry.

### Cited Findings

Punch mechanics

| Quantity | Value | Source |
|---|---|---|
| 7 Olympic boxers, 18 straight punches to Hybrid III face | Punch force 3,427 +/- 811 N; hand velocity 9.14 +/- 2.06 m/s; effective punch mass 2.9 +/- 2.0 kg; jaw load 876 N; force rises with weight class via effective mass | Walilko, Viano, Bir (2005) — [PubMed 16183766](https://pubmed.ncbi.nlm.nih.gov/16183766/) |
| Frank Bruno (pro heavyweight) on ballistic pendulum | Punch travelled 0.49 m in 0.1 s; impact velocity 8.9 m/s; peak force 4,096 N within 14 ms; equivalent to up to 6,320 N on a human head; target head acceleration 520 m/s^2 (53 g); "equivalent to a 6 kg padded mallet at 20 mph" | Atha J, Yeadon MR, Sandover J, Parsons KC (1985) "The damaging punch", Br Med J 291(6511):1756-1757, doi:10.1136/bmj.291.6511.1756 — [PubMed 3936571](https://pubmed.ncbi.nlm.nih.gov/3936571/) |
| Punching mechanism share of hand injuries | Altercations involving punching = 18.5 % of all hand injuries (case report introduction) | Lucas JM et al. (2022) J Orthop Case Rep 12(12):21-24 — [PubMed 37056592](https://pubmed.ncbi.nlm.nih.gov/37056592/) |
| Metacarpal shaft fixation constructs (cadaver, three-point bending) — proxy only, not intact bone | Load to failure 365 +/- 80 N (intramedullary nail) vs 598 +/- 373 N (plate); cyclic 70 N (grasp) and 120 N (tip pinch) | Allen AD et al. (2026) J Hand Surg Glob Online 8(2):100925, doi:10.1016/j.jhsg.2025.100925 — [PubMed 41584052](https://pubmed.ncbi.nlm.nih.gov/41584052/) |

Building materials

| Material | Property | Value | Source |
|---|---|---|---|
| Gypsum board 1/2 in (12.7 mm), regular/Type X | Flexural strength minimum (ASTM C473 Method A), bearing edges perpendicular / parallel to length | 110 lbf (489 N) / 40 lbf (178 N) | Gypsum Association GA-235-2019 "Gypsum Board Typical Mechanical and Physical Properties" — [americangypsum.com PDF](https://www.americangypsum.com/sites/default/files/2022-01/ga-235_gypsum_board_typical_mechanical_and_physical_properties.pdf) (text extracted locally) |
| Gypsum board 5/8 in (15.9 mm) | Flexural minimum perpendicular / parallel | 150 lbf (667 N) / 50 lbf (222 N) | same |
| Gypsum board | Core/end/edge hardness minimum | 15 lbf (67 N) Method A | same |
| Gypsum board 1/2 in | Nail-pull resistance minimum | 80 lbf (356 N) | same |
| Gypsum board 1/2 in regular | Compressive strength (typical) | 350 psi (2,400 kPa) | same |
| Gypsum board, soft-body impact (ASTM E695, 22.7 kg leather bag, wood studs 16 in o.c.) | Drop height at penetration: 1/2 in regular 12 in (300 mm); 1/2 in Type X 24 in (610 mm); 5/8 in Type X 30 in (760 mm); two layers 30-72 in | ≈67 J, ≈136 J, ≈169 J of impact energy respectively (computed as m g h) | same |
| Gypsum board 1/2 in | Effective modulus of rupture (from ASTM C1396) | 750 psi (5.2 MPa) machine direction, 260 psi (1.8 MPa) cross | same |
| Concrete masonry unit (ASTM C90 loadbearing) | Minimum net-area compressive strength | 2,000 psi (13.8 MPa) | CMHA CMU-TEC-001-23 "Concrete Masonry Unit Shapes, Sizes, Properties, and Specifications" — [cmha.org](https://www.cmha.org/resource/cmu-tec-001/) |
| Clay brick, extruded solid | Mean unit compressive strength | 11,305 psi (77.9 MPa), SD 4,464 psi (30.8 MPa); molded brick 11,258 psi (77.6 MPa) | Brick Industry Association Technical Note 3A "Brick Masonry Material Properties" (2024) — [gobrick.com PDF](https://www.gobrick.com/media/file/TN_3A_Brick_Masonry_Material_Properties.pdf) (text extracted locally) |
| Masonry grout (ASTM C476) | Minimum compressive strength | 2,000 psi (13.8 MPa) | same |
| Karate strike on brick/concrete (non-primary) | Hand force ≈3,000 N (675 lbf) quoted from physics-of-karate work | Boing Boing "The physics of breaking stuff with your fists" (2010) — [boingboing.net](https://boingboing.net/2010/09/03/the-physics-of-break.html); primary is Feld MS, McNair RE, Wilk SR (1979) "The Physics of Karate", Scientific American 240(4):150-158 (not opened) |

### Inferences
- Punch energy (computed from Walilko): 0.5 x 2.9 kg x 9.14^2 ≈ 121 J; a full-body 70 kg runner at 7.35 m/s carries 1,891 J and at 12.34 m/s 5,330 J, i.e. 15-45 boxers' punches — this is why shoulder-charging drywall works and why a wall stops a sprinter so violently.
- Drywall gating: 1/2 in board fails in flexure at about 0.5 kN or under a ~67 J soft impact (300 mm drop of 22.7 kg), both well below one boxer's punch (3.4 kN, 121 J); a human should punch through drywall at tier 0, consistent with everyday experience. Two layers of 5/8 in Type X need ≈400 J (72 in x 22.7 kg) — a shoulder charge at 3.4 m/s for 70 kg.
- Masonry gating: a 2,000 psi (13.8 MPa) CMU face shell of ~25 mm x 200 mm bearing area would need >69 kN in compression; brick at 78 MPa is far beyond any human strike (peak 3-4 kN, ~6 kN on a rigid head-mass). Breaking a brick wall therefore belongs to a clearly superhuman tier (roughly 20-50x human punch force), and the hand must be gated separately because metacarpal constructs fail at 0.4-0.6 kN in bending and boxers already fracture their hands at 3-4 kN peak impacts.
- Wooden interior doors: no primary force data found (see Gaps); treat as between drywall and CMU.

### Gaps
- No primary cadaver study giving the force to fracture an intact fifth metacarpal (boxer's fracture threshold) was found; only fixation-construct data (365-598 N in bending) as a proxy.
- No test standard data for forced entry of wooden doors (ASTM F476 / F1233 impact energies) was reached.
- Feld et al. 1979 (Scientific American) on karate strike force was not opened; the 3 kN figure is from a blog.

## Key question 6: Scaling — square-cube law, why superhuman speed/jump requires superhuman bone and tendon, kinetic energy at speed

### Takeaway
Under geometric similarity muscle force scales with L^2 and mass with L^3 (square-cube law), so Hill (1950) predicted all similar animals jump the same height and run the same speed; Bobbert's 2013 simulation shows small bodies actually jump lower because of force-velocity effects (82 kg human 40 cm → mouse-lemur size 6 cm). For a fixed-size human made "stronger", takeoff and landing energies scale linearly with jump height and quadratically with speed, while bone and tendon limits stay fixed, so durability must be gated separately from power.

### Cited Findings
- Square-cube law: when linear size multiplies by k, area grows k^2 and volume/mass k^3; muscle strength follows cross-sectional area while weight follows volume, so isometrically enlarged animals become relatively weaker; primary statements in Galileo, Two New Sciences (1638) and Haldane, "On Being the Right Size" (1928) — [Wikipedia pointer](https://en.wikipedia.org/wiki/Square%E2%80%93cube_law).
- Hill (1950) argued from dimensional analysis that geometrically similar animals of different size should run/swim at the same linear speed and jump the same height, assuming constant tissue stress and safety factor; jump-height independence of size is "widely accepted" in Hill 1950, McMahon & Bonner 1983 and others — Scholz MN et al. (2006) "Scaling and jumping: gravity loses grip on small jumpers", J Theor Biol, abstract — [ScienceDirect](https://www.sciencedirect.com/science/article/abs/pii/S0022519305004674) (snippet only); Hill AV (1950) "The dimensions of animals and their muscular dynamics", Science Progress 38:209-230 (not opened; cited via Bobbert 2013 and Scholz 2006).
- Bobbert (2013): classical theory says "jump height does not depend on body size per se" because work per kg of muscle is size-independent; but scaling an 82 kg human model isometrically down to 0.1 kg cut jump height from 40 cm to 6 cm, because smaller animals must move joints faster (force-velocity limit) and have less time to develop active state; peak power fell from 49 to about 25 W/kg — Bobbert MF (2013) PLOS ONE 8(8):e71209 — [PLOS](https://journals.plos.org/plosone/article?id=10.1371/journal.pone.0071209).
- Larger frogs jump farther than Hill predicts, so the same-height rule is only approximate even within a taxon — Scholz et al. 2006 abstract — [ScienceDirect](https://www.sciencedirect.com/science/article/abs/pii/S0022519305004674) (snippet only).
- Textbook treatments of scaling in locomotion, cited in the brief and by the above papers (not opened in this session): McMahon TA (1984) Muscles, Reflexes, and Locomotion, Princeton University Press; McMahon TA, Bonner JT (1983) On Size and Life, Scientific American Library; Alexander RMcN (2003) Principles of Animal Locomotion, Princeton University Press.
- Tissue limits that stay fixed when "power" is buffed: tibia 7.5-11.3 kN (Cureus 2026 review — [PubMed 41769460](https://pubmed.ncbi.nlm.nih.gov/41769460/)); foot/ankle 50 % fracture risk 3.7-8.3 kN (Funk 2002 — [PubMed 12596644](https://pubmed.ncbi.nlm.nih.gov/12596644/)); femur regulatory 10 kN (49 CFR 571.208 — [govinfo](https://www.govinfo.gov/content/pkg/CFR-2023-title49-vol6/xml/CFR-2023-title49-vol6-sec571-208.xml)); A2 pulley ≈400 N (secondary — [theclimbingdoctor.com](https://theclimbingdoctor.com/climbing-pulley-injury-anatomy-biomechanics-and-research/)); HIC15 700 (49 CFR 571.208).

Kinetic energy and momentum of a 70 kg body (computed; formulas below)

| Speed | km/h | KE = 0.5 m v^2 | Momentum m v | Equivalent free-fall height v^2/(2g) | Reference point |
|---|---|---|---|---|---|
| 5 m/s | 18 | 875 J | 350 kg m/s | 1.27 m | jogging |
| 7.35 m/s | 26.5 | 1,891 J | 514 kg m/s | 2.75 m | trained athlete sprint (Harper 2022) |
| 10 m/s | 36 | 3,500 J | 700 kg m/s | 5.10 m | fast sprinter; Rosén P(death) ≈ 3.6 % at 40 km/h |
| 12.34 m/s | 44.4 | 5,330 J | 864 kg m/s | 7.76 m | Bolt peak (Maćkała & Mero 2013) |
| 15 m/s | 54 | 7,875 J | 1,050 kg m/s | 11.5 m | Rosén ≈ 11 % at 54 km/h; Tefft 50 % AIS4+ at 53 km/h |
| 20 m/s | 72 | 14,000 J | 1,400 kg m/s | 20.4 m | Rosén ≈ 39 % at 72 km/h |
| 30 m/s | 108 | 31,500 J | 2,100 kg m/s | 45.9 m | Rosén ≈ 94 % at 108 km/h |

Superhuman jump tiers (computed; landing assumed feet-first with 0.5 m of knee flexion, no roll)

| Jump height (CoM) | Takeoff = landing speed | KE (70 kg) | Mean landing decel | Mean leg force (70 kg) | Niu PvGRF fit (extrapolated above 1.03 m) | Human tissue verdict |
|---|---|---|---|---|---|---|
| 0.5 m | 3.1 m/s | 343 J | 1 g | 1.4 kN | 3.8 BW | routine |
| 1 m | 4.4 m/s | 687 J | 2 g | 2.1 kN | 5.3 BW | elite human / paratrooper (Whitting 13.7 BW at 4.6 m/s with hard PLF) |
| 2 m | 6.3 m/s | 1,373 J | 4 g | 3.4 kN | 7.3 BW | parkour with roll (Dai 2020 tested 2.7 m) |
| 3 m | 7.7 m/s | 2,060 J | 6 g | 4.8 kN | 8.9 BW | fracture territory begins (Funk 50 % risk 3.7-8.3 kN per tibia) |
| 5 m | 9.9 m/s | 3,434 J | 10 g | 7.6 kN | 11.3 BW | fractures likely; mortality rising (Hsieh >6 m) |
| 10 m | 14.0 m/s | 6,867 J | 20 g | 14.4 kN | 15.9 BW | tibiae at/over limit; ≈50 % death with head/chest injury (Dickinson) |
| 20 m | 19.8 m/s | 13,734 J | 40 g | 28 kN | 22.3 BW | beyond any human bone; LD50-plus |
| 50 m | 31.3 m/s | 34,335 J | 100 g | 69 kN | 35 BW | Snyder's extreme survivors (≈35 m/s) are outliers |

### Inferences
- For a fixed-size superhuman, "jump N times higher" means takeoff velocity x sqrt(N), takeoff/landing kinetic energy x N, and, for the same landing distance, peak leg force x N; "run N times faster" means kinetic energy x N^2 and stopping distance x N^2 for the same braking force. Since bone, tendon and pulley strengths are properties of tissue cross-section (fixed unless separately buffed), the design's separate "durability" tier is not just a game choice but what the square-cube logic demands: power without durability produces self-injury, exactly as the brief intends.
- A practical mapping: each durability tier multiplies the tissue thresholds in this document (tibia ~8 kN, femur 10 kN, HIC15 700, chest 60 g / 63 mm, grip ~0.5 kN per hand, pulley ~0.4 kN) by the same factor as the power tier's force gain; the speed tier should also raise braking force (self-arrest) or stopping distances grow quadratically.

### Gaps
- Hill 1950, McMahon 1984, McMahon & Bonner 1983 and Alexander 2003 were not opened; the scaling statements rest on Bobbert 2013 and the Scholz 2006 abstract, which cite them.
- No allometric data on bone strength vs body mass (e.g., Biewener's safety-factor work) was retrieved; it would let the design give a principled "bone strength must scale as force" statement.

## Physics formulas and derived tables

### Takeaway
Six formulas connect every threshold above: kinetic energy, momentum/impulse, uniform-deceleration stopping distance, free-fall speed, g-load from stopping distance, and the empirical risk functions (Rosén-Sander, Tefft, Rowson-Duma, Niu).

### Cited Findings
- Kinetic energy KE = 0.5 m v^2; momentum p = m v; impulse F_avg * dt = m * dv (Newtonian mechanics; used as stated by, e.g., Viano et al. 2007 where HIC ∝ deltaV^4/d^1.5 — [PubMed 17762744](https://pubmed.ncbi.nlm.nih.gov/17762744/)).
- Stopping distance with reaction time: d = v t_react + v^2/(2 a) (brief's formula; t_react from Woods 2015 — [PMC4374455](https://pmc.ncbi.nlm.nih.gov/articles/PMC4374455/); a from Harper 2022 — [PMC9474351](https://pmc.ncbi.nlm.nih.gov/articles/PMC9474351/)).
- Mean deceleration over stopping distance s: a = v^2/(2 s); g-load = a/9.81; force F = m a (+ m g if vertical). Time of impact dt = 2 s / v.
- Free-fall impact speed v = sqrt(2 g h); equivalent height of a speed h = v^2/(2 g).
- Head Injury Criterion: HIC = max over (t1,t2) of [ (1/(t2-t1)) * integral of a(t) dt ]^2.5 * (t2 - t1), a in g, window ≤ 15 ms (HIC15) or ≤ 36 ms (HIC36); limits 700 and 1,000 — 49 CFR 571.208 S6.2 — [govinfo](https://www.govinfo.gov/content/pkg/CFR-2023-title49-vol6/xml/CFR-2023-title49-vol6-sec571-208.xml); formula form per the HIC mini review — [biomedgrid.com](https://biomedgrid.com/fulltext/volume5/head-injury-criterion-mini-review.000957.php).
- Pedestrian death risk P(v) = 1/(1 + exp(6.9 - 0.090 v)), v in km/h; with age: 1/(1 + exp(9.1 - 0.095 v - 0.040 age)) — Rosén & Sander 2009 eqs (2)-(3) — [NACTO preprint](https://nacto.org/wp-content/uploads/pedestrian_fatality_risk_function_car_impact_speed_rosen.pdf).
- Concussion combined probability CP = 1/(1 + exp(-(-10.2 + 0.0433 a + 0.000873 alpha - 0.00000092 a alpha))), a in g, alpha in rad/s^2 — Rowson & Duma 2013 — [PMC3624001](https://pmc.ncbi.nlm.nih.gov/articles/PMC3624001/).
- Two-leg landing peak force PvGRF [BW] = 0.49 sqrt(DH_cm) + 0.37, valid 10-103 cm — Niu et al. 2014 — [PMC4160626](https://pmc.ncbi.nlm.nih.gov/articles/PMC4160626/).
- Fall factor f = fall length / rope length; rope impact force limited to 12 kN at f = 1.77 with 80 kg — Petzl — [theory](https://www.petzl.com/US/en/Sport/Fall-factor-and-impact-force---theory), [standards](https://www.petzl.com/US/en/Sport/Impact-force---standards).

Derived look-up: mean deceleration and force when a 70 kg runner stops against a wall (computed)

| Speed | Rigid stop, s = 0.1 m | Arms/crumple, s = 0.3 m | Nearest empirical anchor |
|---|---|---|---|
| 3 m/s | 4.6 g, 3.1 kN, 67 ms | 1.5 g, 1.1 kN, 200 ms | below one boxer's punch (3.4 kN) |
| 5 m/s | 12.7 g, 8.8 kN, 40 ms | 4.2 g, 2.9 kN, 120 ms | Petzl FF 1.0 fall on rope = 4 kN "bearable" |
| 7.35 m/s | 27.5 g, 18.9 kN, 27 ms | 9.2 g, 6.3 kN, 82 ms | OSHA harness cap 8 kN; femur limit 10 kN |
| 10 m/s | 51 g, 35 kN, 20 ms | 17 g, 11.7 kN, 60 ms | Stapp 46 g (restrained, 1.4 s); chest 60 g limit |
| 12.34 m/s | 78 g, 53 kN, 16 ms | 26 g, 18 kN, 49 ms | Rosén P(death) 5 % at 44 km/h |
| 15 m/s | 115 g, 79 kN, 13 ms | 38 g, 26 kN, 40 ms | Tefft 50 % AIS4+ at 53 km/h |
| 20 m/s | 204 g, 140 kN, 10 ms | 68 g, 47 kN, 30 ms | Bräck 214 g (survived with fractures) |
| 30 m/s | 459 g, 315 kN, 7 ms | 153 g, 105 kN, 20 ms | Rosén 94 % death at 108 km/h |

### Inferences
- These formulas are enough to implement damage in Jolt: read the contact normal impulse J and contact duration, compute a_mean = J/(m dt), compare with 46 g (bruising/blackout), 60 g / HIC15 700 (serious head/chest injury), and use speed-based probability (Rosén or Tefft) for a stochastic death check; for landings compute v at contact and use Niu/technique multipliers against the tibia/femur limits.

### Gaps
- No source gives body compliance distance for a human hitting a wall; 0.1 m and 0.3 m are engineering assumptions.

## Better search terms and venues

- Sprint mechanics and acceleration profiles: search "sprint acceleration force-velocity profile Samozino" and "Morin Samozino sprint mechanical properties" in Scandinavian Journal of Medicine & Science in Sports, Journal of Biomechanics and Sports Medicine; for Bolt's peak acceleration read the full PDF of Hernández Gómez et al. 2013 (Eur J Phys 34:1227) and Graubner & Nixdorf 2011 "Biomechanical analysis of the sprint and hurdles events at the 2009 IAAF World Championships" (New Studies in Athletics 26:19-53).
- Jump norms: "NBA Draft Combine max vertical", "NFL Combine broad jump dataset", and "countermovement jump reference values force plate adults" in Journal of Strength and Conditioning Research and Sports (MDPI); Bosco 1983 for classic jump-test norms.
- Deceleration/self-arrest: "horizontal deceleration ability" (Harper, Dos'Santos), "acceleration-deceleration ability ADA test" in Sports Medicine and Journal of Sports Sciences.
- Falls: "fall height mortality logistic regression trauma registry", "LD50 fall height", "vertical deceleration injury" in Journal of Trauma and Acute Care Surgery, Injury, Trauma (SAGE), Journal of Forensic Sciences; Warner & Demling 1986 (J Trauma 26:55-58) for the classic 4-storey LD50; Snyder's 1963 FAA report AM 63-15 via the FAA library or NTIS.
- Landing forces: "drop landing peak vertical ground reaction force height", "landing loading rate technique", "parachute landing fall ground reaction force", "judo ukemi breakfall impact force" in Journal of Applied Biomechanics, Journal of Biomechanics, Journal of Sports Science and Medicine, Aviation Space and Environmental Medicine.
- Bone tolerance: "tibia axial impact tolerance Yoganandan", "femur fracture load impact cadaver Kress", "lower extremity injury criteria crash", Stapp Car Crash Journal and IRCOBI conference proceedings (ircobi.org has free PDFs); NHTSA docket "Development of Improved Injury Criteria" (Eppinger et al. 1999/2000) via regulations.gov.
- Head and chest: "Prasad Mertz HIC injury risk curve", "chest deflection rib fracture risk Kroell", "concussion risk function linear rotational acceleration" in Stapp Car Crash Journal, Annals of Biomedical Engineering, Neurosurgery.
- Pedestrian curves: "pedestrian fatality risk impact speed GIDAS", Rosén, Stigson & Sander 2011 literature review (Accid Anal Prev 43:25-33), and Hussain et al. 2019 meta-analysis in Accident Analysis & Prevention.
- Grip, hang, climbing: "finger flexor pulley rupture force cadaver Lin 1990", "climbing fall arrest force load cell climber", "one-arm hang time climbers", "Pavier 1998 climbing falls Sports Engineering", in Journal of Hand Surgery, Journal of Biomechanics, Sports Engineering, Wilderness & Environmental Medicine; also the UIAA Safety Standards site for UIAA 101 test parameters.
- Shoulder/tendon: "glenohumeral dislocation force in vivo", "supraspinatus tendon ultimate tensile load cadaver", "biceps tendon rupture load" in Journal of Shoulder and Elbow Surgery and Clinical Biomechanics.
- Punching and hand: "metacarpal fracture load cadaver three-point bending intact", "boxer's fracture biomechanics force", "punch force Hybrid III", Feld, McNair & Wilk 1979 "The Physics of Karate" (Scientific American), in British Journal of Sports Medicine and Journal of Hand Surgery.
- Building materials: Gypsum Association GA-235; ASTM C1396, C473, C90, C62, C216; Brick Industry Association Technical Notes; for doors search "forced entry resistance test door ASTM F476 impact energy" and "ASTM F1233 security glazing forced entry".
- Scaling: Hill 1950 (Science Progress 38:209), McMahon & Bonner 1983 On Size and Life, McMahon 1984 Muscles, Reflexes, and Locomotion, Alexander 2003 Principles of Animal Locomotion, Biewener 1990 "Biomechanics of mammalian terrestrial locomotion" (Science 250:1097) for bone safety factors; search "allometry bone safety factor Biewener" and "isometric scaling jump height Bobbert".
