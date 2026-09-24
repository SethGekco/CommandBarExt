// ---------------------------------------------------------------------------
// Phase 0: command-bar button plumbing probe.
//
// Question under test: can a Syringe DLL add a NEW named button to the
// tactical bar via UImd.ini's ButtonList, in Rex's 26-DLL stack?
//
// The plumbing, following Phobos PR#1993's pattern (open PR, NOT in the
// release Phobos we co-load, so all five seats are free — hooks.csv checked
// 2026-09-18):
//
//   1. name lookup   -- ShapeButtonClass::FindIndex (0x6CFCC0) resolves each
//                       ButtonList entry to a hardcoded button index. Vanilla
//                       owns IDs 0..11; the bar has 25 slots. Our hook at
//                       0x6CFD08 sits on the not-found epilogue: match our
//                       names first, else fall through to the vanilla return.
//   2. bar init      -- AdvancedCommandBarClass::Init (hook 0x6D0233).
//   3. click         -- Update dispatches by button index (hook 0x6D0827).
//   4. hold-down IO  -- InitButtonIO marks toggle buttons (hook 0x6D10DF).
//   5. tooltip       -- InitToolTip (hook 0x6D14DD).
//
// Vanilla button names, from gamemd's string table: Deploy, Guard, Stop,
// Team01, Team02, Team03, TypeSelect, PlanningMode, Beacon, Cheer,
// AttackMove. Button art is loaded as Button%02d.SHP (string verified in
// gamemd), so our ID 12 needs a loose button12.shp -- see assets/. Button
// SHPs carry Up/Down/Disabled frames (ShapeButtonFlag in YRpp).
// ---------------------------------------------------------------------------

#include <CommandClass.h>
#include <ShapeButtonClass.h>
#include <GameStrings.h>

#include <EventClass.h>
#include <FootClass.h>
#include <HouseClass.h>
#include <BulletClass.h>
#include <BulletTypeClass.h>
#include <MapClass.h>
#include <ObjectClass.h>
#include <WarheadTypeClass.h>
#include <Helpers/Cast.h>

#include <cstdarg>
#include <cstdio>
#include <vector>

#include <Commands/Commands.h> // Phobos submodule: MakeCommand<> + CATEGORY_*

#include <Syringe.h>
#include <Utilities/Macro.h>
#include <Utilities/Debug.h>

namespace CommandBarProbe
{
	// Vanilla bar owns button IDs 0..11 (11 in use + 1 unused); slots run to
	// 24. If Phobos PR#1993 ever merges into the build we co-load it will
	// claim ID 12 and these five seats -- re-base then.
	struct NewButton
	{
		const char* Name;    // ButtonList entry in uimd.ini
		const char* TipName; // tooltip label ("Tip:..." CSF entry, vanilla casing)
		bool HoldDown;       // toggle-style button (planning mode etc.)
		int ID;
	};

	static const NewButton Buttons[] =
	{
		{ "Hunt", "Tip:Hunt", false, 12 },
		{ "AggressiveStance", "Tip:AggStance", false, 13 },
		{ "EffectProbe", "Tip:EffectProbe", false, 14 },
	};

	static const NewButton* FromID(int id)
	{
		for (const auto& btn : Buttons)
			if (btn.ID == id)
				return &btn;
		return nullptr;
	}

	// Phase 1: Hunt = vanilla Mission::Hunt via MegaMission events, one per
	// selected mobile armed techno we own. Events carry the whole decision,
	// so this is multiplayer-synced by construction.
	// -----------------------------------------------------------------------
	// The 128-per-frame ceiling.
	//
	// EventClass::OutList is QueueClass<EventClass, 128>. Vanilla's own order
	// path checks `cmp [OutList.Count], 0x80; jge bail` at 0x646EF5 and drops
	// the event on the floor — no message, no retry. One click on a 128-unit
	// army therefore fills the entire outgoing queue for that frame, and
	// anything past 128 (or any other order issued the same frame, by us or
	// by the player) is silently lost. Rex's log caught it dead on:
	// selected=128 eligible=128 queued=128, OutList 0 -> 128.
	//
	// So don't dump everything at once: queue the orders and drain them over
	// successive frames, always leaving headroom below the drop threshold.
	// Ordering is preserved and only the local client ever adds its own
	// events, so this changes timing, not outcomes — sync-safe.
	//
	// Entries are TargetClass, not pointers: a unit that dies before its
	// order is sent resolves to nothing, exactly like a vanilla order queued
	// against a unit that dies in the same frame. No dangling reads.
	// -----------------------------------------------------------------------
	namespace HuntQueue
	{
		struct PendingOrder
		{
			TargetClass Whom;
			int HouseIndex;
		};

