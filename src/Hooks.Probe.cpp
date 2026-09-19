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
	};

	static const NewButton* FromID(int id)
	{
		for (const auto& btn : Buttons)
			if (btn.ID == id)
				return &btn;
		return nullptr;
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

	if (auto pBtn = CommandBarProbe::FromID(index))
	{
		auto pShape = ShapeButtonClass::GetButton(index);
		Debug::Log("[CommandBarExt] button %d ('%s') clicked, IsOn=%d\n",
			index, pBtn->Name, pShape ? pShape->IsOn : -1);
		// Phase 1 wires this to the matching CommandClass. Probe: log only.
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

	return 0;
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
		Debug::Log("[CommandBarExt] HuntUnits command executed (probe: no-op)\n");
	}
};

DEFINE_HOOK(0x533066, CommandClassCallback_Register_CommandBarExt, 0x6)
{
	MakeCommand<HuntUnitsCommandClass>();
	// Liveness proof for the third same-address chain -- see file header.
	Debug::Log("[CommandBarExt] command 'HuntUnits' registered\n");
	return 0;
}
