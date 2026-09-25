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
// and the samples are neighbourhood scans, not single destination checks, so
// answering Move::No here re-routes the path instead of merely refusing the
// final cell. No "current pather" stash is needed at all.
//
// PERFORMANCE. ~776 calls/frame means this must stay O(1) and, when unused,
// free. The empty-zone test is a single load+branch and short-circuits the
// whole feature. Never scan the zone list per call.
//
// HOOK SEATS
//   InfantryClass::IsCellOccupied  0x51BF90 +5  (Phobos sits at 0x51BFA2+6)
//   UnitClass::IsCellOccupied      0x73F0A0 +6  (Phobos sits at 0x73F0A7+9)
//   DisplayClass::LeftMouseButtonUp 0x4AB9B0 +5 — entry, found by scanning
//     back from Antares' 0x4AC20C to int3 padding. `sub esp,0x7c; push ebx;
//     push ebp` is exactly 5 bytes with no relative branch.
//   TacticalClass radial draw       0x6DBE74 +7 (3rd chain, Phobos x2)
// Entry seats leave ECX = this with nothing pushed yet, so the args sit at
// [esp+4] onward.
//
// LeftMouseButtonUp's signature is already in YRpp:
//   (const CoordStruct& coords, const CellStruct& cell, ObjectClass* pObject,
//    Action action, DWORD)
// Confirmed against the disassembly: at 0x4AB9B6 the function loads
// [esp+0x94] — after sub 0x7c plus three pushes plus the return address, that
// is arg3 — and immediately virtual-calls it, which only makes sense for
// pObject. So arg2 at [esp+8] is the clicked cell, which is what placement
// needs. Five stack args means an early return is a jump to a bare `ret 0x14`
// (0x55AC10, standalone between nops).
//
// ⚠ MULTIPLAYER: placement is still LOCAL and therefore desyncs. Skirmish
// only. Vanilla beacon placement is evented (EventClass type 0x12, built at
// 0x4AC22C) — that is the template to copy next.
// ---------------------------------------------------------------------------

#include <ObjectClass.h>
#include <TechnoClass.h>
#include <FootClass.h>
#include <CellClass.h>
#include <HouseClass.h>
#include <MessageListClass.h>
#include <RulesClass.h>
#include <MapClass.h>
#include <Unsorted.h>

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

		// Optional anchor: when set, the zone follows this unit. Once
		// placement is evented this costs no extra traffic — every client
		// already simulates the anchor identically, so only its IDENTITY has
		// to travel, never its path.
		TechnoClass* Anchor;
	};

	// Until button config lands in INI, one sensible default.
	static constexpr int DefaultRadius = 5;

	std::vector<Zone> Zones; // non-static: the draw hook reads it

	// cell key -> bitmask of houses that may NOT path through it. Rebuilt only
	// when a zone changes, so the hot path is one hash lookup, never a scan.
	static std::unordered_map<unsigned int, unsigned int> BlockedCells;

	// Armed by the click-to-place tool; consumed by the next map click.
	bool PlacementArmed = false; // read by the click hook

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
	}

	static bool IsBlockedFor(const CellStruct& cell, int houseIndex)
	{
		if (houseIndex < 0 || houseIndex >= 32)
			return false;

		const auto it = BlockedCells.find(KeyOf(cell));
		return it != BlockedCells.end() && (it->second & (1u << houseIndex)) != 0;
	}

	// Index of this player's zone containing the cell, or -1.
	static int ZoneAt(const CellStruct& cell, int houseIndex)
	{
		for (size_t i = 0; i < Zones.size(); ++i)
		{
			const auto& zone = Zones[i];
			if (zone.HouseIndex != houseIndex)
				continue;

			const int dx = cell.X - zone.Center.X;
			const int dy = cell.Y - zone.Center.Y;
			if (dx * dx + dy * dy <= zone.Radius * zone.Radius)
				return static_cast<int>(i);
		}
		return -1;
	}

	static void Announce(const wchar_t* text)
	{
		if (auto pPlayer = HouseClass::CurrentPlayer)
		{
			MessageListClass::Instance.PrintMessage(text,
				RulesClass::Instance->MessageDelay, pPlayer->ColorSchemeIndex);
		}
	}

	// Place at a cell, or remove the zone already covering it. Shared by the
	// at-unit tool and the click tool so both behave identically.
	void PlaceOrRemoveAt(const CellStruct& cell, TechnoClass* pAnchor)
	{
		auto pPlayer = HouseClass::CurrentPlayer;
		if (!pPlayer)
			return;

		const int existing = ZoneAt(cell, pPlayer->ArrayIndex);
		if (existing >= 0)
		{
			Zones.erase(Zones.begin() + existing);
			Rebuild();
			Debug::Log("[CommandBarExt] no-go: removed zone at %d,%d "
				"(%d left)\n", cell.X, cell.Y, (int)Zones.size());
			Announce(L"No-go zone removed.");
			return;
		}

		Zones.push_back({ cell, DefaultRadius, pPlayer->ArrayIndex, pAnchor });
		Rebuild();

		Debug::Log("[CommandBarExt] no-go: placed at %d,%d r=%d house %d "
			"anchor=%s (LOCAL ONLY — skirmish, not MP-safe yet)\n",
			cell.X, cell.Y, DefaultRadius, pPlayer->ArrayIndex,
			pAnchor ? "yes" : "no");

		Announce(pAnchor
			? L"No-go zone placed — it will follow that unit."
			: L"No-go zone placed. Click it again to remove it.");
	}

	// Shared by both path gates. True when this unit must be refused.
	static bool ShouldRefuse(ObjectClass* pObject, CellClass* pDestCell)
	{
		// Hot-path short circuit: one load and one branch when unused.
		if (BlockedCells.empty() || !pObject || !pDestCell)
			return false;

		auto pTechno = abstract_cast<TechnoClass*>(pObject);
		if (!pTechno || !pTechno->Owner)
			return false;

		const int house = pTechno->Owner->ArrayIndex;

		if (!IsBlockedFor(pDestCell->MapCoords, house))
			return false;

		// A unit already inside a zone must be able to walk out, so only
		// refuse when it is not currently standing in one. This is also why
		// the very first in-game test looked dead: the at-unit tool centres
		// the zone on a selected unit, making that unit permanently exempt.
		CellStruct here {};
		pObject->GetMapCoords(&here);
		if (IsBlockedFor(here, house))
			return false;

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

	// --- Public tools -------------------------------------------------------

	void ToggleAtSelection(bool follow)
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

		if (!pAnchor)
		{
			Zones.clear();
			Rebuild();
			Debug::Log("[CommandBarExt] no-go: cleared all\n");
			Announce(L"No-go zones cleared.");
			return;
		}

		CellStruct center {};
		pAnchor->GetMapCoords(&center);
		PlaceOrRemoveAt(center, follow ? pAnchor : nullptr);
	}

	void EnterPlacementMode()
	{
		PlacementArmed = !PlacementArmed;

		// Borrow the beacon cursor purely for the visual: our hook sits at
		// the ENTRY of LeftMouseButtonUp, so we consume the click before any
		// beacon logic can run.
		MapClass::Instance.SetPlaceBeaconMode(PlacementArmed ? 1 : 0);

		Debug::Log("[CommandBarExt] no-go: placement mode %s\n",
			PlacementArmed ? "ARMED" : "cancelled");
		Announce(PlacementArmed
			? L"No-go zone: click the map to place, or click an existing zone to remove it."
			: L"No-go zone placement cancelled.");
	}

	void UpdateAnchored()
	{
		if (Zones.empty())
			return;

		bool dirty = false;

		for (size_t i = Zones.size(); i-- > 0; )
		{
			auto pAnchor = Zones[i].Anchor;
			if (!pAnchor)
				continue;

			// Pointer safety: a dead unit's memory is pooled and reused, so
			// reading IsAlive off a freed object returns garbage. Validate by
			// membership in TechnoClass::Array instead — a few hundred
			// comparisons per anchored zone per frame, nothing beside the
			// ~776 path queries/frame this file already lives with.
			bool alive = false;
			for (const auto pTechno : TechnoClass::Array)
			{
				if (pTechno == pAnchor) { alive = true; break; }
			}

			if (!alive)
			{
				Zones.erase(Zones.begin() + i); // anchor died, zone goes too
				dirty = true;
				continue;
			}

			// A limboed anchor (garrisoned, or riding a transport) has no map
			// presence at all, so hold position rather than follow the
			// transport or freeze somewhere stale.
			if (pAnchor->InLimbo)
				continue;

			CellStruct now {};
			pAnchor->GetMapCoords(&now);

			if (now.X != Zones[i].Center.X || now.Y != Zones[i].Center.Y)
			{
				Zones[i].Center = now;
				dirty = true;
			}
		}

		if (dirty)
			Rebuild();
	}
}