		static std::vector<PendingOrder> Pending;
		static size_t Cursor = 0;

		// Stay below vanilla's 0x80 drop point so other events still fit.
		static constexpr int OutListSoftCap = 112;

		static void Reset()
		{
			Pending.clear();
			Cursor = 0;
		}

		static int Flush()
		{
			int sent = 0;

			while (Cursor < Pending.size()
				&& EventClass::OutList.Count < OutListSoftCap)
			{
				const auto& order = Pending[Cursor];

				EventClass event(order.HouseIndex, order.Whom, Mission::Hunt,
					TargetClass(), TargetClass(), TargetClass());

				if (!EventClass::OutList.Add(event))
					break; // queue filled from elsewhere; resume next frame

				++Cursor;
				++sent;
			}

			if (Cursor >= Pending.size())
				Reset();

			return sent;
		}
	}

	static void ExecuteHunt()
	{
		// Rex reports a repro where only ONE of N selected units went
		// hunting. Three suspects, and this logging separates them:
		//   filter  -- eligible < selected means our own gate dropped units
		//              (also logs WHY per skipped unit);
		//   queue   -- OutList.Add returns false silently when its 128
		//              slots are full (queued vs eligible mismatch);
		//   engine  -- all adds succeed yet units stay put: the MegaMission
		//              pre-send merge folds identical events into one
		//              whom-list entry, and the expansion is the suspect.
		const int selected = ObjectClass::CurrentObjects.Count;
		const int outBefore = EventClass::OutList.Count;
		int eligible = 0;
		int queued = 0;

		for (const auto pObject : ObjectClass::CurrentObjects)
		{
			auto pFoot = abstract_cast<FootClass*>(pObject);
			if (!pFoot || pFoot->Berzerk || !pFoot->IsArmed()
				|| !pFoot->Owner->IsControlledByCurrentPlayer())
			{
				if (auto pTechno = abstract_cast<TechnoClass*>(pObject))
				{
					Debug::Log("[CommandBarExt] Hunt skip %s: foot=%d "
						"berzerk=%d armed=%d mine=%d\n",
						pTechno->GetTechnoType()->ID,
						pFoot != nullptr,
						pTechno->Berzerk, pTechno->IsArmed(),
						pTechno->Owner->IsControlledByCurrentPlayer());
				}
				continue;
			}

			++eligible;

			// Per-type census: "specific types stop working" is only
			// answerable if the log says which types were ordered. Only
			// infantry (0x51F60C) and NON-deploying vehicles (0x73F08C)
			// delegate to FootClass::Mission_Hunt, so aircraft and
			// DeploysInto= vehicles never reach our fallback at all.
			if (eligible <= 40)
			{
				Debug::Log("[CommandBarExt] Hunt order %s (rtti %d, "
					"mission %d)\n", pFoot->GetTechnoType()->ID,
					(int)pFoot->WhatAmI(), (int)pFoot->CurrentMission);
			}

			HuntQueue::Pending.push_back(
				{ TargetClass(pFoot), HouseClass::CurrentPlayer->ArrayIndex });

			// Voice feedback from the first unit ordered, vanilla-style.
			if (eligible == 1)
			{
				auto pType = pFoot->GetTechnoType();
				TypeList<int>& voices = pType->VoiceAttack.Count
					? pType->VoiceAttack : pType->VoiceMove;
				if (voices.Count)
					pFoot->QueueVoice(voices.GetItem(0));
			}
		}

		// Send what fits now; the frame hook drains the rest.
		queued = HuntQueue::Flush();

		Debug::Log("[CommandBarExt] Hunt: selected=%d eligible=%d sent=%d "
			"deferred=%d OutList %d -> %d\n", selected, eligible, queued,
			(int)(HuntQueue::Pending.size() - HuntQueue::Cursor),
			outBefore, EventClass::OutList.Count);
	}

