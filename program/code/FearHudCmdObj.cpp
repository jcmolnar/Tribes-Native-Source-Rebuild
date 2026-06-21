//------------------------------------------------------------------------------
// Description 
//    
// $Workfile$
// $Revision$
// $Author  $
// $Modtime $
//
//------------------------------------------------------------------------------
#include "g_surfac.h"
#include "g_bitmap.h"
#include "g_font.h"
#include "simGame.h"
#include "simResource.h"
#include "fear.strings.h"
#include "fearHudCmdObj.h"
#include "fearGlobals.h"
#include "playerManager.h"

namespace FearGui
{

#ifdef __EMSCRIPTEN__
FGHCommandObjectivePage* g_wasmCmdObjPage = NULL;
#endif

FGHCommandObjectivePage::FGHCommandObjectivePage(void)
{
   for (int i = 0; i < MAX_NUM_OBJECTIVES; i++)
   {
      objectives[i] = NULL;
   }
   extent.set(100, 100);
}

FGHCommandObjectivePage::~FGHCommandObjectivePage(void)
{
   for (int i = 0; i < MAX_NUM_OBJECTIVES; i++)
   {
      if (objectives[i]) delete objectives[i];
   }
#ifdef __EMSCRIPTEN__
   if (g_wasmCmdObjPage == this) g_wasmCmdObjPage = NULL;
#endif
}

void FGHCommandObjectivePage::newObjective(int index, const char *text)
{
   if (objectives[index]) delete objectives[index];
   if(!text)
   {
      objectives[index] = NULL;
      return;
   }
   
   objectives[index] = new SimGui::TextFormat();
   
   SimGui::TextFormat *newObj = objectives[index];
   newObj->setFont(0, "sf_white_10b.pft");
   newObj->setFont(1, "sf_green_10b.pft");
   newObj->setFont(2, "sf_yellow_10b.pft");
   newObj->setFont(3, "sf_orange214_10.pft");
   newObj->setFont(4, "sf_red_10b.pft");
   newObj->setFont(5, "sf_green_12.pft");
   newObj->setFont(6, "sf_yellow_12.pft");
   newObj->setFont(7, "sf_orange_12.pft");
   newObj->setFont(8, "sf_green_14.pft");
   newObj->setFont(9, "sf_yellow_14.pft");
   
   newObj->formatControlString(text, extent.x);
   
   //reset the height
   int height = 0;
   for (int i = 0; i < MAX_NUM_OBJECTIVES; i++)
   {
      if (objectives[i]) height += objectives[i]->getHeight();
   }
   Point2I newExt = extent;
   newExt.y = height + 4;
   resize(position, newExt);
   setUpdate();
}

bool FGHCommandObjectivePage::onAdd(void)
{
#ifdef __EMSCRIPTEN__
   g_wasmCmdObjPage = this;
#endif
   return Parent::onAdd();
}

void FGHCommandObjectivePage::onWake(void)
{
   if (parent) extent.x = parent->extent.x;
#ifdef __EMSCRIPTEN__
   // WASM-PORT: pull through the shared refresh so onWake and the late-arriving-event path stay identical.
   refreshFromTeamList();
#else
   PlayerManager::ClientRep *me = cg.playerManager->findClient(manager->getId());
   if(me)
   {
      PlayerManager::TeamRep *team = cg.playerManager->findTeam(me->team);
      if(team)
         for(int i = 0; i < MAX_NUM_OBJECTIVES; i++)
            newObjective(i, team->objectives[i].text);
   }
#endif
}

#ifdef __EMSCRIPTEN__
void FGHCommandObjectivePage::refreshFromTeamList(void)
{
   // WASM-PORT: the original code only pulled teamList in onWake -- which fires when the page opens,
   // BEFORE the pull-based TeamObjectiveEvent burst has arrived over the wire -- so the page stayed blank
   // even though the lines later landed in teamList. Route both onWake and the event handler here so a
   // line that arrives after the page is already awake still shows. Ensure a non-zero width first: the
   // handler's old direct push was gated on extent.x>0 because formatControlString into width 0 can spin.
   if (parent && parent->extent.x > 0) extent.x = parent->extent.x;
   if (extent.x <= 0) return;   // not laid out yet; onWake re-pulls once the page is sized
   PlayerManager::ClientRep *me = cg.playerManager->findClient(manager ? manager->getId() : -1);
   if (!me) return;
   // WASM-PORT: index teamList DIRECTLY rather than via findTeam(me->team). findTeam gates on numTeams
   // (incremented only as TeamAddEvents are processed); on the live server that count lags / is cut short
   // by the event-stream desync, so findTeam(0) returns NULL even though the TeamObjectiveEvent handler
   // already stored this client's lines in teamList[me->team+1]. me->team is the valid 0-based team index;
   // teamList is indexed [team+1] (slot 0 = the unnamed/-1 team). This is the same slot the handler writes
   // and wasmGetObjectives reads, so the page now shows exactly what arrived over the wire.
   int ti = me->team + 1;
   if (ti < 0 || ti >= (int)PlayerManager::MaxTeams) return;
   PlayerManager::TeamRep *team = &cg.playerManager->teamList[ti];
   for (int i = 0; i < MAX_NUM_OBJECTIVES; i++)
      newObjective(i, team->objectives[i].text[0] ? team->objectives[i].text : NULL);
}
#endif

void FGHCommandObjectivePage::onPreRender()
{
   if (parent)
   {
      if(parent->extent.x != extent.x)
      {
         int height = 0;
         for (int i = 0; i < MAX_NUM_OBJECTIVES; i++)
         {
            if (objectives[i])
            {
               objectives[i]->formatControlString(NULL, parent->extent.x, TRUE);
               height += objectives[i]->getHeight();
            }
         }
         Point2I newExt(parent->extent.x, height + 4);
         if(newExt.x != extent.x || newExt.y != extent.y)
            resize(position, newExt);
      }
   }
}

void FGHCommandObjectivePage::onRender(GFXSurface *sfc, Point2I offset, const Box2I &updateRect)
{
   Point2I curOffset = offset;
   
   for (int i = 0; i < MAX_NUM_OBJECTIVES; i++)
   {
      if (objectives[i])
      {
         objectives[i]->onRender(sfc, curOffset, updateRect);
         curOffset.y += objectives[i]->getHeight();
      }
   }
}
      
IMPLEMENT_PERSISTENT_TAG( FGHCommandObjectivePage, FOURCC('F','G','c','o') );

};