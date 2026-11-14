# Tropical animal shortlist (Z1)

Written 2026-11-14 for task Z1 of [animation.md](animation.md) "Tropical animals". It lists where a
photoreal, well-rigged model of each tropical animal can come from, free first, with what each one
is and costs as far as the listing says. sneed picks; Z2 starts from the pick.

**How this was checked.** From the cloud: web searches, the Fab and Superhive listing pages, and one
Sketchfab search. Sketchfab and Fab block most automated reads, so polygon counts, prices and licences
come from the listings' own text where it gave them, and "not shown" means the page did not say. No
model was downloaded or opened in Unreal yet; Z2 does that on the PC. There is no picture sheet: the
images could not be fetched from the cloud, so each line links the listing, where the pictures and a
3D viewer are.

## What was found, in one paragraph

**No free model of these animals is both photoreal and well rigged.** The free ones are game
models of 2,000 to 7,000 triangles with 10 to 30 clips. They look fine from the overseer camera,
plain up close, and they are a step up from the Quaternius cartoons. The realistic ones are paid. One
paid pack covers most of the list: **Junnichi Suko's Mammals Pack** has a capybara, elephant,
rhinoceros, pig, impala and gorilla, realistic, rigged and animated, 5,000 to 30,000 vertices each,
about $150 (the price shown on Superhive; the Fab price was not shown). No pack found has a realistic
monkey; a monkey is a separate buy or our own build.

## Per animal