	// Aggressive Stance: no cross-DLL linkage -- YRAggressiveStance registers
	// its command in the shared CommandClass::Array; find it by name at
	// runtime and fire it. Degrades to a logged no-op if that DLL is absent.
	static void ExecuteNamedCommand(const char* name)
	{
		for (const auto pCommand : CommandClass::Array)
		{
			if (pCommand && _strcmpi(pCommand->GetName(), name) == 0)
			{
				pCommand->Execute(static_cast<WWKey>(0));
				return;
			}
		}
		Debug::Log("[CommandBarExt] command '%s' not found in "
			"CommandClass::Array -- is its DLL loaded?\n", name);
	}

	// -----------------------------------------------------------------------
	// Phase 3 probe: can a bar button inflict Phobos AttachEffects?
	//
	// We cannot call Phobos' AttachEffect code — its 1193 exports are all
	// Syringe hook stubs, there is no API to link against. But Phobos applies
	// AttachEffects from a WarheadType, off its own hook on
	// MapClass::DamageArea (0x489286 -> WarheadTypeExt::Detonate ->
	// ApplyAttachEffects). So detonating an INI-named warhead on the unit
	// gets the whole feature set with zero coupling to Phobos internals.
	//
	// The gate that path checks, WarheadTypeExt::InDamageArea, is initialised
	// to true and only cleared transiently while a bullet detonates, so a
	// direct DamageArea call from here should qualify. That is the specific
	// claim this probe tests.
	//
	// rulesmd carries the matching test content (appended 2026-09-21):
	// [CBEProbeWH] AttachEffect.AttachTypes=CBEProbeEffect, registered as
	// 106= in the real [Warheads] list, granting a green tint + 1.5x speed
	// and firepower for ~30s. Unmistakable on screen if it works.
	//
	// PROBE ONLY: this applies the warhead locally instead of going through
	// the event queue, which is fine for a skirmish and wrong for
	// multiplayer. The real feature routes it through a synced event like
	// everything else — see DESIGN.md.
	// -----------------------------------------------------------------------
	static void ExecuteEffectProbe()
	{
		// Deliberately NOT a WeaponType: gamemd has no [WeaponTypes] or
		// [Projectiles] list (only "Warheads" exists as a list string), so
		// weapons and projectiles are parsed on demand at their referencing
		// site. A brand-new [CBEProbeWeapon] section would allocate empty and
		// silently do nothing. InvisibleAll is already parsed (17 weapons use
		// it) and CBEProbeWH is parsed because it IS in the [Warheads] list.
		auto pProjectile = BulletTypeClass::FindOrAllocate("InvisibleAll");
		auto pWarhead = WarheadTypeClass::FindOrAllocate("CBEProbeWH");

		if (!pProjectile || !pWarhead)
		{
			Debug::Log("[CommandBarExt] EffectProbe: projectile/warhead "
				"lookup failed — is CBEProbeWH listed in [Warheads]?\n");
			return;
		}

		int affected = 0;

		for (const auto pObject : ObjectClass::CurrentObjects)
		{
			auto pTechno = abstract_cast<TechnoClass*>(pObject);
			if (!pTechno || !pTechno->Owner
				|| !pTechno->Owner->IsControlledByCurrentPlayer())
			{
				continue;
			}

			// Bullet aimed at the techno itself: Phobos' route 2 detonates on
			// pBullet->Target and nothing else, so a unit parked on a bridge,
			// inside a building's footprint or sharing a cell cannot leak the
			// effect onto its neighbours.
			auto pBullet = pProjectile->CreateBullet(pTechno, pTechno,
				0 /*damage: we want the effect, not the hit*/, pWarhead,
				100 /*speed*/, false /*bright*/);

			if (!pBullet)
				continue;

			// Route 2 also range-checks the bullet against its target
			// (<= 64 leptons), so the bullet must actually BE at the unit.
			const CoordStruct coords = pTechno->GetCoords();
			pBullet->SetLocation(coords);
			pBullet->Detonate(coords);
			pBullet->UnInit();

			++affected;
		}

		Debug::Log("[CommandBarExt] EffectProbe: detonated CBEProbeWH on %d "
			"targeted unit(s) — expect a green tint ONLY on those units\n",
			affected);
	}

