//----------------------------------------------------------------------------
// scriptGL.cpp  (NATIVE-PORT)
//
// Reimplements the team5150 "ScriptGL" script drawing interface on top of the
// engine's OpenGL surface, so the Kronos vhud / ScriptGL HUD (HP/Mana bars,
// weapon, target, chat, shop, ...) renders. ScriptGL was a native engine patch
// on the real (Borland) Tribes client and is NOT part of TribesSource, so the
// gl* drawing console commands and the ScriptGL::<gui>::onPreDraw render hook
// simply did not exist here. This file adds both.
//
// The gl* commands draw while the engine's GL context is current and its 2D
// ortho is set (during Canvas::render). Filled rects go straight through raw GL
// so they get arbitrary RGBA (the engine's 2D path is palette-indexed); text
// uses the engine's own .pft drawText_p (a first-cut approximation of
// ScriptGL's TrueType/glow fonts).
//----------------------------------------------------------------------------
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>

#include "console.h"
#include "consoleInternal.h"
#include "ast.h"   // gEvalState
#include "g_surfac.h"
#include "g_font.h"
#include "simResource.h"
#include "simBase.h"
#include "stringTable.h"

//---- ScriptGL draw state (only valid during a ScriptGL_renderHook call) -----
static GFXSurface*       sSurf    = NULL;
static SimManager*       sManager = NULL;
static int               sR = 255, sG = 255, sB = 255, sA = 255;
static Resource<GFXFont> sFont;

static void scriptGL_loadFont()
{
   if(!(bool)sFont && sManager)
      sFont = SimResource::get(sManager)->load("sf_white_7.pft");
}

//---- gl* console commands ---------------------------------------------------
static const char *c_glColor(CMDConsole*, int, int argc, const char **argv)
{
   if(argc >= 4) { sR=atoi(argv[1]); sG=atoi(argv[2]); sB=atoi(argv[3]); sA=(argc>4)?atoi(argv[4]):255; }
   return "";
}

static const char *c_glRectangle(CMDConsole*, int, int argc, const char **argv)
{
   if(argc >= 5 && sSurf)
   {
      int x=atoi(argv[1]), y=atoi(argv[2]), w=atoi(argv[3]), h=atoi(argv[4]);
      glPushAttrib(GL_ENABLE_BIT | GL_CURRENT_BIT | GL_COLOR_BUFFER_BIT);
      glDisable(GL_TEXTURE_2D);
      glDisable(GL_ALPHA_TEST);
      glEnable(GL_BLEND);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      glColor4ub((GLubyte)sR,(GLubyte)sG,(GLubyte)sB,(GLubyte)sA);
      glBegin(GL_QUADS);
         glVertex2i(x,   y);
         glVertex2i(x+w, y);
         glVertex2i(x+w, y+h);
         glVertex2i(x,   y+h);
      glEnd();
      glPopAttrib();
   }
   return "";
}

static const char *c_glSetFont(CMDConsole*, int, int, const char **)
{
   // FIRST CUT: ScriptGL uses TrueType (name, pixel height, mode, glow). Approximate with a
   // fixed engine .pft until a GDI/texture font path is added.
   scriptGL_loadFont();
   return "";
}

static const char *c_glDrawString(CMDConsole*, int, int argc, const char **argv)
{
   if(argc >= 4 && sSurf)
   {
      scriptGL_loadFont();
      if((bool)sFont)
      {
         Point2I pt(atoi(argv[1]), atoi(argv[2]));
         sSurf->drawText_p(sFont, &pt, argv[3]);
      }
   }
   return "";
}

static const char *c_glGetStringDimensions(CMDConsole*, int, int argc, const char **argv)
{
   static char buf[32];
   scriptGL_loadFont();
   int w=0, h=8;
   if((bool)sFont) { h = sFont->getHeight(); if(argc >= 2) w = sFont->getStrWidth(argv[1]); }
   sprintf(buf, "%d %d", w, h);
   return buf;
}

// State-only GL calls the scripts make (blend/enable toggles). Our raw rect draw manages its own
// state via push/pop, so these can be inert.
static const char *c_glNoop(CMDConsole*, int, int, const char **) { return ""; }

static void scriptGL_register()
{
   Console->addCommand(0, "glColor4ub",            c_glColor);   // the actual ScriptGL color call (r,g,b,a bytes)
   Console->addCommand(0, "glColor",               c_glColor);
   Console->addCommand(0, "glRectangle",           c_glRectangle);
   Console->addCommand(0, "glSetFont",             c_glSetFont);
   Console->addCommand(0, "glDrawString",          c_glDrawString);
   Console->addCommand(0, "glGetStringDimensions", c_glGetStringDimensions);
   Console->addCommand(0, "glDisable",             c_glNoop);
   Console->addCommand(0, "glEnable",              c_glNoop);
   Console->addCommand(0, "glBlendFunc",           c_glNoop);
}

//---- the render hook: called after a content control renders ----------------
// If a ScriptGL::<controlName>::onPreDraw script function exists, set up the draw state and call
// it (and onPostDraw) so the HUD overlays the just-rendered 3D/GUI. Named onPreDraw by team5150
// but it runs as a pre-flip overlay, which is what we do here.
void ScriptGL_renderHook(SimObject *ctrl, GFXSurface *srf)
{
   static bool registered = false;
   if(!registered) { scriptGL_register(); registered = true; }

   if(!ctrl || !srf) return;
   const char *name = ctrl->getName();
   if(!name || !name[0]) return;

   char fn[160];
   sprintf(fn, "ScriptGL::%s::onPreDraw", name);
   Dictionary::Entry *ent = gEvalState.frames[0]->lookup(stringTable.insert(fn));
   if(!ent || !ent->func) return;

   sSurf    = srf;
   sManager = ctrl->getManager();
   Console->evaluatef("ScriptGL::%s::onPreDraw(\"%d %d\");",  name, srf->getWidth(), srf->getHeight());
   Console->evaluatef("ScriptGL::%s::onPostDraw(\"%d %d\");", name, srf->getWidth(), srf->getHeight());
   sSurf    = NULL;
   sManager = NULL;
}
