# PR #470 hair (claude/project-thread-jvxv0x at 80a304a8), PC strand demo

Female, LongStraight, `-CreatorStrandDemo -CreatorFemale -CreatorHair=Long -CreatorRecord=1`.
- a: as above
- b: plus `-CreatorFaceShape=Brow:1,HeadWidth:1` (logged "face shape ... on everyone", but hair fit scale identical to a and no "STRANDHAIR head sliders" line; head looks unchanged)
- c: plus `-CreatorStrandWalkPitch=-70`

strand_*.jpg are the set pictures; strandwalk_*.jpg every walk frame taken (about 20 per run), 960 px wide.

## b2, c2 (b70cba35, after rebuilding the test copy's bodies so they have the face-shape morphs, and regrowing the hair)
- b2: `-CreatorFaceShape=Brow:1,HeadWidth:1`: hair scale X=1.19, lift 1.9-2.1 mm
- c2: `-CreatorFaceShape=Brow:1,HeadWidth:-1`: hair scale X=0.86, lift 1.8-2.2 mm
- b2/heads_a_b_c.jpg: idle side (top) and back (bottom) heads, left to right: no flag, wide (b2), narrow (c2)
