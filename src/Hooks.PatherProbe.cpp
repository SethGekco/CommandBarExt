// ---------------------------------------------------------------------------
// Per-house no-go zones: cells YOUR units refuse to path through, while every
// other player is completely unaffected.
//
// WHY THIS LEVER. The obvious one is wrong: CellClass::IsClearToMove
// (0x4834A0) takes (speedType, ignoreInfantry, ignoreVehicles, zone,
// movementZone, level, isBridge) — no unit, no house — so anything decided
// there is global and would block enemies too. It also has 22 call sites,
// several of them not movement at all (building placement among them), so
// filtering it would break where you can construct.
//
// ObjectClass::IsCellOccupied is a VIRTUAL, so `this` IS the unit asking.
// PROVEN in-game 2026-09-23 by the probe this file used to be:
//   20,594,000+ calls over 26,550 frames  (~776 per frame)
// and the samples are neighbourhood scans, not single destination checks —
// one E2 walking 71,165 / 72,161 / 70,163 / 71,163 / 71,164 / 70,165 /
// 69,165 / 69,164 / 69,163 in sequence. Pathfinding genuinely consults it,
// so answering Move::No here re-routes the path instead of merely refusing
// the final cell. No "current pather" stash is needed at all.
//
// PERFORMANCE. ~776 calls/frame means this must stay O(1) and, when unused,
// free. Order of the guards below is deliberate: the empty-zone test is a
// single load+branch and short-circuits the entire feature when no zone
// exists, which is the normal case. Never scan the zone list per call.
//
// HOOK SEATS. Phobos hooks INSIDE both implementations (0x51BFA2+6 and
// 0x73F0A7+9), so we take the entries, which end before those begin — no
// overlapping ranges, no same-address chaining:
//   InfantryClass::IsCellOccupied  0x51BF90 +5  (ends 0x51BF95)
//   UnitClass::IsCellOccupied      0x73F0A0 +6  (ends 0x73F0A6)
// At the entry ECX is `this` and nothing is pushed yet, so arg1 (pDestCell)
// is at [esp+4] and arg4 (pSourceCell) at [esp+0x10].
//
// EARLY RETURN. The function is thiscall with five stack args, so returning
// early means EAX = Move::No and a jump to a bare `ret 0x14`. 0x55AC10 is
// one, standalone between nops — verified by disassembly.
//
// ⚠ MULTIPLAYER: placement is currently LOCAL. Pathfinding is simulation
// state, so a zone one client knows about and another does not WILL desync.
// Skirmish only until placement is routed through the event queue — that is
// the next step, not an optional polish.
// ---------------------------------------------------------------------------

#include <ObjectClass.h>
#include <TechnoClass.h>
#include <FootClass.h>
#include <CellClass.h>
#include <HouseClass.h>
#include <MessageListClass.h>
#include <RulesClass.h>

#include <cwchar>
#include <unordered_map>
#include <vector>

#include "NoGoZone.h"

#include <Syringe.h>
#include <Utilities/Macro.h>
#include <Utilities/Debug.h>
#include <Helpers/Cast.h>

namespace NoGoZone
{
	struct Zone
	{
		CellStruct Center;
		int Radius;
		int HouseIndex;
	};

	// Until the button config lands in INI, one sensible default.
	static constexpr int DefaultRadius = 5;

	static std::vector<Zone> Zones;

	// cell key -> bitmask of houses that may NOT path through it. Rebuilt only
	// when a zone changes, so the hot path is one hash lookup and never a scan.
	static std::unordered_map<unsigned int, unsigned int> BlockedCells;

	static unsigned int KeyOf(const CellStruct& cell)
	{
		return (static_cast<unsigned int>(static_cast<unsigned short>(cell.X)) << 16)
			| static_cast<unsigned short>(cell.Y);
	}

	static void Rebuild()
	{
		BlockedCells.clear();

		for (const auto& zone : Zones)
		{
			if (zone.HouseIndex < 0 || zone.HouseIndex >= 32)
				continue;

			const unsigned int bit = 1u << zone.HouseIndex;
			const int r = zone.Radius;

			for (int dx = -r; dx <= r; ++dx)
			{
				for (int dy = -r; dy <= r; ++dy)
				{
					if (dx * dx + dy * dy > r * r)
						continue; // circular, not square

					CellStruct cell {
						static_cast<short>(zone.Center.X + dx),
						static_cast<short>(zone.Center.Y + dy) };

					BlockedCells[KeyOf(cell)] |= bit;
				}
			}
		}

		Debug::Log("[CommandBarExt] no-go: %d zone(s), %d blocked cell(s)\n",
			(int)Zones.size(), (int)BlockedCells.size());
	}

