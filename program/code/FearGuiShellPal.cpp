//----------------------------------------------------------------------------

//----------------------------------------------------------------------------

#include "g_surfac.h"
#include "fear.strings.h"
#include "fearGlobals.h"
#include "fearGuiShellPal.h"

namespace FearGui
{

//----------------------------------------------------------------------------

void InitializePaletteTable(GFXPalette *)
{
/*
   if (gPalette) return;
   for (int i = 0; i < _NUM_HUD_COLORS; i++)
   {
      gPaletteColors[i].colorIdx = pal->GetNearestColor(gPaletteColors[i].r,
                                                        gPaletteColors[i].g,
                                                        gPaletteColors[i].b );
   }
   
   gPalette = pal;
*/
}

//----------------------------------------------------------------------------

// WASM-PORT: duplicate removed. FGHPaletteTable's persistent registration lives in
// FearHudPalTable.cpp (same FOURCC 'FGpt'); having it here too is a link-time ODR dup.
// This TU keeps only its unique InitializePaletteTable stub.

};