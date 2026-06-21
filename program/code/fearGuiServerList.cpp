// WASM-PORT: minimal faithful reconstruction of FearGui::ServerListCtrl.
//
// The 0.8.1 source shipped this file ENTIRELY commented out, and its header (also matching the
// Kronos original) derived from SimGui::Control with no data members — yet the commented body
// used FGArrayCtrl features and a `servers` list, i.e. the real class was an FGArrayCtrl-derived
// 1-column list (setSize(1,N)) whose single per-row cell renders the full server line. The two
// halves never matched, so there was nothing to "uncomment". This is a from-scratch minimal
// version that:
//   - registers under the original persist tag FOURCC('F','G','s','L') so JoinGame.gui's list
//     control node resolves to it,
//   - shows the live servers (name / players / ping / mission), one per row,
//   - supports row selection + Join (getServerSelected drives dlgJoinGame's Join button),
//   - reads straight from cg.csDelegate->mServerList (no second copy of the server table),
//   - keeps every other declared method as a no-op stub so dlgJoinGame.cpp + FearCSDelegate.cpp
//     link and call into it unchanged.
// Sorting / filtering / favorites / address-book file persistence are intentionally dropped.

#include "console.h"
#include "fearGlobals.h"
#include "fearCSDelegate.h"
#include "fearGuiServerList.h"
#include "dlgJoinGame.h"
#include "simResource.h"
#include "g_surfac.h"
#include "g_font.h"

