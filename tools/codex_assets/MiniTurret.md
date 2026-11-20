# MiniTurret (budget 20k tris, 2048 textures)
Game use: Turret_Mini, a small automatic gun turret on a 1 x 1 m cell, ~1.3-1.5 m tall. A compact
squat tripod or short pedestal base bolted to the ground with a small battery/power box and a cable run,
a yaw ring, and a boxy gun head carrying a short-barrelled automatic gun with a perforated barrel shroud,
a drum or box magazine on the side, a small sensor eye (camera lens / rangefinder) above the barrel, and a
thin ballistic shield plate on the front. Industrial, field-repaired look: olive or grey paint over steel,
bolt heads, chipped edges, a dust skirt of red tropical mud on the feet.
Separate parts: `Base` (fixed, z = 0 at the bottom), `Head` (pivot = yaw axis, on the vertical line through
the origin), `Gun` (pivot = pitch axis), `Barrel` (pivot at the barrel's rear, along +local X, so it can
recoil). Add an empty `Muzzle` at the barrel tip.
Axis: building convention, barrel pointing towards -Y at rest.
