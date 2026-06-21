#pragma once

// WASM-PORT: ServerListCtrl reconstruction. The 0.8.1 source shipped this class's .cpp
// entirely commented out AND its header derived from SimGui::Control with no data members —
// yet the commented .cpp used FGArrayCtrl features (cellSize/headerDim/hFont/columnInfo) and a
// `servers` list. So the original was an FGArrayCtrl-derived 1-column list (setSize(1,N)); each
// row's single cell renders the full server line via getCellText. This is a MINIMAL faithful
// reconstruction: it shows the live servers in JoinGame.gui, supports row selection + Join, but
// drops sorting/filtering/favorites/file-persistence (those methods are kept as no-op stubs so
// dlgJoinGame.cpp + FearCSDelegate.cpp link unchanged). Reads servers straight from
// cg.csDelegate->mServerList (no second copy) so there's no nested-Vector copy to worry about.
// Persist tag is FOURCC('F','G','s','L') (from the original IMPLEMENT_PERSISTENT_TAG) so the
// JoinGame.gui control node resolves to this class.

#include "FearGuiArrayCtrl.h"
#include "FearCSDelegate.h"

namespace SimGui
{
	class JoinGameDelegate;
}


namespace FearGui
{
	class ServerListCtrl : public FGArrayCtrl
	{
		typedef FGArrayCtrl Parent;

		friend class FearCSDelegate;
		friend class SimGui::JoinGameDelegate;

	public:   // WASM-PORT: reconstruction lost the public: before the API block

		DECLARE_PERSISTENT(ServerListCtrl);

		static int prefColumnToSort;
		static bool prefSortAscending;
		static int prefColumnToSortSecond;
		static bool prefSortAscendingSecond;

		static int mConnectionHi;
		static int mConnectionLo;

		bool sortAscending;
		int columnToSort;

		bool sortAscendingSecond;
		int columnToSortSecond;

		// WASM-PORT reconstruction state (the reconstructed header had lost these):
		bool mbMasterTimedOut;
		char pingAddress[64];
		char mCellText[512];   // scratch buffer returned by getCellText

		ServerListCtrl();

		virtual void readDisplayTable();
		virtual void writeScriptFile();
		virtual void serverTimeout(const char* address);
		virtual bool isNeverPing(const char* address);
		virtual void setNeverPing(const char* address, bool value);
		virtual void rebuildNeverPingList();
		virtual bool isFavorite(const char* address);
		virtual void setFavorite(const char* address, bool value);

		virtual void removeCurrentServer();
		virtual void updatePingList(const char* address);
		virtual void gotPingInfo(const char* address, const char* name, UInt16 version, int ping);
		virtual void gotGameInfo(FearCSDelegate::ServerInfo* info);
		virtual FearCSDelegate::ServerInfo* getServerSelected(bool& infoAvail);
		virtual void writeDisplayTable();
		virtual bool cellSelectable(const Point2I& cell);
		virtual bool cellSelected(Point2I cell);
		virtual void updateCurFilter();
		virtual bool findBuddy(FearCSDelegate::ServerInfo* info, const char* buddy);
		virtual bool findBuddy(FearCSDelegate::ServerInfo* info);
		virtual bool filterServer(FearCSDelegate::ServerInfo* info);
		virtual void refreshList();
		virtual void rebuildList();
		virtual bool refreshVisible();
		virtual void rebuildFinished(bool bMasterTimedOut);
		virtual void rebuildCancel();
		virtual char* getCellText(GFXSurface* sfc, const Point2I& cell, const Point2I& cellOffset, const Point2I& cellExtent);

	protected:
		void onMouseDown(const SimGui::Event& event) override;
		void onKeyDown(const SimGui::Event& event) override;
		void onMouseUp(const SimGui::Event& event) override;
		Int32 getMouseCursorTag() override;
		Int32 getHelpTag(float elapsedTime) override;
		bool onAdd() override;
		void onWake() override;
		void onPreRender() override;
		void onRenderCell(GFXSurface* sfc, Point2I offset, Point2I cell, bool selected, bool mouseOver) override;

	public:
		virtual void addGameServer(int argc, const char * argv[]);
	};
}
