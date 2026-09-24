#pragma once

// Per-house no-go zones. See Hooks.PatherProbe.cpp for the mechanism and the
// evidence behind it.
namespace NoGoZone
{
	// Bar button: drop a zone on the first selected unit, or clear all zones
	// when nothing is selected.
	void ToggleAtSelection();
}
