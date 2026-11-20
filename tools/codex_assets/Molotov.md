# Molotov (budget 4k tris, 1024 textures)
Game use: Grenade_Molotov, a thrown incendiary. A used glass liquor/soda bottle (~0.75 L, ~28 cm tall
without the rag) filled 2/3 with amber-brown fuel, a torn cloth rag stuffed in the neck and hanging
~12 cm down the side, wrapped and tied with a bit of wire or string; a faded scraped-off paper label
remnant; glass with real thickness, refraction-ready (separate glass material: transmissive, slight green
tint; liquid as its own closed mesh inside with its own material).
Separate parts: `Bottle` (glass), `Fuel` (liquid volume, flat liquid surface at the 2/3 line), `Rag`
(cloth, with an origin at the neck so we can attach a flame effect at `Rag`'s lowest tip; also add an empty
object `FlameSocket` at the rag tip).
Axis: thrown-item convention (origin at centre of mass, bottle upright along +Z). Three materials allowed.