	static void ExecuteButton(int id)
	{
		switch (id)
		{
		case 12: ExecuteHunt(); break;
		case 13: ExecuteNamedCommand("AggressiveStance"); break;
		case 14: ExecuteEffectProbe(); break;
		}
	}
}

// --- 1. Name lookup -------------------------------------------------------
// 0x6CFD08 is the not-found epilogue of ShapeButtonClass::FindIndex:
//   6CFD08: a1 cc 27 84 00   mov eax,[0x8427CC]   (default index)
//   6CFD0D: 5f 5e 5d 5b c3   pop edi/esi/ebp/ebx; ret
// The name argument arrives in ECX (__fastcall) and survives to the
// epilogue. On a match we set EAX ourselves and resume at 0x6CFD0D; on no
// match, return 0 re-runs the stolen mov (an idempotent read -- safe).
DEFINE_HOOK(0x6CFD08, ShapeButtonClass_FindIndex_NewButtons, 0x5)
{
	enum { SkipDefaultIndex = 0x6CFD0D };

	GET(const char*, name, ECX);

	// Free recon: FindIndex runs once per ButtonList entry at bar init, so a
	// short log window records what the parser actually queries without
	// risking hot-path spam later.
	static int logBudget = 32;
	if (logBudget > 0 && name)
	{
		--logBudget;
		Debug::Log("[CommandBarExt] FindIndex('%s')\n", name);
	}

	if (name)
	{
		for (const auto& btn : CommandBarProbe::Buttons)
		{
			if (_strcmpi(name, btn.Name) == 0)
			{
				Debug::Log("[CommandBarExt] FindIndex('%s') -> ID %d\n",
					name, btn.ID);
				R->EAX(btn.ID);
				return SkipDefaultIndex;
			}
		}
	}

	return 0;
}

// --- 2. Bar init ----------------------------------------------------------
// One-shot banner: proves the AdvancedCommandBar hooks are live even if the
// ButtonList never mentions us (ScatterExt lesson: an empty log cannot
// distinguish "no hooks ran" from "nothing to report").
DEFINE_HOOK(0x6D0233, AdvancedCommandBar_Init_NewButtons, 0x6)
{
	Debug::Log("[CommandBarExt] bar Init reached\n");
	return 0;
}

// --- 3. Click dispatch ----------------------------------------------------
// Update compares the clicked button's index against a global at 0xB0CB38;
// indices it does not know fall through to 0x6D0995. We only observe.
DEFINE_HOOK(0x6D0827, AdvancedCommandBar_Update_NewButtonClicked, 0x6)
{
	GET(const int, index, EAX);

	// Phase 0.1 calibration: the first run proved everything up to the click,
	// but a click on our button never logged. Log EVERY index that reaches
	// this dispatcher (budgeted) so a click on a KNOWN button (Deploy) shows
	// whether EAX here is the button index or something else entirely.
	static int dispatchBudget = 64;
	if (dispatchBudget > 0)
	{
		--dispatchBudget;
		Debug::Log("[CommandBarExt] Update dispatch: EAX=%d\n", index);
	}

	if (auto pBtn = CommandBarProbe::FromID(index))
	{
		auto pShape = ShapeButtonClass::GetButton(index);
		Debug::Log("[CommandBarExt] button %d ('%s') clicked, IsOn=%d\n",
			index, pBtn->Name, pShape ? pShape->IsOn : -1);
		CommandBarProbe::ExecuteButton(index);
	}

	return 0;
}