namespace FearGui
{

IMPLEMENT_PERSISTENT_TAG(ServerListCtrl, FOURCC('F','G','s','L'));

int  ServerListCtrl::prefColumnToSort       = 0;
bool ServerListCtrl::prefSortAscending      = true;
int  ServerListCtrl::prefColumnToSortSecond = 0;
bool ServerListCtrl::prefSortAscendingSecond= true;
int  ServerListCtrl::mConnectionHi          = 0;
int  ServerListCtrl::mConnectionLo          = 0;

ServerListCtrl::ServerListCtrl()
{
   sortAscending = true;
   columnToSort = 0;
   sortAscendingSecond = true;
   columnToSortSecond = 0;
   mbMasterTimedOut = false;
   pingAddress[0] = 0;
   mCellText[0] = 0;
}

// the live decoded server table lives on the client session object
static FearCSDelegate* del() { return cg.csDelegate; }
static int serverCount() { FearCSDelegate* d = del(); return d ? d->mServerList.size() : 0; }

bool ServerListCtrl::onAdd()
{
   // FGArrayCtrl::onAdd loads the row fonts + cellSize + the column bitmap array
   // (IDPBA_SHELL_COLUMNS -> ShellCHeaderCtrl.PBA, present in Shell.vol).
   if (!Parent::onAdd())
      return false;
   cg.gameServerList = this;
   mbMasterTimedOut = false;
   pingAddress[0] = 0;
   headerDim.set(0, 0);   // single column, no header chrome -> engine skips the bma render path
   active = true;
   setSize(Point2I(1, serverCount()));
   return true;
}

void ServerListCtrl::onWake()
{
   // one column spanning the parent width; row count from the live server list
   if (parent)
      cellSize.set(parent->extent.x, cellSize.y);
   setSize(Point2I(1, serverCount()));
}

char* ServerListCtrl::getCellText(GFXSurface*, const Point2I& cell, const Point2I&, const Point2I&)
{
   FearCSDelegate* d = del();
   if (d && cell.y >= 0 && cell.y < d->mServerList.size())
   {
      FearCSDelegate::ServerInfo& s = d->mServerList[cell.y];
      sprintf(mCellText, "%-32s  %2d/%-2d  %4dms  %s",
              s.name, s.numPlayers, s.maxPlayers, s.pingTime, s.missionName);
   }
   else
      mCellText[0] = 0;
   return mCellText;
}

void ServerListCtrl::onRenderCell(GFXSurface* sfc, Point2I offset, Point2I cell, bool selected, bool mouseOver)
{
   Resource<GFXFont>& font = selected ? hFontHL : (mouseOver ? hFontMO : hFont);
   const char* text = getCellText(sfc, cell, offset, cellSize);
   if (text && text[0])
      drawInfoText(sfc, font, text, Point2I(offset.x + 2, offset.y), Point2I(cellSize.x - 4, cellSize.y), false, false);
}

bool ServerListCtrl::cellSelectable(const Point2I& cell)
{
   return cell.y >= 0 && cell.y < serverCount();
}

bool ServerListCtrl::cellSelected(Point2I cell)
{
   bool ok = Parent::cellSelected(cell);   // ArrayCtrl records selectedCell + scrolls it visible
   // re-enable the Join button for the new selection
   SimGui::JoinGameDelegate* dlgt = (SimGui::JoinGameDelegate*) manager->findObject("SimGui::JoinGameDelegate");
   if (dlgt) dlgt->verifyServer();
   return ok;
}

FearCSDelegate::ServerInfo* ServerListCtrl::getServerSelected(bool& infoAvail)
{
   FearCSDelegate* d = del();
   Point2I sc = getSelectedCell();
   if (d && sc.y >= 0 && sc.y < d->mServerList.size())
   {
      infoAvail = true;
      return &d->mServerList[sc.y];
   }
   infoAvail = false;
   return NULL;
}

// --- server-list events from FearCSDelegate: just refresh the row count + repaint ---
void ServerListCtrl::gotGameInfo(FearCSDelegate::ServerInfo*)
{
   setSize(Point2I(1, serverCount()));
   setUpdate();
}

void ServerListCtrl::gotPingInfo(const char*, const char*, UInt16, int)
{
   // the full GameInfo follows (gotGameInfo) and is what adds the server; nothing to do here.
}

void ServerListCtrl::serverTimeout(const char*)
{
   setSize(Point2I(1, serverCount()));
   setUpdate();
}

void ServerListCtrl::rebuildList()
{
   selectedCell.set(-1, -1);
   setSize(Point2I(1, 0));
   setUpdate();
}

void ServerListCtrl::rebuildFinished(bool bMasterTimedOut)
{
   mbMasterTimedOut = bMasterTimedOut;
   setSize(Point2I(1, serverCount()));
   setUpdate();
}

bool ServerListCtrl::refreshVisible()
{
   return false;   // no re-ping of the visible set in the minimal browser
}

void ServerListCtrl::rebuildCancel() {}
void ServerListCtrl::rebuildNeverPingList() {}

// --- input: route through the engine ArrayCtrl (cell hit-test + selection), skipping
//     FGArrayCtrl's column resize/sort handling (we have no column table) ---
void ServerListCtrl::onMouseDown(const SimGui::Event& event) { SimGui::ArrayCtrl::onMouseDown(event); }
// NATIVE-PORT: this was empty, which leaked the mouse lock onMouseDown takes (ScrollCtrl::onMouseDown ->
// root->mouseLock(this)). After selecting a server the list captured ALL later clicks, so Join Game /
// Back never received them. Route to the engine ArrayCtrl/ScrollCtrl up handler, which releases the lock.
void ServerListCtrl::onMouseUp(const SimGui::Event& event) { SimGui::ArrayCtrl::onMouseUp(event); }
void ServerListCtrl::onKeyDown(const SimGui::Event& event) { SimGui::ArrayCtrl::onKeyDown(event); }
void ServerListCtrl::onPreRender() {}

Int32 ServerListCtrl::getMouseCursorTag() { return Parent::getMouseCursorTag(); }
Int32 ServerListCtrl::getHelpTag(float elapsedTime) { return Parent::getHelpTag(elapsedTime); }

// --- everything below is a deliberate no-op stub (sort / filter / favorites / persistence) ---
void ServerListCtrl::readDisplayTable() {}
void ServerListCtrl::writeScriptFile() {}
void ServerListCtrl::writeDisplayTable() {}
bool ServerListCtrl::isNeverPing(const char*) { return false; }
void ServerListCtrl::setNeverPing(const char*, bool) {}
bool ServerListCtrl::isFavorite(const char*) { return false; }
void ServerListCtrl::setFavorite(const char*, bool) {}
void ServerListCtrl::removeCurrentServer() {}
void ServerListCtrl::updatePingList(const char*) {}
void ServerListCtrl::updateCurFilter() {}
bool ServerListCtrl::findBuddy(FearCSDelegate::ServerInfo*, const char*) { return false; }
bool ServerListCtrl::findBuddy(FearCSDelegate::ServerInfo*) { return false; }
bool ServerListCtrl::filterServer(FearCSDelegate::ServerInfo*) { return true; }
void ServerListCtrl::refreshList() {}
void ServerListCtrl::addGameServer(int, const char**) {}

}
