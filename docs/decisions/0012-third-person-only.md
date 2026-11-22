# 0012: Third person only

Date: 2026-11-22. Asked by sneed.

**Decision.** The game is third person only. There is no first-person view: the V key (first / third person) is gone,
the "hold aim goes to first person" setting is gone, and an old `Feel.AimView 1` setting is ignored. Right mouse with a
gun keeps the over-the-shoulder aim (zoomed in, steadier). The one exception is a gun with a scope (`assets/guns.json`
`"scope"`, the bolt-action rifle): fully aimed, the view goes straight to the scope (its field of view, the scope ring and
reticle, the body hidden), never a first-person view of the arms or gun.

**What changed.** `AProtagonistPawn::SetFirstPerson` always leaves the camera behind the body and `IsFirstPerson` is
always false, so the first-person arms (`UGunRangeSubsystem::UpdateView`, `UpdateHeldView`) are never drawn. The scope
view is `AProtagonistPawn::IsScopeView` (camera at the eyes once aim passes 0.97) with the field of view from
`UGunRangeSubsystem::GetScopeFov`; the range's scope ring is drawn in the colony too. The first-person code stays in the
tree, unused, in case it is ever wanted again.

**Why.** sneed: "lets forget first person view now. lets disable it its a 3rd person game ... lets fix 3rd person."
Every first-person fix (gun holds, arms that breathe, weapon swaps) was a second copy of the third-person work, and
third person looks better.