// --- 4. Hold-down IO ------------------------------------------------------
// Also our best probe for "did vanilla actually build a ShapeButtonClass for
// an ID it has no hardcoded art table entry for?" -- if shape=N here, the
// button needs its button12.shp before it can render.
DEFINE_HOOK(0x6D10DF, AdvancedCommandBar_InitButtonIO_NewButtons, 0x6)
{
	for (const auto& btn : CommandBarProbe::Buttons)
	{
		auto pShape = ShapeButtonClass::GetButton(btn.ID);
		Debug::Log("[CommandBarExt] InitButtonIO: button %d ('%s') shape=%s\n",
			btn.ID, btn.Name, pShape ? "Y" : "N");

		if (pShape && btn.HoldDown)
		{
			pShape->ToggleType = 1;
			pShape->UseFlash = true;
		}
	}

	return 0;
}

// --- 5. Tooltip -----------------------------------------------------------
DEFINE_HOOK(0x6D14DD, AdvancedCommandBar_InitToolTip_NewButtons, 0x5)
{
	for (const auto& btn : CommandBarProbe::Buttons)
	{
		if (auto pShape = ShapeButtonClass::GetButton(btn.ID))
			ShapeButtonClass::SetToolTip(pShape, btn.TipName);
	}

	// Phase 0.1 diagnosis: dump our gadget's state next to a known-good
	// button's (Deploy). Whatever differs -- Disabled, input Flags, control
	// ID, list linkage -- is the reason clicks never dispatched for us.
	auto dump = [](const char* tag, int index)
	{
		auto p = ShapeButtonClass::GetButton(index);
		if (!p)
		{
			Debug::Log("[CommandBarExt] state %s(idx %d): NULL\n", tag, index);
			return;
		}
		Debug::Log("[CommandBarExt] state %s(idx %d): ID=%d pos=%d,%d %dx%d "
			"Disabled=%d Flags=%X Sticky=%d shpLoaded=%d next=%p prev=%p\n",
			tag, index, p->ID, p->X, p->Y, p->Width, p->Height,
			p->Disabled, (unsigned int)p->Flags, p->IsSticky,
			p->IsShapeLoaded, (void*)p->GetNext(), (void*)p->GetPrev());
	};
	dump("Deploy", ShapeButtonClass::FindIndex("Deploy"));
	dump("Hunt", 12);

	return 0;
}

// ---------------------------------------------------------------------------
// Hunt fallback for player-owned units — ROOT CAUSE VERIFIED 2026-09-20.
//
// Symptom: every selected unit accepts Mission::Hunt (the in-game log shows
// all 119 sitting on mission 15 on the next click), yet most never move.
//
// Why, from gamemd: FootClass::Mission_Hunt (0x4D5350) only acts when the
// unit can auto-acquire a target right now (the scan at 0x709820). When that
// scan fails it drops to the fallback at 0x4D54EE, which asks
// 0x50B730 "is this house player-controlled?" and then:
//
//   AI house    -> 0x4D5506: take the *human player's* base centre
//                  (HouseClass @ 0xA83D4C, base cell via 0x50DEF0),
//                  SetDestination(cell) + ForceMission(Move). This is how
//                  AI hunters cross the map — and why rulesmd's StupidHunt
//                  comment reads "should just run towards the player".
//   HUMAN house -> 0x4D5578: idle. Nothing. By design: Hunt is an AI mission.
//
// So vanilla Hunt for a player is "shoot what is already in range", never
// "go find them". The fix reuses the engine's own fallback, but aimed at an
// ENEMY base instead of the local player's:
//
//   hook A (0x4D54EE) — for player-controlled owners, skip BOTH bails and
//                       resume at 0x4D5506 so the engine does the work;
//   hook B (0x4D5514) — swap the hardcoded local-player house in ECX for the
//                       enemy house we picked.
//
// Determinism (this is sim code, it must not desync): the house is chosen by
// walking HouseClass::Array in index order and taking the nearest enemy base
// centre, ties broken by the lower array index. No local-player state, no
// unsynced randomness. AI units are untouched — hook A returns 0 for them,
// so their vanilla behaviour (and the game's balance) is unchanged.
// ---------------------------------------------------------------------------

