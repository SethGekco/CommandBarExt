// ---------------------------------------------------------------------------
// Pather probe: can we make a cell impassable to ONE house only?
//
// The blocker idea needs a per-house answer, and the obvious lever is the
// wrong one: CellClass::IsClearToMove(speedType, ignoreInfantry,
// ignoreVehicles, zone, movementZone, level, isBridge) has no house and no
// unit in its arguments, so anything decided there is global — it would block
// enemies too. It also has 22 call sites, several of which are not movement
// at all (building placement among them), so filtering there would break
// where you can construct.
//
// But the engine already asks the question per-object:
//
//   ObjectClass::IsCellOccupied(CellClass* pDestCell, FacingType facing,
//                               int level, CellClass* pSourceCell, bool alt)
//
// is a VIRTUAL, so `this` IS the unit doing the asking. If pathfinding routes
// through it, a per-house no-go zone needs no "current pather" stash at all —
// we just answer Move::No for that unit's house and every other player is
// untouched. That would be a materially simpler and safer feature than the
// pathfinding surgery I first described.
//
// THE QUESTION THIS PROBE ANSWERS: is IsCellOccupied consulted for the many
// cells of a path (so blocking there actually re-routes), or only for the
// final destination / placement (so blocking there would merely make units
// refuse to stop, and they would still drive straight through the zone)?
//
// Read the log for:
//   calls=N over one move  -> a handful means placement-only (idea is dead in
//                             this form); hundreds means pathfinding uses it.
//   distinct cells         -> a path's worth of cells, or one.
//
// Hook seats. Phobos already hooks INSIDE both implementations
// (0x51BFA2 +6 and 0x73F0A7 +9), so we take the function ENTRIES instead,
// which end before those start — no overlapping ranges (the corruption case
// from the hook-overlap check), and no same-address chaining needed:
//   InfantryClass::IsCellOccupied  0x51BF90 +5  (ends 0x51BF95 < 0x51BFA2)
//   UnitClass::IsCellOccupied      0x73F0A0 +6  (ends 0x73F0A6 < 0x73F0A7)
// At the entry ECX is still `this` and the stack is untouched, so arg1
// (pDestCell) is at [esp+4].
//
// PURELY OBSERVATIONAL: every path returns 0. Nothing is blocked yet.
// ---------------------------------------------------------------------------

#include <ObjectClass.h>
#include <TechnoClass.h>
#include <CellClass.h>
#include <HouseClass.h>

#include <Syringe.h>
#include <Utilities/Macro.h>
#include <Utilities/Debug.h>
#include <Helpers/Cast.h>

namespace PatherProbe
{
	static int TotalCalls = 0;
	static int SampleBudget = 24;

	// Rex's debug.log is already 123 MB; this is a hot path, so sample a few
	// calls in detail and then only summarise, never log per call.
	static void Observe(const char* who, ObjectClass* pObject,
		CellClass* pDestCell)
	{
		++TotalCalls;

		auto pTechno = abstract_cast<TechnoClass*>(pObject);
		const char* id = pTechno && pTechno->GetTechnoType()
			? pTechno->GetTechnoType()->ID : "?";
		const int house = pTechno && pTechno->Owner
			? pTechno->Owner->ArrayIndex : -1;

		if (SampleBudget > 0)
		{
			--SampleBudget;
			Debug::Log("[CommandBarExt] IsCellOccupied %s: %s house=%d "
				"cell=%d,%d\n", who, id, house,
				pDestCell ? pDestCell->MapCoords.X : -1,
				pDestCell ? pDestCell->MapCoords.Y : -1);
		}
		else if (TotalCalls % 2000 == 0)
		{
			// Volume is the actual answer: placement-only would never reach
			// thousands of calls, pathfinding will.
			Debug::Log("[CommandBarExt] IsCellOccupied total=%d (last %s "
				"house=%d cell=%d,%d)\n", TotalCalls, id, house,
				pDestCell ? pDestCell->MapCoords.X : -1,
				pDestCell ? pDestCell->MapCoords.Y : -1);
		}
	}
}

DEFINE_HOOK(0x51BF90, InfantryClass_IsCellOccupied_PatherProbe, 0x5)
{
	GET(ObjectClass*, pThis, ECX);
	GET_STACK(CellClass*, pDestCell, 0x4);

	PatherProbe::Observe("inf", pThis, pDestCell);
	return 0;
}

DEFINE_HOOK(0x73F0A0, UnitClass_IsCellOccupied_PatherProbe, 0x6)
{
	GET(ObjectClass*, pThis, ECX);
	GET_STACK(CellClass*, pDestCell, 0x4);

	PatherProbe::Observe("veh", pThis, pDestCell);
	return 0;
}