| Animal | Best free | Best paid (realistic) | Notes |
| --- | --- | --- | --- |
| **Capybara** | Only stylized: "Capybara - Animated - Lowpoly" by deko on [Fab](https://www.fab.com/listings/939d3066-3730-49c6-974c-f267a58160f9) (2,546 triangles, idle, eat, walk, run, sit, 4K textures; cute style, a poor fit) | Junnichi Suko capybara, in the [Mammals Pack](https://www.fab.com/listings/840d905c-3236-4a03-a95b-5b5dd615b814) (also sold alone on the Unity store) | the free one does not match realistic people |
| **Monkey** | none realistic found | none in a pack; single models: [Capuchin by Nyilonelycompany](https://www.renderhub.com/nyilonelycompany/capuchin-monkey) (RenderHub), [rhesus macaques](https://www.turbosquid.com/3d-models/3d-wild-rhesus-monkey-light-brown-rigged-model-2436935) on TurboSquid and CGTrader (rigged for Cinema 4D or Maya, fur made for offline renders) | the hardest one; may need our own build (Blender, fur groom, Rigify) on a bought or AI-made mesh |
| **Wild boar** | none realistic found free | [Wild Boar Animated by WDallgraphics](https://www.fab.com/listings/a8b46c74-4dd5-4398-9985-55b33aea9d42) on Fab (43 clips: walk, trot, run, turns, lie down, wallow, swim, two attacks, deaths; 4K textures; only 1,736 vertices); a [Realistic Boar](https://superhivemarket.com/products/realistic-boar) on Superhive (also listed on Epic's store; author and price not checked); Suko's pig as a stand-in | WDallgraphics has the clips but very few vertices |
| **Elephant** | [AFRICAN ANIMALS PACK by Protofactor](https://www.fab.com/listings/9d73ed2e-7917-4afe-9f89-e68420e6b1b7), **free on Fab** (elephant, rhino, hippo, crocodile, lion, zebra; 4,000 to 7,000 triangles, 35 bones, 12 clips); on Sketchfab, CC BY: "African Elephant" by jimmyho905 (about 4,800 faces, 20 clips) | Suko elephant (Mammals Pack); Nyilonelycompany's Asian Elephant, in their [Asian Animals pack](https://sketchfab.com/Nyilonelycompany/collections/asian-animals-pack-unreal-engine-gameready-049cf4be538241649482378ba7e99c42) on Fab | the free Protofactor elephant is the best free model found for any animal on the list |
| **Rhinoceros** | Protofactor's free AFRICAN ANIMALS PACK (as above) | Suko rhinoceros (Mammals Pack); Nyilonelycompany's Indian Rhinoceros (Asian Animals pack) | |
| **Alpaca** | Quaternius' alpaca (already in the game, cartoon) | none checked yet; wool needs our own groom either way | low priority: alpacas are rare in the tropics |
| **Blastbuck** (our boomalope) | none (invented animal) | Suko's impala as the base, with a gas sac added in Blender | our own build on a bought base |

Also free and worth a look in Z2: the **Truebones free zoo** on [Gumroad](https://truebones.gumroad.com/p/free-truebones-zoo-over-75-animated-animals-with-textures-in-fbx-format)
(over 75 animated animals in FBX; its list of animals and its terms were not readable from the cloud).

## sneed's choice (2026-11-14): free models, upgraded by us

sneed picked route 1: free models only, with our own fur and skin materials and grooms on top, and
"find more, we need heaps", birds and insects too. sneed has already added Protofactor's
[AFRICAN ANIMALS PACK](https://www.fab.com/listings/9d73ed2e-7917-4afe-9f89-e68420e6b1b7) (elephant, rhino, hippo, crocodile,
lion, zebra) to the Fab library. The paid options above stay here only as a later upgrade path.

## More free models (searched 2026-11-14)

All of these showed as **Free** on Fab on 2026-11-14 (searched in sneed's own browser with the Free
filter). Lines marked **checked** had their listing read (clips, textures); the rest are picked from
the search card and Z2 opens each one before use. Free Fab items sometimes go back to paid, so adding
them to the library now keeps them.

### Mammals

| Model | Seller | What it gives us |
| --- | --- | --- |
| [ANIMAL VARIETY PACK](https://www.fab.com/listings/2dd7964c-a601-4264-a53d-465dcae1644c) **checked** | Protofactor (same artist as the African pack) | crow, fox, deer stag, deer doe, pig; 17 to 26 clips each, 2K to 4K textures. The pig is our wild boar base (tusks and bristles added). |
| [Monkey_B1](https://www.fab.com/listings/b5fda9d9-184d-4a58-bb6f-68c94c54e846) **checked**, [Monkey_B2](https://www.fab.com/listings/789189cd-1a61-47c1-bb54-71443a8cbf4a), [Monkey_B3](https://www.fab.com/listings/04dd168f-405d-428d-b2ee-3678fcf34358) | Animated3D | **the monkey**, which looked like the hardest gap: 26,600 triangles, 4K PBR textures, a UE5 file, 17 clips (idle, run, climb idle, climb sit, sit, eat, jump, roar, die and more). |
| [Leopard Hybrid_A1](https://www.fab.com/listings/d0e9ce8d-bdb9-412f-9740-6f528f56a2c1), [Leopard Hybrid_A2](https://www.fab.com/listings/3506d61b-1f96-4ca3-8fae-dea2ceee0ea5) | Animated3D | a big jungle cat (our panther and cougar stand-in); same maker as the monkey, so likely the same quality. |
| [Buffalo_A1](https://www.fab.com/listings/1ba67c0b-a236-45ca-91e2-6b18396e00e3), [Buffalo_A2](https://www.fab.com/listings/3ae82f01-37f6-497c-9bde-53404a7b57b7) | Animated3D | a buffalo (wild herd animal, and a muffalo-style base). |
| [Hyena_A1](https://www.fab.com/listings/48f273ad-defb-436a-9e23-131f1409700e), [A2](https://www.fab.com/listings/c481d494-7363-4137-9eb7-a0461dfe8b5e), [A3](https://www.fab.com/listings/2a35fe32-211f-4ee8-9965-93bc50c9390f), [A4](https://www.fab.com/listings/ba414538-8410-450e-8b85-3747051bdb35) | Animated3D | a pack predator (savanna more than jungle; good for raids by hungry animals). |
| [Fawn_A1](https://www.fab.com/listings/ef3f1277-fdb7-43b7-ad33-fd07257c2868), [Fawn_A2](https://www.fab.com/listings/65ab4747-a9ab-49d8-bf10-eb34685d98fe), [Fawn_A3](https://www.fab.com/listings/1be8ecdb-59dd-4409-86da-2b3f58e22408) | Assets Animated | young deer, to pair with Protofactor's stag and doe. |

### Birds

| Model | Seller | What it gives us |
| --- | --- | --- |
| crow, in the ANIMAL VARIETY PACK above **checked** | Protofactor | a realistic bird with 17 clips, the base for jungle birds (recoloured, new beak). |
| [Birds](https://www.fab.com/listings/a34b6ed1-fb30-4cfc-83ee-22fa31cde3e9) **checked** | Zacxophone | simple low-poly flying birds made for the background: right for flocks in the sky, not for close up. |
| [Parrot #3DST15](https://www.fab.com/search?is_free=1&q=parrot) | JuanG3D | a parrot model; rig and clips not checked. |

No free realistic parrot or toucan with animations was found. The plan is the crow's rig with a
parrot or toucan mesh and feathers made by us in Blender, or the Truebones zoo below.

### Insects and spiders

| Model | Seller | What it gives us |
| --- | --- | --- |
| [Cinematic Dragonfly](https://www.fab.com/listings/51182fab-2d08-42be-aaad-f90a782775b4) | ROTU | the best-rated free insect (4.8 from 15 reviews); dragonflies over swamps and water. |
| [Hummingbird Hawk-Moth](https://www.fab.com/listings/6029486e-cc09-48c2-b7a6-f1390fe43d92) | | a moth for night ambience. |
| [Butterflies](https://www.fab.com/listings/d3d80956-31a2-4395-959f-c592002c83c7) | | butterflies for day ambience (also the CGTrader pack below). |
| [Realistic Insect Coccinellidae](https://www.fab.com/listings/c48c7389-95f1-46a1-a35e-f2b8edefa4c2) | Evsed | a realistic ladybird. |
| [Ant - Formica Rufa](https://www.fab.com/listings/40eabf60-5e50-4c6a-aba5-c336f9b5d9ca) | BeshMesh | an ant (ant trails, and a base for insect hives). |
| [Realistic Fly](https://www.fab.com/listings/dabfa596-ce97-493b-bdc9-3985d3453e9d) | | flies around corpses and rotting food. |
| [Beetle](https://www.fab.com/listings/3d7ac2f6-9e9e-4e15-b4e5-9d02dd337d04), [BEETLES](https://www.fab.com/listings/00b077b7-5a80-436f-8455-6077dfb21b99) | | beetles: ambience, and the base for our megascarab-style insects. |
| [Nephila pilipes spider](https://www.fab.com/listings/4265106f-e4d6-4b82-88ab-5f54c359e77c) | huangyuanqiang | a golden orb-weaver, a real tropical spider. |
| [Theraposa blondi](https://www.fab.com/listings/020683e7-faae-438e-b411-50e2b53f615d) | | the Goliath birdeater tarantula, a real jungle spider; the base for our megaspider-style insect, scaled up. |
| [Spider animated character](https://www.fab.com/listings/62456bda-b5f6-4372-8673-6471359c5e50), [Spiders - characters with animations](https://www.fab.com/listings/41eeeaca-af9e-43b2-8a71-0874356cebb2) | Bestgameassets and others | spiders that already walk and attack: the fastest way to animated giant insects. |
| [Insectoid Monster Rig](https://www.fab.com/listings/ff1306d9-c968-4745-8437-0388a8d7aa5a) | DM-913 | a rigged insect monster (5.0 from 4 reviews), a candidate for the hive insects. |

Small ambient insects (butterflies, flies, dragonflies, moths) are cosmetic and drawn as Niagara
swarms, so they need a mesh and a flap clip only; the giant hive insects are real sim creatures and
need full rigs.

### Reptiles

| Model | Seller | What it gives us |
| --- | --- | --- |
| crocodile, in the AFRICAN ANIMALS PACK | Protofactor | **already in the library**; the swamp crocodile. |
| [Snake Cobra (Head Up) Animated](https://www.fab.com/listings/bc208c6c-9b64-433b-8824-62431223c061), [Snake Cobra animated (Base Model)](https://www.fab.com/listings/fa501201-17e7-49c2-a36a-a3b55834cfc7) | | an animated cobra; the base for jungle snakes. |

Checked and **not** useful: the free "Crocodile" (a crocodile-man fighter) and "Jungle Water
Crocodile" (a 1.6 MB low-poly mesh). No free realistic frog or lizard with animations was found.

### Extras for animals, also free on Fab

[Animal Carcasses Pack](https://www.fab.com/search?is_free=1&q=animal%20carcasses) (RMDavid; corpses and butchering),
[Animal Sound Effect Pack](https://www.fab.com/search?is_free=1&q=animal%20sound%20effect) (Bancet),
[Animal Foot Print Decal Pack](https://www.fab.com/search?is_free=1&q=animal%20foot%20print%20decal) and the Quixel Megascans
[animal skulls and bones](https://www.fab.com/search?is_free=1&q=animal%20skull%20megascans).

### Free outside Fab (no library step; Z2 downloads these)

- **Truebones free zoo** on [Gumroad](https://truebones.gumroad.com/p/free-truebones-zoo-over-75-animated-animals-with-textures-in-fbx-format):
  over 75 animated animals in FBX. Its list was not readable from the cloud; **Not free** (sneed found it asks $146 AUD): dropped, not needed.
- **[10 Animated Butterflies - FREE](https://www.cgtrader.com/free-3d-models?keywords=10%20animated%20butterflies)** on CGTrader.
- **Sketchfab, CC BY** (credit the author in the game's credits): "African Elephant" by jimmyho905
  and other elephants (backups to Protofactor's).
- **Quaternius** animals (CC0, cartoon), already used in the game, stay as placeholders until each
  new model lands.

### What is still missing

- **Capybara**: dropped (sneed, 2026-11-14: the Sketchfab one is not free; find other animals instead).
- **Parrot, toucan**: found in round 2 (Sketchfab).
- **Frog, lizard, gecko**: none free and animated; low priority (ambience only).

## What sneed does

Sign in to [Fab](https://www.fab.com) with the Epic account, open each link and click **Add to My
Library** (all free; already done for the African pack). In order of how much we need them:

1. [ANIMAL VARIETY PACK](https://www.fab.com/listings/2dd7964c-a601-4264-a53d-465dcae1644c) (crow, fox, deer, pig)
2. [Monkey_B1](https://www.fab.com/listings/b5fda9d9-184d-4a58-bb6f-68c94c54e846), [Monkey_B2](https://www.fab.com/listings/789189cd-1a61-47c1-bb54-71443a8cbf4a), [Monkey_B3](https://www.fab.com/listings/04dd168f-405d-428d-b2ee-3678fcf34358)
3. [Leopard Hybrid_A1](https://www.fab.com/listings/d0e9ce8d-bdb9-412f-9740-6f528f56a2c1), [Leopard Hybrid_A2](https://www.fab.com/listings/3506d61b-1f96-4ca3-8fae-dea2ceee0ea5)
4. [Buffalo_A1](https://www.fab.com/listings/1ba67c0b-a236-45ca-91e2-6b18396e00e3), [Buffalo_A2](https://www.fab.com/listings/3ae82f01-37f6-497c-9bde-53404a7b57b7)
5. [Snake Cobra (Head Up) Animated](https://www.fab.com/listings/bc208c6c-9b64-433b-8824-62431223c061), [Snake Cobra animated (Base Model)](https://www.fab.com/listings/fa501201-17e7-49c2-a36a-a3b55834cfc7)
6. Birds and insects: [Birds](https://www.fab.com/listings/a34b6ed1-fb30-4cfc-83ee-22fa31cde3e9), [Cinematic Dragonfly](https://www.fab.com/listings/51182fab-2d08-42be-aaad-f90a782775b4),
   [Hummingbird Hawk-Moth](https://www.fab.com/listings/6029486e-cc09-48c2-b7a6-f1390fe43d92), [Butterflies](https://www.fab.com/listings/d3d80956-31a2-4395-959f-c592002c83c7),
   [Realistic Insect Coccinellidae](https://www.fab.com/listings/c48c7389-95f1-46a1-a35e-f2b8edefa4c2), [Ant - Formica Rufa](https://www.fab.com/listings/40eabf60-5e50-4c6a-aba5-c336f9b5d9ca),
   [Realistic Fly](https://www.fab.com/listings/dabfa596-ce97-493b-bdc9-3985d3453e9d), [Beetle](https://www.fab.com/listings/3d7ac2f6-9e9e-4e15-b4e5-9d02dd337d04),
   [BEETLES](https://www.fab.com/listings/00b077b7-5a80-436f-8455-6077dfb21b99), [Nephila pilipes spider](https://www.fab.com/listings/4265106f-e4d6-4b82-88ab-5f54c359e77c),
   [Theraposa blondi](https://www.fab.com/listings/020683e7-faae-438e-b411-50e2b53f615d), [Spider animated character](https://www.fab.com/listings/62456bda-b5f6-4372-8673-6471359c5e50),
   [Spiders - characters with animations](https://www.fab.com/listings/41eeeaca-af9e-43b2-8a71-0874356cebb2), [Insectoid Monster Rig](https://www.fab.com/listings/ff1306d9-c968-4745-8437-0388a8d7aa5a)
7. Nice to have: [Hyena_A1](https://www.fab.com/listings/48f273ad-defb-436a-9e23-131f1409700e) to [A4](https://www.fab.com/listings/ba414538-8410-450e-8b85-3747051bdb35),
   [Fawn_A1](https://www.fab.com/listings/ef3f1277-fdb7-43b7-ad33-fd07257c2868) to [A3](https://www.fab.com/listings/1be8ecdb-59dd-4409-86da-2b3f58e22408), and the extras above
   (carcasses, sounds, footprints, skulls).

Nothing else is needed from sneed for Z2: the PC thread downloads each pack from the library into
the project when it starts.

## Round 2 (sneed's feedback, 2026-11-14)

sneed kept one cobra ([base model](https://www.fab.com/listings/fa501201-17e7-49c2-a36a-a3b55834cfc7)); the others looked
awful. sneed also added a [Velociraptor](https://www.fab.com/listings/2dd1fd61-8052-4815-9f47-c471fff67a4d), the
[Tribal Mutant Bug1 Rig](https://www.fab.com/listings/54526751-62b0-4a00-b2cd-ab0d0e91c273) (bug men), a
[Game ready crow](https://www.fab.com/listings/21cd62fa-246d-41e6-a94f-55b8a8d962c7), Pomba (a pigeon), the
[Doomsday Pickup Truck](https://www.fab.com/listings/d89201ed-5de8-437c-9b91-3af2d913421a) and the
[Vehicle Variety Pack](https://www.fab.com/listings/dc1ada50-2523-44b1-b0e2-a72d14076fb4).

**Leopards.** Animated3D has about 20 big cats (leopards, leopard cats, black panthers, tigers and
cubs), but with Fab's Free filter only the two **Leopard Hybrid** ones are free; the rest sell for
about $49 each (Tiger_A3 shows $48.69). Free: [Leopard Hybrid_A1](https://www.fab.com/listings/d0e9ce8d-bdb9-412f-9740-6f528f56a2c1),
[Leopard Hybrid_A2](https://www.fab.com/listings/3506d61b-1f96-4ca3-8fae-dea2ceee0ea5).

**Best new source: Animal Animator on Sketchfab** ([all models](https://sketchfab.com/animalanimator/models)).
Realistic, rigged and animated, and every one checked is **free, CC BY** (credit the author in the
game's credits). Sketchfab downloads need a free Sketchfab account; there is no library button.
- [Hummingbird, 6 animations](https://sketchfab.com/3d-models/hummingbird-rigged-6-animations-open-bill-d9b448ac1025485e843d54bebf35390a), a real tropical bird
- [Darter Dragonfly, real-time flight](https://sketchfab.com/3d-models/darter-dragonfly-38-hz-real-time-flight-rigged-6e1af4c0f7e943cd8f85e3085f26fd71)
- [Monarch Butterfly, animated](https://sketchfab.com/3d-models/monarch-butterfly-danaus-plexippus-animated-9682aebc9f93491b9cf1d0b2f021a87f)
- [Male Chimpanzee](https://sketchfab.com/3d-models/male-chimpanzee-hairless-rigged-animated-c53e8ca04bb54d5e8649d95b437e2ca3) (hairless: we add the fur)
- also on that page, not yet opened: an African elephant bull, a spotted hyena, a giant panda, a
  giraffe, a sphynx cat, a great white shark, an axolotl and more.

**Other free Sketchfab finds:**
- **Toucan Bird** by sevketkabatas ([link](https://sketchfab.com/3d-models/toucan-bird-eb77223ebfd946e69fb58ffe9ad32adf)): animated,
  19,000 triangles, CC BY.
- **Love Birds Parrot** by Nyilonelycompany ([link](https://sketchfab.com/3d-models/love-birds-parrot-e31e1c72cff140158b99172285b40056)):
  animated, 14,400 triangles, CC BY-NC. Same realistic artist as the paid capybara and Asian animals.
- Skip "Cotw Capybara" and similar: those are ripped from another game.

Free Fab birds stay poor; the [Harpy Eagle](https://www.fab.com/listings/9f99d816-5913-4f37-b48a-dc05b79d8f0d) (CC BY, a
rainforest eagle) has no rig information and may be AI-made, so it is a maybe.

## Round 3 (sneed, 2026-11-14 evening)

**Rules from sneed:** free only, and realistic only (no cartoon or zany styles). So the low-poly
Zacxophone "Birds", the "Cartoon butterflies", stylized ants and the cute capybara are out, and the
capybara is dropped as an animal for now. The Truebones zoo is not free and not needed.

**Downloaded by sneed** and copied to `C:\Dev\Rim3D\assets\raw\sketchfab\` (not in Git; a
`README.txt` there lists each model's author and licence): Animal Animator's hummingbird, dragonfly,
monarch butterfly, chimpanzee, panda, hyena, horse, sphynx cat, shark, pliosaur, T. rex and axolotl;
the toucan; the parrot; an "Ultimate Animal Pack" (one FBX, check in Z2); a robot; two space skyboxes;
sound effects; and orc voice clips (`assets\raw\sound\OrcFemale\`). All the Animal Animator ones
come with full texture sets (colour, normal, roughness; fur cards on the panda and hyena). Set aside,
not used: a tylosaurus ripped from a commercial game.

**Fab insects with no textures.** Fab's free insects are a mix: some ship their textures in a
separate folder or inside the file (and some viewers don't show them), some have none at all. It is
not a problem and needs no real compute: Z2 imports them and, where a model has no textures, gives
it one of our own insect materials (chitin, hair, wing membrane), made once and reused for all bugs.

**Giant insects (megaspider and friends).** Plan: the Goliath birdeater mesh (realistic) moved onto a
working spider rig, by re-binding the mesh to the rig's bones in Blender (a script, minutes of
compute); the walk, attack and death clips come from the rigged spiders (Spider animated character,
Spiders with animations), and the legs plant on the ground with Unreal's Control Rig, so uneven
ground and turning look right without hand animation. Flies and maggots on corpses are particle
effects (Niagara), cheap and easy.