namespace HuntFix
{
	// 0x50DEF0 — HouseClass::GetBaseCenter(CellStruct* out) -> CellStruct*.
	// __fastcall with a dummy EDX is the safe way to call a thiscall game
	// address (memory: __stdcall corrupts 4 bytes of stack per call).
	using GetBaseCenter_t = CellStruct* (__fastcall*)(HouseClass*, void*, CellStruct*);
	static const auto GetBaseCenter = reinterpret_cast<GetBaseCenter_t>(0x50DEF0);

	// Per-frame census, reported and cleared by the frame hook.
	static int FallbackThisFrame = 0;
	static int RetargetedThisFrame = 0;

	// Last run logged NEITHER the census nor a drain line. Both live in the
	// frame hook, so that is consistent with two very different stories:
	// the fallback never firing, or 0x55B6FC never executing. Log straight
	// from the fallback itself so the two can never be confused again.
	static void LogBudgeted(const char* format, ...)
	{
		static int budget = 40;
		if (budget <= 0)
			return;
		--budget;

		char message[256];
		va_list args;
		va_start(args, format);
		vsnprintf(message, sizeof(message), format, args);
		va_end(args);

		Debug::Log("[CommandBarExt] hunt fallback: %s\n", message);
	}

	static bool IsHuntableEnemy(HouseClass* pOwner, HouseClass* pHouse)
	{
		return pHouse && pHouse != pOwner && !pHouse->Defeated
			&& !pHouse->IsNeutral() && !pHouse->IsObserver()
			&& !pOwner->IsAlliedWith(pHouse);
	}

	// Rex, after testing: "hunt rarely goes after nearest targets". Correct —
	// and it was our doing. Redirecting the engine's fallback only let us
	// choose a HOUSE, and the engine then walks to that house's base centre.
	// A unit would march past three enemy tanks to reach a distant base.
	//
	// So pick the nearest enemy OBJECT instead and drive to it ourselves.
	// Deterministic: TechnoClass::Array in index order, squared distance,
	// strictly-less so the lowest index wins ties. No local-player state, no
	// unsynced RNG.
	static TechnoClass* PickNearestEnemy(FootClass* pFoot)
	{
		auto pOwner = pFoot->Owner;
		if (!pOwner)
			return nullptr;

		CellStruct here {};
		pFoot->GetMapCoords(&here);

		TechnoClass* pBest = nullptr;
		int bestDistance = 0;

		for (const auto pTechno : TechnoClass::Array)
		{
			if (!pTechno || pTechno == pFoot || pTechno->InLimbo
				|| !pTechno->IsAlive || !pTechno->Health)
			{
				continue;
			}

			if (!IsHuntableEnemy(pOwner, pTechno->Owner))
				continue;

			CellStruct there {};
			pTechno->GetMapCoords(&there);

			const int dx = there.X - here.X;
			const int dy = there.Y - here.Y;
			const int distance = dx * dx + dy * dy;

			if (!pBest || distance < bestDistance)
			{
				pBest = pTechno;
				bestDistance = distance;
			}
		}

		return pBest;
	}
}

