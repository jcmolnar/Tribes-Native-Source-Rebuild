#pragma once

#include "FearGuiPage.h"
#include "SimGuiTextFormat.h"

// WASM-PORT pt.40: the live Kronos RPG (InitObjectives in rpgfunk.cs) uses objNum 1..26 (lore lines +
// the House status table at 23..26), so 24 is too small -- lines 25/26 (House Arbal/Kronos) would be
// dropped or overflow teamList[].objectives[]. Bumped to 32 to cover the full set with headroom. This
// is a client-side array size only (TeamObjectiveEvent carries individual objNum+text pairs, not the
// whole array), so enlarging it does not change any wire format.
const size_t MAX_NUM_OBJECTIVES = 32;

namespace FearGui
{
	class FGHCommandObjectivePage : public FearGuiPage
	{
		typedef FearGuiPage Parent;

		DECLARE_PERSISTENT(FGHCommandObjectivePage);

		SimGui::TextFormat* objectives[MAX_NUM_OBJECTIVES];

	protected:
		bool onAdd() override;
		void onWake() override;
		void onPreRender() override;
		void onRender(GFXSurface* sfc, Point2I offset, const Box2I& updateRect) override;

	public:
		FGHCommandObjectivePage();
		virtual ~FGHCommandObjectivePage();

		void newObjective(int index, const char* text);
#ifdef __EMSCRIPTEN__
		// WASM-PORT: rebuild objectives[] from the authoritative teamList store. Called from BOTH onWake
		// (page opens) AND the TeamObjectiveEvent handler (a line lands later), so pull-based objective
		// lines arriving AFTER the page woke still render. Self-guards width so it never spins at extent.x==0.
		void refreshFromTeamList();
#endif
	};

#ifdef __EMSCRIPTEN__
	// WASM-PORT: live pointer to the on-canvas objectives page (set in onAdd, cleared in dtor).
	// The TeamObjectiveEvent handler uses this to populate+show the page when the event lands,
	// since the original "CmdObjectivesGui" named-object lookup never resolves in our shell.
	extern FGHCommandObjectivePage* g_wasmCmdObjPage;
#endif
}
