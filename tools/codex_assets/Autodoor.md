# Autodoor (budget 10k tris, 2048 textures)
Game use: Autodoor, a powered sliding door, one 1 m wide cell in a wall, ~2.2 m tall opening plus a ~0.8 m
header (3.0 m total like the walls), ~0.25 m deep frame. A steel frame with a header box housing the motor
(small vent, status lamp), and two door leaves that slide apart sideways into the frame: painted steel
panels with a narrow reinforced window strip each, rubber seal edges, hazard stripes on the leading edges,
grime near the bottom, handprint smudges. Emissive map for the status lamp.
Separate parts: `Frame`, `Leaf_L`, `Leaf_R` (each leaf's origin at the bottom of its meeting edge so it
slides along X).
Axis: building convention, the door's face towards -Y; leaves slide along X.