// --- Click capture ----------------------------------------------------------
// Entry hook, so we see the click before every mode-specific block further
// down the function (including the beacon one whose cursor we borrowed).
DEFINE_HOOK(0x4AB9B0, DisplayClass_LeftMouseButtonUp_NoGoZone, 0x5)
{
	enum { RetGadget = 0x55AC10, Vanilla = 0 };

	if (!NoGoZone::PlacementArmed)
		return Vanilla;

	GET_STACK(CellStruct*, pCell, 0x8); // arg2 — see file header
	if (!pCell)
		return Vanilla;

	NoGoZone::PlacementArmed = false;
	MapClass::Instance.SetPlaceBeaconMode(0); // release the borrowed cursor

	NoGoZone::PlaceOrRemoveAt(*pCell, nullptr);

	return RetGadget; // consume the click; void function, five stack args
}

// --- Visualiser -------------------------------------------------------------
// A zone you cannot see is unusable — the first test read as a dead button
// purely because nothing was drawn.
//
// Rendering is client-side and unsynced, which is what we want: draw only the
// LOCAL player's zones. A no-go zone is private planning information and must
// not leak to opponents — which also rules out spawning animations, since
// everyone would see those.
DEFINE_HOOK(0x6DBE74, TacticalClass_DrawRadialIndicators_NoGoZones, 0x7)
{
	auto pPlayer = HouseClass::CurrentPlayer;
	if (!pPlayer || NoGoZone::Zones.empty())
		return 0;

	for (const auto& zone : NoGoZone::Zones)
	{
		if (zone.HouseIndex != pPlayer->ArrayIndex)
			continue; // never reveal another house's zones

		CoordStruct coords = CellClass::Cell2Coord(zone.Center);
		coords.Z = MapClass::Instance.GetCellFloorHeight(coords);

		// Anchored zones read amber so they are distinguishable at a glance
		// from static ones.
		ColorStruct color = zone.Anchor
			? ColorStruct { 255, 170, 40 }
			: ColorStruct { 255, 40, 40 };

		Game::DrawRadialIndicator(false, true, coords, color,
			static_cast<float>(zone.Radius), false, true);
	}

	return 0;
}

// --- The two per-unit path gates --------------------------------------------
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
