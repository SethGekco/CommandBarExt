#pragma once

struct CellStruct;
class TechnoClass;

// Per-house no-go zones. See Hooks.PatherProbe.cpp for the mechanism and the
// in-game evidence behind it.
namespace NoGoZone
{
	// Legacy tool: drop a zone on the first selected unit. `follow` anchors it
	// to that unit so it moves along. Triggering it while the selected unit
	// stands in one of your zones DELETES that zone instead of stacking
	// another. Nothing selected clears every zone.
	void ToggleAtSelection(bool follow);

	// Click-to-place tool: arm placement mode; the next click on the tactical
	// map drops a zone there (or removes the zone you clicked inside).
	void EnterPlacementMode();

	// Once per logic frame: move anchored zones, drop zones whose anchor died.
	void UpdateAnchored();

	// Shared by the click hook (defined in Hooks.PatherProbe.cpp).
	extern bool PlacementArmed;
	void PlaceOrRemoveAt(const CellStruct& cell, TechnoClass* pAnchor);
}
