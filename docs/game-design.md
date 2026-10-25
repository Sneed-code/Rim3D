# Colony game: game design

_Drafted 2026-10-24 with sneed. This file is the source of truth; the original Claude Doc is [here](https://claude.ai/artifact/CZyBNb87GP7E573BwEuBrq)._

This is the game we are building: RimWorld's colony simulation, rebuilt in 3D in Unreal Engine 5, where you also walk the colony as one of its colonists. It fixes what the game is, so the [architecture doc](architecture.md) and every later system doc can build on it. Where you haven't decided something yet, I picked a default and marked it in the last section; change any of them by editing or commenting. The story and setting are our own, set in the year 6650: see [lore.md](design/lore.md), with our names for RimWorld's in [naming.md](design/naming.md).

## Vision and pillars

A story generator you can stand inside. The colony plays out like RimWorld, with the same systems, threats and long campaigns, but you see it at eye level and can pick up a rifle and join in. When two ideas conflict, these five pillars decide, in this order.

1. **RimWorld's depth comes first.** Mechanics, numbers and content follow RimWorld and its expansions unless 3D forces a change or we choose to add something of our own, such as magic. Loose resemblance is fine for art; the rules should feel the same.
2. **You are there.** You play one colonist directly in first or third person, and can switch to a top-down overseer view at any time.
3. **Colonists who don't need babysitting.** Work, schedules and defense run from assignments you set ahead of time. In a fight, pawns take cover, fall back and rescue each other on their own.
4. **Grounded and physical.** Realistic lighting and materials, fire that spreads, walls that break into debris, wounds you can see, clothes that tear.
5. **Endless replay.** Everything is generated from a seed: maps, colonists, their looks, events. No two campaigns are alike.

## Two ways to play

You switch freely between two modes with one key. The colony keeps running either way.

|  | Direct mode | Overseer mode |
| --- | --- | --- |
| Camera | 1st or 3rd person on your protagonist (toggle) | Free top-down camera over the whole map |
| Time | Normal speed only, no pause | Pause, 1x, 2x, 3x |
| You do | Walk, build, mine, chop, carry, craft, fight, talk, trade | Everything in RimWorld's UI: architect menu, work tab, schedules, zones, designations, defense plans |
| Your protagonist | Controlled by you | Runs on the same AI as every other colonist, following their work priorities |

**The protagonist is a colonist with extras.** They have skills, traits, needs, health and gear like any pawn and are created at game start from a seed, which you can reroll or tweak like RimWorld's character screen. On top of that:

- **Tougher:** double the hit points of a normal colonist on every body part.
- **A talent tree:** experience earns talent points, spent on perks such as faster building, steadier aim, more carry weight or better trading. Normal colonists still level skills the RimWorld way.

**Hands-on work, Palworld style.** Look at a tree and hold the interact key to chop it; look at a wall blueprint to build it. You work at your own skill speed. Building uses a 3D ghost that snaps to the grid, so placing a room by hand is as quick as in the overseer view.

**Fighting.** In direct mode you aim and shoot or swing yourself, third-person-shooter style. Your hits go through the same damage rules as everyone else's: body parts, armor, bleeding.

**Taking over another colonist.** You can possess any colonist and play as them. The one you left goes back to their AI.

**Saving and death.** F5 quicksaves, F9 quickloads, and the game autosaves every in-game day. If your protagonist dies, it's game over and you reload your last save.

**Skipping the boring parts.** When you and every colonist who isn't on night shift are asleep, the night can be skipped to morning, Palworld style. Fast travel to places you've already visited on the map works when no threat is active. Nothing can happen during a skip, so no event is ever missed.

## Core loop

The loop is RimWorld's: keep a growing group of people alive and sane while the narrator throws problems at you. What changes is that you live the small moments in person.

| Scale | What you do | What pushes back |
| --- | --- | --- |
| Minutes | Chop, haul, build a wall, shoot a raider, check on a wounded colonist | Your own needs, hunger, darkness, wild animals |
| A day (about 48 real minutes) | Set priorities and schedules, plan new rooms, trade with a caravan, handle a mental break | Food and rest needs, moods, minor events |
| A season (15 days) | Expand the base, research, gear up, recruit, grow and store food for winter | Raids, disease, heat waves and cold snaps, quests with deadlines |
| A campaign (years) | Build a fortress, tame a world, chase a goal such as building a ship or a skyhull to leave, or waking a Sleeper | Escalating raids, Remnant clusters, the narrator's bigger threats |

The narrator (RimWorld's storyteller: Orrin, Maren or Jinx, with difficulty settings) drives the pacing, using RimWorld's threat-point rules so danger scales with colony wealth and size.

## Colonists

Colonists work exactly like RimWorld pawns, with RimWorld's numbers as the starting point. Each system below gets its own short design doc when we build it.

- **Identity:** name, age (adults only), backstories, traits, skills with passions, lineage (RimWorld's xenotype) and genes. All rolled from a seed, so the same seed always gives the same person, looks included.
- **Looks:** MetaHuman-based bodies and faces mixed by seed (heavy, thin, tall, muscular), skin tone, hair, body hair, scars, tattoos and gene parts such as Emberkin horns. Bodies are anatomically complete when unclothed.
- **Work:** RimWorld's work tab, with work types, priorities 1 to 4 and skill-based speed and quality. Within the same priority, pawns prefer closer jobs, which cuts down on cross-map walking.
- **Schedules:** hourly sleep, work, recreation and anything blocks, as in RimWorld.
- **Needs and mood:** food, rest, recreation, comfort, beauty, outdoors, temperature and the rest, feeding RimWorld's thought and mood system, with mental breaks at low mood.
- **Health:** RimWorld's body-part health: injuries per part, bleeding, infections, diseases, pain, prosthetics and bionics. Wounds show on the body.
- **Apparel:** RimWorld's layers (skin, middle, shell, headgear, belt) with hit points, quality, material and insulation. Damaged clothes look torn and fall off when destroyed.
- **Social:** opinions, friendships, rivalries, social fights and partners and marriage as social bonds. There is no sexual activity of any kind, and no pregnancy.
- **Movement:** base speed about 4 m/s, a steady jog. Colonists move freely along smooth paths, not cell to cell, and are slowed by injuries, carried weight, terrain and slope.

New colonists join through RimWorld's routes: wanderers, refugees, rescues, prisoner recruitment, quests and trade.

**Gear goes a little beyond RimWorld.** Clothing, armor and weapons can be repaired at a bench. Weapons can be upgraded with parts and recipes, so a good basic rifle stays useful late in the game. Colonists and the protagonist carry more than one weapon, such as a rifle, a sidearm and a melee weapon, and switch to suit the fight.

## Building the base

Building feels like Palworld, but structures sit on an invisible 1 m grid like RimWorld's. Palworld has no world grid: you put a foundation down anywhere, and walls and roofs snap to it. We snap structures to the world grid instead, because rooms, temperature, fire and pathing all depend on it. In play it feels the same: you aim, a ghost piece snaps into place, and you or your colonists build it.

**The grid is for structures only; this is not Minecraft.** Terrain is natural, sloped ground. Mountains are smooth rock, and mined tunnels have rough, natural walls. People and animals walk freely along smooth paths, step around each other, and knock loose objects about.

- **Grid pieces:** walls, doors, floors, roofs, furniture, production benches, power, turrets and traps snap to 1 m cells. Walls look about 40 cm thick even though they fill a cell.
- **Materials matter:** wood, stone blocks, steel, plasteel and the rest change hit points, flammability, beauty and build time, as in RimWorld.
- **Rooms:** an enclosed, roofed space is a room with its own temperature, cleanliness, beauty and role (bedroom, hospital, kitchen), which drives mood.
- **Multi-storey:** floors stack, joined by stairs and ladders. Upper floors need walls or pillars underneath and collapse if those are destroyed. Each floor's rooms have their own temperature, and the overseer view cuts away upper floors so you can see inside.
- **Mining:** dig into mountains to make cave rooms, with the same overhead-rock risks as RimWorld. Ore lodes (steel, silver, gold, plasteel, uranium, jade, components) are mined out of the rock.
- **Trees:** chopped down or blown apart, they fall with physics and leave logs and a stump.
- **Decorations:** small decor items can be placed freely off-grid, since they don't affect the simulation beyond beauty.

Everything built has hit points, shows damage as it wears down, and breaks into physics debris and rubble when destroyed.

## Threats, combat and defense

Threats follow RimWorld: raids by factions, Remnant clusters, chitinid brood mounds, manhunter packs, sieges, sappers and drop pods, scaled by the narrator. The difference is how colonists fight. They defend from plans you set ahead of time, and they fight sensibly without micro.

**Defense plans replace drafting.** You set these up once, and the alarm (manual, or automatic when a threat is spotted) puts the plan into action.

| Role | What the colonist does when the alarm sounds |
| --- | --- |
| Defender | Goes to the best cover on the side the threat comes from, or to a defense spot you placed |
| Medic | Stays behind the line and rescues and treats the downed |
| Firefighter | Puts out fires once it is safe to move |
| Shelter | Goes to a safe room you marked |
| Keep working | Ignores the alarm unless the threat comes within a set distance |

You can still draft and command any colonist directly, or possess one and fight yourself.

**How a good fighter behaves.** These are the tests the combat AI must pass, checked by running scripted raids hundreds of times overnight.

- Takes cover when any is within about 10 m, and never stands in the open trading shots.
- Moves like a shooter player: strafes, crouches, leans out from cover to fire, dives away from grenades and sidesteps slow projectiles.
- Picks targets sensibly: the closest real threat, the downed only when nothing else is left.
- Falls back to the next cover or fallback line when outnumbered, badly hurt or out-gunned.
- Drags downed friends to safety when the route is reasonably safe.
- Only charges into melee when armed for it, or when the trait (such as Brawler) says so. In melee, swings, blocks and dodges with timing.
- Raiders use the same rules, so they flank, retreat and rout when they are losing, as in RimWorld.

**Every fight is a real shooter fight, for everyone.** Bullets, arrows, grenades and rockets are real projectiles with speed, drop and spread, whether you fire them or a raider does. Slow projectiles can be dodged, explosions hit everything in their radius, and walls and sandbags physically stop shots. There is no hidden hit-chance roll.

**What skill does.** For AI fighters, Shooting skill sets how well they aim: tighter spread, quicker reactions and leading moving targets. For you, aiming is your own, and your protagonist's skill reduces recoil and sway and speeds up reloads. Weapons set damage, fire rate, range, projectile speed and blast radius.

**Damage** then follows RimWorld: armor penetration against armor, then injuries to the body part that was actually hit. Gore is shown: wounds, blood, severed limbs, and explosions that blow bodies apart.

## World, maps and time

Your colony sits on one 1 km map, and the wider world is a RimWorld-style planet map you travel across with caravans. Other maps are generated when you visit them.

- **Home map:** 1,024 m by 1,024 m, generated from a seed with terrain, rivers, mountains, ore, plants, ruins and animals. Biome sets climate, plants and wildlife. The colony itself usually covers 100 to 250 m.
- **Other maps:** quest sites, ambushes, settlements and caravan stops are generated on arrival at 256 to 512 m and discarded afterwards. Your home colony keeps running while you're away.
- **World map:** a planet of tiles with biomes, factions, settlements and roads. Caravans travel on it in abstract form, as in RimWorld, without walking every kilometre in 3D.
- **Time:** one in-game day takes 48 real minutes at normal speed, so one in-game hour is 2 minutes. RimWorld's 60-day year with four 15-day seasons stays. All of this is a tunable number.
- **Weather and seasons:** rain, fog, snow, heat waves, cold snaps, toxic fallout, eclipses and the rest, affecting temperature, plant growth, moods and accuracy.
- **Light:** day and night matter. Darkness slows work and hurts mood, and colonists need lamps.

**Animals** breed and raise young as in RimWorld, so a farm can hold hundreds of chickens. The no-children rule is about people only.

## Content rules

This is a private game for one player. These are the rules for now, and they can be revisited later.

- **Adults only.** No children, no pregnancy, no growth stages. Biotech's gene systems apply to adults through lineage grafts and gene implanting.
- **Nudity yes, sex no.** Unclothed colonists are anatomically complete, and nudity matters to mood through ideology and traits, as in RimWorld. There is no sexual content of any kind.
- **Gore yes.** Visible wounds, blood, dismemberment, bodies blown apart by explosions, ragdolls and corpses that rot.
- **Our own setting and names.** The lore is ours ([lore.md](design/lore.md)) and RimWorld's proper names (peoples, factions, enemies, narrators, places) have our own names ([naming.md](design/naming.md)). Plain words such as steel or sniper rifle stay. Names live in data files, so renaming anything is a one-line change that doesn't affect gameplay.
- **Free assets only.** Each third-party asset is logged with its source and licence. Art only needs to fill the same role as RimWorld's, not copy it.

## Scope by milestone

We go systems first, art second. Every vanilla mechanic gets built and working with whatever art is ready, using rough placeholders where needed, and the art catches up alongside. Expansion pieces come in whenever a thread is free, not after the vanilla core is finished.

| Milestone | What's in it | Target |
| --- | --- | --- |
| Prototype | Protagonist in 1st/3rd person; overseer camera; 5 to 10 colonists who mine, chop, haul, build, cook, eat and sleep; work tab and schedules; walls, doors, beds, a stove, stockpiles; basic needs and mood; one raid type with shooter-style AI; a 512 m test map; quicksave | Weeks 3 to 4 |
| Vanilla mechanics | Full health and medicine, apparel and armor, weapons and quality, crafting chains, research, power, mental breaks, social, fire, temperature, weather, multi-storey building, narrator and events, animals and breeding, world map, caravans, trade, quests, full 1 km maps | Weeks 8 to 10 at 40% of a Max 5x plan |
| Expansion pieces | Binders and Remnant machines (Biotech's mechanitor), genes and lineages, resonance (Royalty's psycasts, our magic), creeds and roles (Ideology), the Black Spires (Anomaly), skyhulls (Odyssey), brought in as standalone pieces first, with their quests later | Starting around week 4, ongoing |
| Art and polish | MetaHuman pawns with anatomy, the full creature and item roster, gore effects, sound, styled UI | Ongoing from week 2 |

Usage is the main limit on speed. Staying on target needs two or three threads running most days, Sonnet for routine work, an Unreal session on your PC most days, and fast playtest feedback from you. We'll know at the end of week 2 whether we're on pace.

**Not planned:** multiplayer, mod support beyond editing data files, VR, consoles, and anything in the content rules above.

## Open questions

I'm building with the default answers below. Change any of them by editing this table or leaving a comment; none of them blocks the prototype.

| Question | Default I'm using | Other options |
| --- | --- | --- |
| What happens when your protagonist dies? | Decided: game over, and you reload your last save | Carry on as another colonist (could be added later) |
| Does your protagonist have the same needs as everyone else? | Yes, but the night can be skipped once everyone is asleep | No sleep need for the protagonist, or slower-draining needs |
| How does a new game start? | Crashlanded: you plus 2 colonists, RimWorld's starting gear | Other RimWorld scenarios, picked at start |
| How long is a day at normal speed? | 48 real minutes | Anything from 20 to 60 minutes |
| Can time speed up while you're in direct mode? | No, but night skip and fast travel cover the slow parts | Allow 2x in direct mode |
| Multi-storey buildings? | Decided: yes, built into the sim from the start and playable in the vanilla core | Stairs and ladders first, elevators later |