// Drains deferred Hunt orders, one batch per logic frame. 0x55B6FC sits at a
// join point inside LogicClass::Update, between Phobos' AI_After (0x55B6B3)
// and Kratos' Update_Late (0x55B719) — both reachable paths converge here, so
// it runs every frame, and the 8 stolen bytes are a single `mov [esp+0x10],
// imm32` (absolute immediate, not a relative branch, so `return 0` is safe).
DEFINE_HOOK(0x55B6FC, LogicClass_Update_DrainHuntQueue, 0x8)
{
	if (!HouseClass::CurrentPlayer)
	{
		CommandBarProbe::HuntQueue::Reset(); // scenario ended mid-drain
		return 0;
	}

	// Unconditional heartbeat: last run logged nothing from this hook, which
	// could equally mean "nothing to report" or "this seat never executes".
	// Never leave those two indistinguishable again.
	static int heartbeat = 0;
	if (++heartbeat % 450 == 0)
		Debug::Log("[CommandBarExt] frame hook alive (tick %d)\n", heartbeat);

	if (const int sent = CommandBarProbe::HuntQueue::Flush())
	{
		Debug::Log("[CommandBarExt] Hunt drain: sent=%d deferred=%d\n", sent,
			(int)(CommandBarProbe::HuntQueue::Pending.size()
				- CommandBarProbe::HuntQueue::Cursor));
	}

	// Per-frame census of units our fallback is actually driving, so the next
	// in-game run says outright whether a ceiling remains and where it bites.
	if (HuntFix::FallbackThisFrame)
	{
		Debug::Log("[CommandBarExt] Hunt fallback: units=%d retargeted=%d\n",
			HuntFix::FallbackThisFrame, HuntFix::RetargetedThisFrame);
		HuntFix::FallbackThisFrame = 0;
		HuntFix::RetargetedThisFrame = 0;
	}

	return 0;
}

DEFINE_HOOK(0x4D54EE, FootClass_Mission_Hunt_PlayerSeeksEnemy, 0x6)
{
	// 0x4D5582 is the function's own "done" join point: it computes and
	// returns the re-check delay. Doing the work ourselves and landing there
	// keeps us on the engine's normal exit path.
	enum { Done = 0x4D5582, Vanilla = 0 };

	GET(FootClass*, pFoot, ESI);

	if (!pFoot || !pFoot->Owner || !pFoot->Owner->IsControlledByHuman())
		return Vanilla;

	++HuntFix::FallbackThisFrame;

	auto pEnemy = HuntFix::PickNearestEnemy(pFoot);
	if (!pEnemy)
	{
		// Nothing left to hunt — idle exactly like vanilla rather than
		// inventing behaviour.
		HuntFix::LogBudgeted("no enemy found for %s",
			pFoot->GetTechnoType()->ID);
		return Vanilla;
	}

	// DEFINE_REFERENCE yields a reference, not a pointer — same trap as
	// ObjectClass::CurrentObjects and CommandClass::Array. Use '.'.
	auto pCell = MapClass::Instance.TryGetCellAt(pEnemy->GetCoords());
	if (!pCell)
		return Vanilla;

	++HuntFix::RetargetedThisFrame;

	HuntFix::LogBudgeted("%s -> nearest enemy %s",
		pFoot->GetTechnoType()->ID, pEnemy->GetTechnoType()->ID);

	pFoot->SetDestination(pCell, true);
	pFoot->ForceMission(Mission::Move);

	return Done;
}

// --- 6. Command registration ----------------------------------------------
// Registered as a CommandClass too, so "Hunt Units" appears in the hotkey
// configuration dialog for free. Phase 0 Execute is log-only; Phase 1 issues
// the vanilla Mission::Hunt MegaMission events.
class HuntUnitsCommandClass : public CommandClass
{
public:
	virtual const char* GetName() const override
	{
		return "HuntUnits";
	}

	virtual const wchar_t* GetUIName() const override
	{
		return L"Hunt Units";
	}

	virtual const wchar_t* GetUICategory() const override
	{
		return CATEGORY_CONTROL;
	}

	virtual const wchar_t* GetUIDescription() const override
	{
		return L"Send the selected units to hunt down enemies.";
	}

	virtual void Execute(WWKey eInput) const override
	{
		Debug::Log("[CommandBarExt] HuntUnits command executed\n");
		CommandBarProbe::ExecuteHunt();
	}
};

DEFINE_HOOK(0x533066, CommandClassCallback_Register_CommandBarExt, 0x6)
{
	MakeCommand<HuntUnitsCommandClass>();
	// Liveness proof for the third same-address chain -- see file header.
	Debug::Log("[CommandBarExt] command 'HuntUnits' registered\n");
	return 0;
}