	static bool IsBlockedFor(const CellStruct& cell, int houseIndex)
	{
		if (houseIndex < 0 || houseIndex >= 32)
			return false;

		const auto it = BlockedCells.find(KeyOf(cell));
		return it != BlockedCells.end() && (it->second & (1u << houseIndex)) != 0;
	}

	// Shared by both hooks. Returns true when this unit must be refused.
	static bool ShouldRefuse(ObjectClass* pObject, CellClass* pDestCell)
	{
		// Hot-path short circuit: costs one load and one branch when the
		// feature is unused, which is almost always.
		if (BlockedCells.empty() || !pObject || !pDestCell)
			return false;

		auto pTechno = abstract_cast<TechnoClass*>(pObject);
		if (!pTechno || !pTechno->Owner)
			return false;

		const int house = pTechno->Owner->ArrayIndex;

		if (!IsBlockedFor(pDestCell->MapCoords, house))
			return false;

		// A unit that is already inside a zone must be able to walk out of
		// it, so only refuse cells when the unit is not standing in one.
		//
		// NOTE this is exactly why the first in-game test showed nothing:
		// the button centres the zone on a SELECTED unit, so that unit is
		// inside it and permanently exempt. Moving the anchor unit therefore
		// demonstrates nothing. Test by moving a DIFFERENT unit across.
		CellStruct here {};
		pObject->GetMapCoords(&here);
		if (IsBlockedFor(here, house))
			return false;

		// Decisive evidence that the gate actually fires. Budgeted: this is
		// a ~776-calls-per-frame path.
		static int refusalBudget = 30;
		if (refusalBudget > 0)
		{
			--refusalBudget;
			auto pType = pTechno->GetTechnoType();
			Debug::Log("[CommandBarExt] no-go: REFUSED %s (house %d) cell "
				"%d,%d\n", pType ? pType->ID : "?", house,
				pDestCell->MapCoords.X, pDestCell->MapCoords.Y);
		}

		return true;
	}

	void ToggleAtSelection()
	{
		auto pPlayer = HouseClass::CurrentPlayer;
		if (!pPlayer)
			return;

		FootClass* pAnchor = nullptr;
		for (const auto pObject : ObjectClass::CurrentObjects)
		{
			if (auto pFoot = abstract_cast<FootClass*>(pObject))
			{
				pAnchor = pFoot;
				break;
			}
		}

		// The first build gave no feedback at all — no cursor change, no
		// sound, nothing drawn — so a working zone was indistinguishable
		// from a dead button. Say something on screen.
		wchar_t message[128];

		if (!pAnchor)
		{
			Zones.clear();
			Rebuild();
			Debug::Log("[CommandBarExt] no-go: cleared (nothing selected)\n");

			swprintf(message, 128, L"No-go zones cleared.");
			MessageListClass::Instance->PrintMessage(message,
				RulesClass::Instance->MessageDelay,
				pPlayer->ColorSchemeIndex);
			return;
		}

		CellStruct center {};
		pAnchor->GetMapCoords(&center);

		Zones.push_back({ center, DefaultRadius, pPlayer->ArrayIndex });
		Rebuild();

		Debug::Log("[CommandBarExt] no-go: placed at %d,%d r=%d for house %d "
			"(LOCAL ONLY — skirmish, not MP-safe yet)\n",
			center.X, center.Y, DefaultRadius, pPlayer->ArrayIndex);

		swprintf(message, 128,
			L"No-go zone %d at %d,%d (r=%d). Move a unit from OUTSIDE across "
			L"it — the unit it was placed on is exempt.",
			(int)Zones.size(), center.X, center.Y, DefaultRadius);
		MessageListClass::Instance->PrintMessage(message,
			RulesClass::Instance->MessageDelay, pPlayer->ColorSchemeIndex);
	}
}

// --- The two per-unit gates -------------------------------------------------
// EAX = Move::No (7) then jump to a bare `ret 0x14` (thiscall, five stack
// args). Returning 0 instead lets the engine answer normally.

DEFINE_HOOK(0x51BF90, InfantryClass_IsCellOccupied_NoGoZone, 0x5)
{
	enum { RetGadget = 0x55AC10 };

	GET(ObjectClass*, pThis, ECX);
	GET_STACK(CellClass*, pDestCell, 0x4);

	if (NoGoZone::ShouldRefuse(pThis, pDestCell))
	{
		R->EAX(Move::No);
		return RetGadget;
	}

	return 0;
}

DEFINE_HOOK(0x73F0A0, UnitClass_IsCellOccupied_NoGoZone, 0x6)
{
	enum { RetGadget = 0x55AC10 };

	GET(ObjectClass*, pThis, ECX);
	GET_STACK(CellClass*, pDestCell, 0x4);

	if (NoGoZone::ShouldRefuse(pThis, pDestCell))
	{
		R->EAX(Move::No);
		return RetGadget;
	}

	return 0;
}
