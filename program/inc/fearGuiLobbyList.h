#pragma once

// WASM-PORT: header reconstructed from FearGuiLobbyList.cpp usage (0.8.1 shipped an empty
// fearGuiLobbyList.h). FGLobbyList is the in-game player roster — an FGArrayCtrl that
// builds a flat, command-tree-ordered list of PlayerManager::ClientRep* and renders
// name/team/score columns. columnsResizeable/Sortable/numColumns/columnInfo/hFont/refresh/
// columnToSort/sortAscending are inherited from FGArrayCtrl; cellSize/selectedCell/setSize
// from SimGui::ArrayCtrl.

#include "FearGuiArrayCtrl.h"
#include "PlayerManager.h"

namespace FearGui
{
	class FGLobbyList : public FGArrayCtrl
	{
		typedef FGArrayCtrl Parent;

		DECLARE_PERSISTENT(FGLobbyList);

		// flat display order built each frame from the command tree; sized for headroom
		// over any real server's client count.
		PlayerManager::ClientRep* displayPlayers[256];
		int displayPlayersCount;

		void buildPlayerDisplayList(PlayerManager::ClientRep* cl);

	public:
		bool onAdd();
		void onWake();
		void onPreRender();
		char* getCellText(GFXSurface*, const Point2I& cell, const Point2I&, const Point2I&);
	};
}
