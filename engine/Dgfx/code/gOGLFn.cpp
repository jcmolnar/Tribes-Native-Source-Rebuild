//------------------------------------------------------------------------------
// Description 
//    
// $Workfile$
// $Revision$
// $Author  $
// $Modtime $
//
//------------------------------------------------------------------------------
#include <windows.h>
#include <GL/gl.h>
#ifdef __EMSCRIPTEN__
#include <stdio.h>   // WASM-PORT diag: [FARV] printf
#endif

#include "d_funcs.h"
#include "fn_all.h"
#include "g_pal.h"
#include "g_bitmap.h"
#include "gfxMetrics.h"
#include "p_txcach.h"

#include "gOGLSfc.h"
#include "gOGLTx.h"
//#include "gOGLTCache.h"
#include "r_clip.h"

#ifdef DEBUG
#define OGL_ERROR_CHECK(line) while(checkOGLError(line));
#else
#define OGL_ERROR_CHECK(line) ;
#endif

extern UInt32 g_texDownloadThisFrame;
extern UInt32 g_lmDownloadThisFrame;
extern UInt32 g_lmDownloadBytes;
extern UInt32 g_oglFrameKey;
extern UInt32 g_oglEntriesTouched;

extern UInt32 g_oglFrameKeyAccum;
extern UInt32 g_oglFrameKeyNum;
extern float  g_oglAverageFrameKey;

bool g_prefOGLNoAddFade = false;

// WASM-PORT: when true, EmitPoly2Pass skips the lightmap multiply so unlit interiors/terrain
// (no lightmaps built on the decode client) render their base textures at full brightness.
// Set from JS via Module._wasmSetFullbright(N): 1=skip lightmap (base texture), 2=flat magenta
// (texturing off, geometry/camera isolation test). Global scope so the harness export can reach it.
// DEFAULT 0 (was 1): interiors now render with their real BAKED static lightmaps (see
// g_wasmLitInterior below and the dumpOGLLightmap WASM-PORT fix in gOGLTx.cpp). Fullbright was a
// stopgap for when the lightmap GL upload was broken (it threw GL_INVALID_OPERATION and no-opped,
// multiplying the world to black); that's fixed, so the lit path is the default. Set 1 to force
// unlit base textures (brighter, flatter) for debugging.
int g_wasmFullbright = 0;

// WASM-PORT: backface-cull winding sense. The FAKE_W_BUFFER emit submits pre-projected screen
// verts; combined with the projection's glScalef(1,-1,1) Y-flip the winding GL sees may be
// reversed vs the original gOGL's GL_CW=front assumption -> inverted culling (see-through walls,
// inside-out player models). Toggle live from JS (Module._wasmSetFrontFace(1)=CCW) to find the
// correct sense without a rebuild. 0 = GL_CW (stock), 1 = GL_CCW.
int g_wasmFrontFaceCCW = 0;

// WASM-PORT: independent backface-cull disable (decoupled from g_wasmFullbright's magenta debug),
// so we can test "cull OFF but textures ON" to separate an interior CULLING bug from a TEXTURING
// bug. Module._wasmSetCull(1) = culling disabled. 0 = stock (enabled).
int g_wasmCullOff = 0;

// WASM-PORT: test the CORRECT interior 2pass path -- the [LMPROBE] dump proved the static
// lightmaps decode BRIGHT (0xeeed), so the "lightmaps are black" premise behind the fullbright
// skip is false. When set, EmitPoly2Pass forces the base pass opaque AND does the real lightmap
// multiply (instead of fullbright's skip), so interiors render base x real-lightmap = properly
// lit. Module._wasmSetLit(1). CONFIRMED CORRECT (interiors render lit) -- now the DEFAULT (1); this
// replaced the fullbright hack for interiors once the dumpOGLLightmap GL-upload bug was fixed.
int g_wasmLitInterior = 1;

// WASM-PORT debug counters (reset each frame in Draw3dBegin): how many polys take the TWOPASS
// interior path (EmitPoly2Pass) vs each inline EmitPoly fill branch. Surfaced via window.__cstate
// to tell whether the city geometry even reaches EmitPoly2Pass (mode-5 test only runs there).
int g_dbgEmit2Pass = 0;   // EmitPoly2Pass entries
int g_dbgNoTex2Pass = 0;  // WASM-PORT diag: 2pass polys with pBmp==NULL && cb==NULL -> NO texture
                          // bound, render flat gray (the gray interior walls). Surfaced in PATHS.
int g_dbgCbTex2Pass = 0;  // WASM-PORT diag: 2pass polys that ARE callback textures (cb!=NULL)
int g_dbgEmitConst = 0;   // EmitPoly GFX_FILL_CONSTANT
int g_dbgEmitLmap  = 0;   // EmitPoly GFX_FILL_LIGHTMAP
int g_dbgEmitTex   = 0;   // EmitPoly textured (else) branch
int g_dbgBadVerts  = 0;   // WASM-PORT diag: emitted verts with invW (in_pVert->z) <= 0 -> spike to center
int g_dbgFarVerts  = 0;   // WASM-PORT diag: emitted verts with tiny invW (0 < z < 1e-4) -> near vanishing pt
extern int g_dbgDrawPoly, g_dbgDrawTri;  // WASM-PORT diag: defined in ts_PointArray.cpp (global scope)
extern int g_dbgEmitTag;                 // WASM-PORT diag: which draw path is currently emitting
                                         //   1=drawTriangle 2=drawTriangleClip 3=drawPoly(interior) 4=drawPolyClip

namespace OpenGL {

inline bool
checkOGLError(const int in_lineNo)
{
#ifdef DEBUG
   GLenum error = glGetError();
   AssertWarn(error == GL_NO_ERROR, avar("%s: real line: %d", translateOpenGLError(error), in_lineNo));
   return error != GL_NO_ERROR;
#else
   return false;
#endif   
}


// WASM-PORT: interior/terrain polys now carry up to sm_maxNumVertices=1024 verts (raised from the
// stock 100 to fit ncity.dis surfaces, commit 35d2d9a). The haze cache accumulates a poly's verts
// BEFORE checkCache() flushes (threshold 900): the CONSTANT-haze path expands one N-vert poly into
// (N-2)*3 cache verts (~3066 for N=1024) and the fan path adds up to N — so the stock 1024-entry
// caches overflowed (a stray write past m_vertexCache corrupting adjacent memory -> a later
// EmitPoly faulted reading the Surface vertex array) once terrain pushed per-frame poly counts high
// enough to keep the cache near 900. Stock (100-vert polys -> 294) never hit this. Size for the
// worst single poly + the 900 flush headroom: 900+3066 < 4096 (tris), 900+1024 < 2048 (fans).
#ifdef __EMSCRIPTEN__
#  define WASM_HAZE_TRI_CAP 4096
#  define WASM_HAZE_FAN_CAP 2048
#else
#  define WASM_HAZE_TRI_CAP 1024
#  define WASM_HAZE_FAN_CAP 1024
#endif
class HazeTriCache
{
   GLfloat m_vertexCache[WASM_HAZE_TRI_CAP * 4];
   GLfloat m_hazeCache[WASM_HAZE_TRI_CAP];

   GLfloat m_vertexFanCache[WASM_HAZE_FAN_CAP * 4];
   GLfloat m_hazeFanCache[WASM_HAZE_FAN_CAP];
   UInt32  m_fanCache[256];

   UInt32 m_currentVertex;

   UInt32 m_currentFanVertex;
   UInt32 m_numFans;

  public:
   HazeTriCache() : m_currentVertex(0), m_currentFanVertex(0), m_numFans(0) { }

   void addVertex(const float* in_v, const float in_haze);

   void addFanVertex(const float* in_v,
                     const float  in_haze);
   void emitFan();

   void flushCache(Surface*);
   void checkCache(Surface*);
};

inline void
HazeTriCache::addVertex(const float* in_v, const float in_haze)
{
   GLfloat* pVert = &m_vertexCache[m_currentVertex * 4];
   GLfloat* pHaze = &m_hazeCache[m_currentVertex];

   memcpy(pVert, in_v, sizeof(float) * 4);
   *pHaze = in_haze;

   m_currentVertex++;
}

inline void
HazeTriCache::addFanVertex(const float* in_v, const float in_haze)
{
   AssertFatal(m_currentFanVertex < 1023, "Huh?");

   GLfloat* pVert = &m_vertexFanCache[m_currentFanVertex * 4];
   GLfloat* pHaze = &m_hazeFanCache[m_currentFanVertex];

   memcpy(pVert, in_v, sizeof(float) * 4);
   *pHaze = in_haze;

   m_currentFanVertex++;
}

inline void
HazeTriCache::emitFan()
{
   m_fanCache[m_numFans] = m_currentFanVertex;
   m_numFans++;
}

void
HazeTriCache::checkCache(Surface* pSurface)
{
   if (m_currentVertex    >= 900 ||
       m_currentFanVertex >= 900)
      flushCache(pSurface);
}

void
HazeTriCache::flushCache(Surface* pSurface)
{
//   ColorF fillColor;
//   fillColor.set(pow(float(pColors[in_index].peRed)   / 255.0f, 1.0f / gamma),
//                 pow(float(pColors[in_index].peGreen) / 255.0f, 1.0f / gamma),
//                 pow(float(pColors[in_index].peBlue)  / 255.0f, 1.0f / gamma));


   Surface::TextureCache* pTxCache = pSurface->getTextureCache();
   GLfloat hazeColor[4] = {
      pow(pSurface->m_hazeColor.red,   1.0f / pSurface->getGamma()),
      pow(pSurface->m_hazeColor.green, 1.0f / pSurface->getGamma()),
      pow(pSurface->m_hazeColor.blue,  1.0f / pSurface->getGamma())
   };

   if (m_currentVertex != 0) {
      pTxCache->enableTexUnits(false);
      pTxCache->setBlendMode(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

      glBegin(GL_TRIANGLES);
      for (UInt32 i = 0; i < m_currentVertex; i++) {
         hazeColor[3] = m_hazeCache[i];
         glColor4fv(hazeColor);
         glVertex4fv(&m_vertexCache[i * 4]);
      }
      glEnd();
   }
   m_currentVertex = 0;

   if (m_numFans != 0) {
      int vertexIndex = 0;
      for (UInt32 i = 0; i < m_numFans; i++) {
         glBegin(GL_TRIANGLE_FAN);
         for (UInt32 j = vertexIndex; j < m_fanCache[i]; j++) {
            hazeColor[3] = m_hazeFanCache[j];
            glColor4fv(hazeColor);
            glVertex4fv(&m_vertexFanCache[j * 4]);
         }
         glEnd();
         vertexIndex = m_fanCache[i];
      }
   }
   m_currentFanVertex = 0;
   m_numFans          = 0;

   OGL_ERROR_CHECK(__LINE__);
}

namespace {

HazeTriCache g_hazeTriCache;

const GLuint SGIS_TEXTURE_0 = 0x835E; //0x83C6
const GLuint SGIS_TEXTURE_1 = 0x835F; //0x83C7
UInt8 g_callbackBuffer[256*256];

#undef GL_CLAMP_TO_EDGE /* WASM-PORT: modern gl.h #defines it already */
const GLenum GL_CLAMP_TO_EDGE              = 0x812F;

inline void
transformLMap(Point2F&                io_rCoord,
              const HandleCacheEntry* in_pEntry)
{
   Point2F copy;
   copy.x = io_rCoord.x * in_pEntry->m_col0.x +
            io_rCoord.y * in_pEntry->m_col0.y +
            1.0f        * in_pEntry->m_col2.x;
   copy.y = io_rCoord.x * in_pEntry->m_col1.x +
            io_rCoord.y * in_pEntry->m_col1.y +
            1.0f        * in_pEntry->m_col2.y;

   io_rCoord = copy;
}

inline void
setupGLConstantIndexedColor(Surface* io_pSurface,
                            DWORD    in_color)
{
   PALETTEENTRY* pColors = io_pSurface->getPalette()->palette[0].color;
   Surface::TextureCache* pTxCache = io_pSurface->getTextureCache();
   float gamma = pTxCache->m_gamma;

   ColorF finalColor;
   finalColor.red   = float(pColors[in_color].peRed)   / 255.0f;
   finalColor.green = float(pColors[in_color].peGreen) / 255.0f;
   finalColor.blue  = float(pColors[in_color].peBlue)  / 255.0f;
   if (io_pSurface->m_shadeSource == GFX_SHADE_CONSTANT) {
      finalColor.red   *= io_pSurface->m_constantShadeColor.red;
      finalColor.green *= io_pSurface->m_constantShadeColor.green;
      finalColor.blue  *= io_pSurface->m_constantShadeColor.blue;
   }

   finalColor.set(pow(finalColor.red,   1.0f / gamma),
                  pow(finalColor.green, 1.0f / gamma),
                  pow(finalColor.blue,  1.0f / gamma));
   glColor4f(GLfloat(finalColor.red),
             GLfloat(finalColor.green),
             GLfloat(finalColor.blue),
             GLfloat(io_pSurface->m_constantAlpha));
}

inline void
setupGLConstantColor(Surface* io_pSurface)
{
   if (io_pSurface->m_alphaSource == GFX_ALPHA_NONE) {
      ColorF finalColor = io_pSurface->m_fillColor;
      if (io_pSurface->m_shadeSource == GFX_SHADE_CONSTANT) {
         finalColor.red   *= io_pSurface->m_constantShadeColor.red;
         finalColor.green *= io_pSurface->m_constantShadeColor.green;
         finalColor.blue  *= io_pSurface->m_constantShadeColor.blue;
      }

      glColor4f(GLfloat(finalColor.red),
                GLfloat(finalColor.green),
                GLfloat(finalColor.blue),
                GLfloat(io_pSurface->m_constantAlpha));
   } else {
      glColor4f(GLfloat(io_pSurface->m_fillColor.red),
                GLfloat(io_pSurface->m_fillColor.green),
                GLfloat(io_pSurface->m_fillColor.blue),
                GLfloat(io_pSurface->m_constantAlpha));
   }
}


UInt32
getNextPow2(UInt32 size)
{
   int oneCount = 0;
   int shiftCount = -1;
   while(size) {
      if(size & 1)
         oneCount++;
      shiftCount++;
      size >>= 1;
   }
   if(oneCount > 1)
      shiftCount++;
   return (1 << shiftCount);
}

float sg_lscale;
float sg_rscale;
float sg_tscale;
float sg_bscale;

void
bitmapDraw(GFXSurface*      io_pSurface,
           const GFXBitmap* in_pBM)
{
   Surface* pSurface = static_cast<Surface*>(io_pSurface);
   Surface::TextureCache* pTxCache = pSurface->getTextureCache();

   if (pTxCache->setTexture(in_pBM->getCacheInfo(), 0) == false)
      pTxCache->cacheBitmap(in_pBM, in_pBM->getCacheInfo(), 0, true);

   pTxCache->enableTexUnits(true, false);
   pTxCache->setTransparent((in_pBM->attribute & BMA_TRANSPARENT) != 0);

   if ((in_pBM->attribute & BMA_TRANSLUCENT) != 0 ||
       pSurface->m_constantAlpha != 1.0f) {
      pTxCache->setBlendMode(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
   } else {
      pTxCache->setBlendMode(GL_ONE, GL_ZERO);
   }
   pTxCache->setTexMode(GL_MODULATE);

   glColor4f(GLfloat(1.0f), GLfloat(1.0f), GLfloat(1.0f), 
             GLfloat(pSurface->m_constantAlpha));
   glBegin(GL_TRIANGLE_FAN);
   for (int i = 0; i < pSurface->getCurrVertexIndex(); i++) {
      glTexCoord2fv((GLfloat*)&pSurface->m_pTexCoord0Array[i]);
      glVertex4fv((GLfloat*)&pSurface->m_pVertexArray[i]);
   }
   glEnd();

   pSurface->clearCurrVertexIndex();
}

bool setupBitmapPoints(GFXSurface* io_pSurface,
                       int x0, int y0,
                       int x1, int y1)
{
   sg_lscale = sg_tscale = 0.0f;
   sg_rscale = sg_bscale = 1.0f;

   if(io_pSurface->getFlags() & GFX_DMF_RCLIP) {
      RectI *cr = io_pSurface->getClipRect();
      float le = float(cr->upperL.x);
      float te = float(cr->upperL.y);
      float re = float(cr->lowerR.x) + 1.0f;
      float be = float(cr->lowerR.y) + 1.0f;

      if(x0 >= re || (x0 + x1) <= le || y0 >= be || (y0 + y1) <= te)
         return false;

      if(x0 < le)
         sg_lscale = (le - x0) / x1;
      if(y0 < te)
         sg_tscale = (te - y0) / y1;
      if(x0 + x1 > re)
         sg_rscale = (re - x0) / x1;
      if(y0 + y1 > be)
         sg_bscale = (be - y0) / y1;
   }

   Surface* pSurface = static_cast<Surface*>(io_pSurface);
   DGLVertex4F* pVerts = pSurface->getCurrentVertex();

   pVerts[0].x = float(x0) + float(sg_lscale * x1);
   pVerts[0].y = float(y0) + float(sg_tscale * y1);
   pVerts[1].x = float(x0) + float(sg_rscale * x1);
   pVerts[1].y = float(y0) + float(sg_tscale * y1);
   pVerts[2].x = float(x0) + float(sg_rscale * x1);
   pVerts[2].y = float(y0) + float(sg_bscale * y1);
   pVerts[3].x = float(x0) + float(sg_lscale * x1);
   pVerts[3].y = float(y0) + float(sg_bscale * y1);

   for(int i = 0; i < 4; i++) {
      pVerts[i].z = 0.0f;
      pVerts[i].w = 1.0f;
   }

   return true;
}

void
setupBitmapTexCoords(GFXSurface*      io_pSurface,
                     const GFXBitmap* in_pBM,
                     float x0, float y0,
                     float x1, float y1)
{
   Surface* pSurface        = static_cast<Surface*>(io_pSurface);
   DGLTexCoord4F* pTexVerts = pSurface->getCurrentTexCoord0();

   UInt32 padWidth  = getNextPow2(in_pBM->getWidth());
   UInt32 padHeight = getNextPow2(in_pBM->getHeight());

   float hScale = 1.0f / float(padWidth);
   float vScale = 1.0f / float(padHeight);

   pTexVerts[0].s = float(x0) + float(x1 - x0) * sg_lscale;
   pTexVerts[0].t = float(y0) + float(y1 - y0) * sg_tscale;
   pTexVerts[1].s = float(x0) + float(x1 - x0) * sg_rscale;
   pTexVerts[1].t = pTexVerts[0].t;
   pTexVerts[2].s = pTexVerts[1].s;
   pTexVerts[2].t = float(y0) + float(y1 - y0) * sg_bscale;
   pTexVerts[3].s = pTexVerts[0].s;
   pTexVerts[3].t = pTexVerts[2].t;
   
   for (int i = 0; i < 4; i++) {
      pTexVerts[i].s *= hScale;
      pTexVerts[i].t *= vScale;

      pTexVerts[i].r = 0.0f;
      pTexVerts[i].q = 1.0f;
   }
}

} // namespace {}


void
externCheckCache(Surface* pSurface)
{
   g_hazeTriCache.flushCache(pSurface);
}



#define FAKE_W_BUFFER

void 
AddVertexVTC(GFXSurface*          io_pSurface,
             const Point3F*       in_pVert,
             const Point2F*       in_pTex,
             const GFXColorInfoF* in_pColor,
             DWORD                /*in_softwareEdgeKey*/)
{
   // Function must handle the case that in_pTex or in_pColor are NULL.  This
   //  should never happen unless the parameters are unnecessary to the current
   //  poly type.
   // in_softwareEdgeKey can be ignored
   //
   Surface* pSurface = static_cast<Surface*>(io_pSurface);
   DGLVertex4F* pVertex = pSurface->getCurrentVertex();

#ifdef __EMSCRIPTEN__
   // WASM-PORT diag: in_pVert->z is pt_z = invW (1/w). A vert with invW <= 0 is at/behind the eye
   // plane and should have been clipped; emitted here it reconstructs to a huge/negated position and
   // spikes from screen center. Tiny positive invW = a far/vanishing-point vert (legit). Count both.
   if (in_pVert->z <= 0.0f)             ::g_dbgBadVerts++;
   else if (in_pVert->z < 1.0e-4f)      ::g_dbgFarVerts++;
   // [FARV] dump SILENCED: the 4 farV verts are simSky::renderSolid's fullscreen backdrop
   // quad corners (z=W_DIST=1/65536), confirmed benign. Counter still increments above.
#endif

   // We always enter the coord into the array...
   //
#ifdef FAKE_W_BUFFER
   if (in_pVert->z != 0.0f) {
      // First, reextract z.  in_pVert->z == nearPlane / z
      //
      float oglZ  = (2.0f * in_pVert->z) - 1.0f;
      float zCalc = (1.0f / in_pVert->z) * pSurface->m_nearClipPlane;

      pVertex->x = in_pVert->x * zCalc;
      pVertex->y = in_pVert->y * zCalc;
      pVertex->z = oglZ        * zCalc;
      pVertex->w = zCalc;
   } else {
      float zCalc = pSurface->m_farClipPlane;

      pVertex->x = in_pVert->x * zCalc;
      pVertex->y = in_pVert->y * zCalc;
      pVertex->z = 0.0f;
      pVertex->w = zCalc;
   }
#else
   if (in_pVert->z != 0.0f) {
      // First, reextract z.  in_pVert->z == nearPlane / z
      //
      float zCalc = (1.0f / in_pVert->z) * pSurface->m_nearClipPlane;
      float oglZ  = (zCalc * (pSurface->m_nearClipPlane + pSurface->m_farClipPlane) - 2.0f * (pSurface->m_nearClipPlane * pSurface->m_farClipPlane)) / (pSurface->m_farClipPlane - pSurface->m_nearClipPlane);

      pVertex->x = in_pVert->x * zCalc;
      pVertex->y = in_pVert->y * zCalc;
      pVertex->z = oglZ        * zCalc;
      pVertex->w = zCalc;
   } else {
      float zCalc = pSurface->m_farClipPlane;

      pVertex->x = in_pVert->x * zCalc;
      pVertex->y = in_pVert->y * zCalc;
      pVertex->z = 0.0f;
      pVertex->w = zCalc;
   }
#endif

   // We only need the color if the mode is GFX_SHADE_VERTEX
   //
   if (pSurface->m_shadeSource == GFX_SHADE_VERTEX) {
      AssertFatal(in_pColor != NULL, "No color info for SHADE_VERTEX poly!");
      
      DGLColor4F* pColor = pSurface->getCurrentColor();
      pColor->r = in_pColor->color.red;
      pColor->g = in_pColor->color.green;
      pColor->b = in_pColor->color.blue;

      if (pSurface->m_alphaSource == GFX_ALPHA_NONE)
         pColor->a = 1.0f;
      else if (pSurface->m_alphaSource == GFX_ALPHA_CONSTANT)
         pColor->a = pSurface->m_constantAlpha;
      else if (pSurface->m_alphaSource == GFX_ALPHA_VERTEX)
         pColor->a = in_pColor->alpha;
      else if (pSurface->m_alphaSource == GFX_ALPHA_FILL) {
         AssertFatal(0, "Inconsistent alpha state: ALPHA_FILL w/ SHADE_VERTEX");
      } else {
         AssertFatal(0, "alphaSource mode is corrupt");
      }
   }

   if (pSurface->m_fillMode != GFX_FILL_CONSTANT) {
      // We need to use the texture coordinates...
      //
      DGLTexCoord4F* pTexCoord = pSurface->getCurrentTexCoord0();
      pTexCoord->s = in_pTex->x;
      pTexCoord->t = in_pTex->y;
      pTexCoord->r = 0.0;
      pTexCoord->q = 1.0;
   }

   if (pSurface->m_hazeSource == GFX_HAZE_VERTEX) {
      AssertFatal(in_pColor != NULL, "No color info for HAZEVERTEX?");

      DGLHazeCoordF* pHazeCoord = pSurface->getCurrentHazeCoord();
      pHazeCoord->h = in_pColor->haze;
   }

   pSurface->incCurrVertexIndex();
}

void 
finishPoly2PassSGI(Surface* io_pSurface)
{
   Surface::TextureCache* pTextureCache = io_pSurface->getTextureCache();
   HandleCache* pHandleCache   = io_pSurface->getHandleCache();
   HandleCacheEntry* pEntry    = pHandleCache->getCurrentEntry();
   AssertFatal(pEntry != NULL, "No handle _here_?!");

   OGL_ERROR_CHECK(__LINE__);
   if (pEntry->pBmp == NULL) {
      // Callback texture...
      AssertFatal(pEntry->cb != NULL, "No callback, and no bitmap?");

      if (pTextureCache->setTexture(pEntry->bitmapCacheInfo, 0) == false) {
         GFXBitmap bmp;
         bmp.height = bmp.stride = bmp.width = pEntry->size;
         bmp.pBits  = g_callbackBuffer;
         bmp.pMipBits[0] = bmp.pBits;
         bmp.detailLevels = 1;
         bmp.bitDepth = 8;
         pEntry->cb(pEntry->handle, &bmp, 0);
         pTextureCache->cacheBitmap(&bmp, pEntry->bitmapCacheInfo, 0, true);
         if (pTextureCache->supportsEdgeClamp() == false) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
         } else {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
         }
      }
   } else {
      // Normal...
      if (pTextureCache->setTexture(pEntry->pBmp->getCacheInfo(), 0) == false) {
         pTextureCache->cacheBitmap(pEntry->pBmp, pEntry->pBmp->getCacheInfo(), 0,
                                    (pEntry->isTerrain == true));
      }
   }
   OGL_ERROR_CHECK(__LINE__);

   pTextureCache->enableTexUnits(true, true);
   if (pTextureCache->setLightmap(*pEntry->pLightmapCacheInfo, 1) == false)
      pTextureCache->cacheLightmap(pEntry->pLMap, *pEntry->pLightmapCacheInfo, 1);
   OGL_ERROR_CHECK(__LINE__);

   pTextureCache->setBlendMode(GL_ONE, GL_ZERO);
   pTextureCache->setTexMode(GL_REPLACE, GL_MODULATE);
   OGL_ERROR_CHECK(__LINE__);

   glColor3f(1, 1, 1);
   glBegin(GL_TRIANGLE_FAN);
   for (int i = 0; i < io_pSurface->getCurrVertexIndex(); i++) {
      float coords[2];
      
      coords[0] = io_pSurface->m_pTexCoord0Array[i].s * pEntry->coordScale.x;
      coords[1] = io_pSurface->m_pTexCoord0Array[i].t * pEntry->coordScale.y;
      coords[0] += pEntry->coordTrans.x;
      coords[1] += pEntry->coordTrans.y;
      pTextureCache->m_multiTexExtSGI.glMTexCoord2fv(SGIS_TEXTURE_0, coords);

      Point2F lmapCoord(io_pSurface->m_pTexCoord0Array[i].s,
                        io_pSurface->m_pTexCoord0Array[i].t);
      transformLMap(lmapCoord, pEntry);
      pTextureCache->m_multiTexExtSGI.glMTexCoord2fv(SGIS_TEXTURE_1, (float*)&lmapCoord);

      glVertex4fv((GLfloat*)&io_pSurface->m_pVertexArray[i]);
   }
   glEnd();
   OGL_ERROR_CHECK(__LINE__);
}

void 
EmitPoly2Pass(Surface* io_pSurface)
{
   ::g_dbgEmit2Pass++;
   Surface::TextureCache* pTextureCache = io_pSurface->getTextureCache();

   if (pTextureCache->supportsSGIMultiTexture() == true) {
      // Use mTex Ext.
      finishPoly2PassSGI(io_pSurface);
      OGL_ERROR_CHECK(__LINE__);
   } else {
      HandleCache* pHandleCache   = io_pSurface->getHandleCache();
      HandleCacheEntry* pEntry    = pHandleCache->getCurrentEntry();
      AssertFatal(pEntry != NULL, "No handle _here_?!");

      if (pEntry->pBmp == NULL
#ifdef __EMSCRIPTEN__
          // WASM-PORT: a callback interior texture with NO callback (cb==NULL; the AssertFatal
          // below compiles out under NDEBUG) has no pixel data — calling cb() is a null indirect
          // call ("null function") AND caching the uninitialized g_callbackBuffer bmp OOBs in
          // chooseDynamicName. Treat it like "no texture": skip the whole callback upload and
          // render with whatever's bound. (These are procedural/animated interior textures the
          // observe client doesn't need for a first render.)
          && pEntry->cb != NULL
#endif
          ) {
         // Callback texture...
         AssertFatal(pEntry->cb != NULL, "No callback, and no bitmap?");

         if (pTextureCache->setTexture(pEntry->bitmapCacheInfo, 0) == false) {
            GFXBitmap bmp;
            bmp.height = bmp.stride = bmp.width = pEntry->size;
            bmp.pBits  = g_callbackBuffer;
            bmp.pMipBits[0] = bmp.pBits;
            bmp.detailLevels = 1;
            bmp.bitDepth = 8;
            pEntry->cb(pEntry->handle, &bmp, 0);
            pTextureCache->cacheBitmap(&bmp, pEntry->bitmapCacheInfo, 0, true);
         }
      } else if (pEntry->pBmp != NULL) {
         // Normal...
         if (pTextureCache->setTexture(pEntry->pBmp->getCacheInfo(), 0) == false) {
            pTextureCache->cacheBitmap(pEntry->pBmp, pEntry->pBmp->getCacheInfo(), 0,
                                       pEntry->isTerrain == true);
         }
      }
#ifdef __EMSCRIPTEN__
      // WASM-PORT diag: classify the texture source for this 2pass poly. The "gray walls" are
      // surfaces that bind NO texture (pBmp==NULL && cb==NULL) and draw with stale state.
      if (pEntry->pBmp == NULL && pEntry->cb == NULL) ::g_dbgNoTex2Pass++;
      else if (pEntry->pBmp == NULL)                  ::g_dbgCbTex2Pass++;

      // WASM-PORT debug g_wasmFullbright==6: PATH COLORIZER. Flat-color each interior surface by which
      // texture path it takes so a screenshot reveals what the WALLS are: BLUE = noTex (no bitmap AND
      // no callback -> nothing bound -> renders white default), GREEN = callback/cbTex (uploads via
      // dumpOGLTextureNormalDyn), RED = base bitmap (pBmp, dumpOGLTextureNormal). Texturing off.
      if (::g_wasmFullbright == 6) {
         pTextureCache->enableTexUnits(false);
         pTextureCache->setBlendMode(GL_ONE, GL_ZERO);
         if (pEntry->pBmp == NULL && pEntry->cb == NULL) glColor4f(0.2f, 0.4f, 1.0f, 1.0f); // blue
         else if (pEntry->pBmp == NULL)                  glColor4f(0.1f, 1.0f, 0.2f, 1.0f); // green
         else                                            glColor4f(1.0f, 0.2f, 0.2f, 1.0f); // red
         glBegin(GL_TRIANGLE_FAN);
         for (int k = 0; k < io_pSurface->getCurrVertexIndex(); k++)
            glVertex4fv((GLfloat*)&io_pSurface->m_pVertexArray[k]);
         glEnd();
         return;
      }
#endif
      OGL_ERROR_CHECK(__LINE__);

      int i;
#ifdef __EMSCRIPTEN__
      // WASM-PORT debug: g_wasmFullbright==2 draws the base pass as a FLAT bright color with
      // texturing OFF — isolates "geometry/camera correct?" from "texture upload broken?". If the
      // city shows as solid magenta with this, the render path + camera are fine and the black is
      // purely the palettized-texture upload (or the lightmap multiply, skipped below).
      if (::g_wasmFullbright == 5) {
         // Decisive test: emit a HARDCODED screen-space triangle (w=1, z=0) near screen center,
         // ignoring the interior's own verts. If THIS shows magenta, the GL emit + projection +
         // state at 3D-draw time are fine and the interior VERTEX DATA is off-screen/degenerate.
         // If even this is invisible, the projection/modelview/state at 3D-draw time is broken.
         pTextureCache->enableTexUnits(false);
         pTextureCache->setBlendMode(GL_ONE, GL_ZERO);
         glDisable(GL_CULL_FACE);
         glColor4f(1.0f, 0.0f, 1.0f, 1.0f);
         glBegin(GL_TRIANGLE_FAN);
         glVertex4f(220.0f, 150.0f, 0.0f, 1.0f);
         glVertex4f(420.0f, 150.0f, 0.0f, 1.0f);
         glVertex4f(320.0f, 300.0f, 0.0f, 1.0f);
         glEnd();
         glEnable(GL_CULL_FACE);
         return;
      }
      if (::g_wasmFullbright >= 2 && ::g_wasmFullbright <= 4) {
         pTextureCache->enableTexUnits(false);
         pTextureCache->setBlendMode(GL_ONE, GL_ZERO);
         // mode 4: also disable backface culling — Draw3dBegin sets GL_CULL_FACE + glFrontFace(CW);
         // if the interior winding is opposite GL's expectation EVERY face is culled (black).
         if (::g_wasmFullbright == 4) glDisable(GL_CULL_FACE);
         glBegin(GL_TRIANGLE_FAN);
         glColor4f(1.0f, 0.0f, 1.0f, 1.0f);
         for (i = 0; i < io_pSurface->getCurrVertexIndex(); i++) {
            // mode 3/4: force z=0 (z_ndc safe) to test whether the 3D polys are DEPTH-CLIPPED
            // (oglZ outside [-1,1]) rather than texture/lighting. m_pVertexArray[i] = (x,y,z,w).
            DGLVertex4F v = io_pSurface->m_pVertexArray[i];
            if (::g_wasmFullbright >= 3) v.z = 0.0f;
            glVertex4fv((GLfloat*)&v);
         }
         glEnd();
         if (::g_wasmFullbright == 4) glEnable(GL_CULL_FACE);
         return;
      }
#endif
      pTextureCache->enableTexUnits(true);
      pTextureCache->setBlendMode(GL_ONE, GL_ZERO);
      pTextureCache->setTexMode(GL_REPLACE);
#ifdef __EMSCRIPTEN__
      // WASM-PORT: force opaque under fullbright — peFlags-as-alpha (see gOGLTx setPalette) + a
      // stale setTransparent(true) makes GL_ALPHA_TEST discard interior texels (dither/holes).
      if (::g_wasmFullbright || ::g_wasmLitInterior) pTextureCache->setTransparent(false);
#endif
      glBegin(GL_TRIANGLE_FAN);
      for (i = 0; i < io_pSurface->getCurrVertexIndex(); i++) {
         float coords[2];
         coords[0] = io_pSurface->m_pTexCoord0Array[i].s * pEntry->coordScale.x;
         coords[1] = io_pSurface->m_pTexCoord0Array[i].t * pEntry->coordScale.y;
         coords[0] += pEntry->coordTrans.x;
         coords[1] += pEntry->coordTrans.y;
         glTexCoord2fv(coords);
         glVertex4fv((GLfloat*)&io_pSurface->m_pVertexArray[i]);
      }
      glEnd();

#ifdef __EMSCRIPTEN__
      // WASM-PORT: fullbright. Interiors/terrain render base-texture (pass 1, above) then MULTIPLY
      // by the lightmap (pass 2, below, blend ZERO/SRC_COLOR). On the observe/decode client the
      // lightmaps are never built (the MissionLighting subsystem is a non-compiling stub), so the
      // lightmap is black and pass 2 multiplies the whole world to black — geometry renders (high
      // poly counts) but is invisible. When g_wasmFullbright is set, skip the lightmap multiply so
      // the base textures show at full brightness. Toggle from JS via Module._wasmSetFullbright(1).
      if (::g_wasmFullbright && !::g_wasmLitInterior)
         return;
#endif
      if (pTextureCache->setLightmap(*pEntry->pLightmapCacheInfo, 0) == false)
         pTextureCache->cacheLightmap(pEntry->pLMap, *pEntry->pLightmapCacheInfo, 0);

      pTextureCache->setBlendMode(GL_ZERO, GL_SRC_COLOR);

      glBegin(GL_TRIANGLE_FAN);
      for (i = 0; i < io_pSurface->getCurrVertexIndex(); i++) {
         float coords[2];

         Point2F lmapCoord(io_pSurface->m_pTexCoord0Array[i].s,
                           io_pSurface->m_pTexCoord0Array[i].t);
         transformLMap(lmapCoord, pEntry);
         glTexCoord2f(lmapCoord.x, lmapCoord.y);
         glVertex4fv((GLfloat*)&io_pSurface->m_pVertexArray[i]);
      }
      glEnd();
      OGL_ERROR_CHECK(__LINE__);
   }
}

void 
EmitPoly(GFXSurface* io_pSurface)
{
   // Send the current set of vertices to the card...
   //
   Surface* pSurface = static_cast<Surface*>(io_pSurface);
   Surface::TextureCache* pTxCache = pSurface->getTextureCache();

   // WASM-PORT: the GL emit path historically never updated GFXMetrics (only the software
   // GFXSortEmitPoly did), so emitted/rendered poly counts read 0 under GL. Count here so the
   // render probe (window.__cstate.emitPolys/renderPolys) reflects actual GL geometry — every
   // emitted poly is drawn (no software occlusion pass).
   GFXMetrics.incEmittedPolys();
   GFXMetrics.incRenderedPolys();

#ifdef __EMSCRIPTEN__
   // WASM-PORT debug (g_wasmFullbright==5): emit a HARDCODED screen-space magenta triangle near
   // screen center for EVERY emitted poly, ignoring this poly's verts/fill. Unlike the EmitPoly2Pass
   // mode-5 test, this runs in the INLINE EmitPoly path (const/lmap/tex fills) so it fires even when
   // the observer camera sees only DTS shapes (2pass=0). If THIS shows, the GL 3D-draw path +
   // projection + state are sound and the world-black is overpaint/empty-view; if black, a 3D-pass
   // overpaint is still clobbering the viewport. Drawn opaque, texturing off, cull off, z=0.
   if (::g_wasmFullbright == 5) {
      pTxCache->enableTexUnits(false);
      pTxCache->setBlendMode(GL_ONE, GL_ZERO);
      glDisable(GL_CULL_FACE);
      glColor4f(1.0f, 0.0f, 1.0f, 1.0f);
      glBegin(GL_TRIANGLE_FAN);
      glVertex4f(220.0f, 150.0f, 0.0f, 1.0f);
      glVertex4f(420.0f, 150.0f, 0.0f, 1.0f);
      glVertex4f(320.0f, 300.0f, 0.0f, 1.0f);
      glEnd();
      glEnable(GL_CULL_FACE);
      return;
   }
#endif

   if (pSurface->m_fillMode == GFX_FILL_TWOPASS) {
      EmitPoly2Pass(pSurface);
   } else {
      if (pSurface->m_fillMode == GFX_FILL_CONSTANT) {
         ::g_dbgEmitConst++;
         pTxCache->enableTexUnits(false);

         setupGLConstantColor(pSurface);
         glBegin(GL_TRIANGLE_FAN);
         for (int i = 0; i < pSurface->getCurrVertexIndex(); i++) {
            if (pSurface->m_colorEnabled == true) {
               glColor4f(pSurface->m_pColorArray[i].r * pSurface->m_fillColor.red,
                         pSurface->m_pColorArray[i].g * pSurface->m_fillColor.green,
                         pSurface->m_pColorArray[i].b * pSurface->m_fillColor.blue,
                         pSurface->m_pColorArray[i].a);
            }

            glVertex4fv((GLfloat*)&pSurface->m_pVertexArray[i]);
         }
         glEnd();
      } else if (pSurface->m_fillMode == GFX_FILL_LIGHTMAP) {
         ::g_dbgEmitLmap++;
         HandleCache* pHandleCache = pSurface->getHandleCache();
         HandleCacheEntry* pEntry  = pHandleCache->getCurrentEntry();

         // Texture: lightmap
         pTxCache->enableTexUnits(true, false);
         pTxCache->setTexMode(GL_REPLACE);
         pTxCache->setBlendMode(GL_ZERO, GL_SRC_COLOR);

         glBegin(GL_TRIANGLE_FAN);   // WASM-PORT: GL_POLYGON (mode 9) aborts in emscripten's glemu; TRIANGLE_FAN is identical for these convex surface polys
         for (int i = 0; i < pSurface->getCurrVertexIndex(); i++) {
            Point2F lmapCoord(pSurface->m_pTexCoord0Array[i].s,
                              pSurface->m_pTexCoord0Array[i].t);
            transformLMap(lmapCoord, pEntry);
            glTexCoord2f(lmapCoord.x, lmapCoord.y);
            glVertex4fv((GLfloat*)&pSurface->m_pVertexArray[i]);
         }
         glEnd();
      } else if (pSurface->m_fillMode == GFX_FILL_TEXTUREP1) {
         HandleCache* pHandleCache = pSurface->getHandleCache();
         HandleCacheEntry* pEntry  = pHandleCache->getCurrentEntry();

         // Texture: lightmap
         pTxCache->enableTexUnits(true, false);
         pTxCache->setTexMode(GL_REPLACE);
         pTxCache->setBlendMode(GL_ONE, GL_ZERO);

         glBegin(GL_TRIANGLE_FAN);   // WASM-PORT: GL_POLYGON (mode 9) aborts in emscripten's glemu; TRIANGLE_FAN is identical for these convex surface polys
         for (int i = 0; i < pSurface->getCurrVertexIndex(); i++) {
            float coords[2];
            coords[0] = pSurface->m_pTexCoord0Array[i].s * pEntry->coordScale.x;
            coords[1] = pSurface->m_pTexCoord0Array[i].t * pEntry->coordScale.y;
            coords[0] += pEntry->coordTrans.x;
            coords[1] += pEntry->coordTrans.y;
            glTexCoord2fv(coords);
            glVertex4fv((GLfloat*)&pSurface->m_pVertexArray[i]);
         }
         glEnd();
      } else {
         // Texture
         ::g_dbgEmitTex++;
         pTxCache->enableTexUnits(true, false);
         pTxCache->setTexMode(GL_MODULATE);

#ifdef __EMSCRIPTEN__
         // WASM-PORT: the textured path MODULATEs the texture by the per-vertex color
         // (m_pColorArray), which is the RUNTIME LIGHTING result. Our observe/decode client has
         // no lighting (MissionLighting is a stub, the SimLightSet is empty), so those vertex
         // colors are ~black and texture*black renders the geometry black. This path is used by
         // DTS SHAPES (player models, items, etc.) which — unlike interiors — have NO baked
         // lightmaps to fall back on, so we must ALWAYS force white modulation here (not just under
         // fullbright): otherwise player skins render black/invisible. (Interiors are lit via the
         // separate EmitPoly2Pass baked-lightmap path; this is the inline shape path.) fullbright==2
         // drops texturing for a flat-magenta vert check.
         {
            if (::g_wasmFullbright == 2) {
               pTxCache->enableTexUnits(false);
               pTxCache->setBlendMode(GL_ONE, GL_ZERO);
               glColor4f(1.0f, 0.0f, 1.0f, 1.0f);
            } else {
               // Force FULLY OPAQUE: the palette->RGBA expansion stuffs peFlags into the alpha
               // channel (gOGLTx.cpp setPalette), so on a surface left in setTransparent(true)
               // state the GL_ALPHA_TEST (GREATER 0.65) discards every low-alpha texel — the
               // green "dither/holes" over the world. Disable alpha test + opaque blend + alpha 1
               // so the texture shows solid at full brightness.
               pTxCache->setTransparent(false);
               pTxCache->setBlendMode(GL_ONE, GL_ZERO);
               glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
            }
            glBegin(GL_TRIANGLE_FAN);
            for (int i = 0; i < pSurface->getCurrVertexIndex(); i++) {
               glTexCoord2fv((GLfloat*)&pSurface->m_pTexCoord0Array[i]);
               glVertex4fv((GLfloat*)&pSurface->m_pVertexArray[i]);
            }
            glEnd();
            // WASM-PORT FIX: this early return short-circuits past the clearCurrVertexIndex() at the
            // end of EmitPoly, so every textured DTS-shape triangle left its 3 verts in the surface
            // buffer; the next drawTriangle appended 3 more and re-drew the whole accumulating
            // GL_TRIANGLE_FAN (nv climbing 3,6,9,... fanning from vert 0) -- THE "fan"/stretched-vertex
            // artifact on player/bot models. Stock falls through to the reset; this branch must do it
            // explicitly before returning. (Proven via [EMITSLIVER] probe: TEX tag=1 nv growing by 3.)
            pSurface->clearCurrVertexIndex();
            return;
         }
#endif
         glBegin(GL_TRIANGLE_FAN);   // WASM-PORT: GL_POLYGON (mode 9) aborts in emscripten's glemu; TRIANGLE_FAN is identical for these convex surface polys
         setupGLConstantColor(pSurface);
         for (int i = 0; i < pSurface->getCurrVertexIndex(); i++) {
            if (pSurface->m_colorEnabled == true)
               glColor4fv((GLfloat*)&pSurface->m_pColorArray[i]);

            glTexCoord2fv((GLfloat*)&pSurface->m_pTexCoord0Array[i]);
            glVertex4fv((GLfloat*)&pSurface->m_pVertexArray[i]);
         }
         glEnd();
      }
   }
   
   if (pSurface->m_hazeSource == GFX_HAZE_VERTEX) {
      if (pSurface->getCurrVertexIndex() != 3) {
         for (int i = 0; i < pSurface->getCurrVertexIndex(); i++) {
            g_hazeTriCache.addFanVertex((const float*)&pSurface->m_pVertexArray[i],
                                        pSurface->m_pHazeStoreArray[i].h);
         }
         g_hazeTriCache.emitFan();
         g_hazeTriCache.checkCache(pSurface);
      } else {
         for (int i = 0; i < pSurface->getCurrVertexIndex(); i++) {
            g_hazeTriCache.addVertex((const float*)&pSurface->m_pVertexArray[i],
                                     pSurface->m_pHazeStoreArray[i].h);
         }
         g_hazeTriCache.checkCache(pSurface);
      }
   } else if (pSurface->m_hazeSource == GFX_HAZE_CONSTANT) {
      if (pSurface->getCurrVertexIndex() > 3) {

         for (int i = 0; i < pSurface->getCurrVertexIndex() - 2; i++) {
            g_hazeTriCache.addVertex((const float*)&pSurface->m_pVertexArray[0],
                                     pSurface->m_constantHaze);
            g_hazeTriCache.addVertex((const float*)&pSurface->m_pVertexArray[i+1],
                                     pSurface->m_constantHaze);
            g_hazeTriCache.addVertex((const float*)&pSurface->m_pVertexArray[i+2],
                                     pSurface->m_constantHaze);
         }

         g_hazeTriCache.checkCache(pSurface);
      } else {
         for (int i = 0; i < pSurface->getCurrVertexIndex(); i++) {
            g_hazeTriCache.addVertex((const float*)&pSurface->m_pVertexArray[i],
                                     pSurface->m_constantHaze);
         }
         g_hazeTriCache.checkCache(pSurface);
      }
   }

   pSurface->clearCurrVertexIndex();
   OGL_ERROR_CHECK(__LINE__);
}


void 
SetTransparency(GFXSurface* io_pSurface,
                Bool        in_transFlag)
{
   Surface* pSurface = static_cast<Surface*>(io_pSurface);
   Surface::TextureCache* pTextureCache = pSurface->getTextureCache();
   pTextureCache->setTransparent(in_transFlag);
}


void 
SetFillMode(GFXSurface* io_pSurface,
            GFXFillMode in_fm)
{
   // Possible fill modes:
   //  GFX_FILL_CONSTANT: Color fill
   //  GFX_FILL_TEXTURE:  Texture fill
   //  GFX_FILL_TWOPASS:  Texture with lightmap
   //
   Surface* pSurface = static_cast<Surface*>(io_pSurface);

   pSurface->m_fillMode = in_fm;
   pSurface->m_fillColor.set(1, 1, 1);
}


void 
SetTextureMap(GFXSurface*      io_pSurface,
              const GFXBitmap* in_pTextureMap)
{
   Surface* pSurface = static_cast<Surface*>(io_pSurface);
   Surface::TextureCache* pTextureCache = pSurface->getTextureCache();

   if (pTextureCache->setTexture(in_pTextureMap->getCacheInfo(), 0) == false)
      pTextureCache->cacheBitmap(in_pTextureMap, in_pTextureMap->getCacheInfo(), 0, true);

   OGL_ERROR_CHECK(__LINE__);
}


void
precacheTextureHandle(Surface*          pSurface,
                      HandleCacheEntry* pEntry)
{
   Surface::TextureCache* pTextureCache = pSurface->getTextureCache();

   if (pEntry->pBmp == NULL) {
      // Callback texture...
#ifdef __EMSCRIPTEN__
      // WASM-PORT: a partially-loaded interior surface can reach here with neither a bitmap nor
      // a callback (the AssertFatal below compiles out under NDEBUG, so pEntry->cb() would be a
      // null indirect call -> "null function" trap during interior render). Skip such a surface.
      if (pEntry->cb == NULL)
         return;
#endif
      AssertFatal(pEntry->cb != NULL, "No callback, and no bitmap?");

      if (pTextureCache->isCurrent(pEntry->bitmapCacheInfo) == false) {
         GFXBitmap bmp;
         bmp.height = bmp.stride = bmp.width = pEntry->size;
         bmp.pBits  = g_callbackBuffer;
         bmp.pMipBits[0] = bmp.pBits;
         bmp.detailLevels = 1;
         bmp.bitDepth = 8;
         pEntry->cb(pEntry->handle, &bmp, 0);
         pTextureCache->cacheBitmap(&bmp, pEntry->bitmapCacheInfo, 0, true);
      } else {
         pTextureCache->touchEntryIfNecessary(pEntry->bitmapCacheInfo);
      }
   } else {
      // Normal...
      if (pTextureCache->isCurrent(pEntry->pBmp->getCacheInfo()) == false) {
         pTextureCache->cacheBitmap(pEntry->pBmp, pEntry->pBmp->getCacheInfo(), 0,
                                    pEntry->isTerrain == true);
      } else {
         pTextureCache->touchEntryIfNecessary(pEntry->pBmp->getCacheInfo());
      }
   }

   if (pTextureCache->supportsSGIMultiTexture() == true) {
      if (pTextureCache->isCurrentLM(*pEntry->pLightmapCacheInfo) == false) {
         pTextureCache->cacheLightmap(pEntry->pLMap, *pEntry->pLightmapCacheInfo, 1);
      } else {
         pTextureCache->touchEntryIfNecessary(*pEntry->pLightmapCacheInfo);
      }
   } else {
      if (pTextureCache->isCurrentLM(*pEntry->pLightmapCacheInfo) == false) {
         pTextureCache->cacheLightmap(pEntry->pLMap, *pEntry->pLightmapCacheInfo, 0);
      } else {
         pTextureCache->touchEntryIfNecessary(*pEntry->pLightmapCacheInfo);
      }
   }
}

void 
RegisterTexture(GFXSurface*      io_pSurface,
                GFXTextureHandle in_handle,
                int              in_sizeX,
                int              in_sizeY,
                int              in_offsetX,
                int              in_offsetY,
                int              in_lightScale,
                GFXLightMap*     io_pLightMap,
                const GFXBitmap* in_pTexture,
                int              /*in_mipLevel*/)
{
   AssertFatal((in_pTexture->getWidth()  <= 256 && in_pTexture->getWidth() > 0 &&
                in_pTexture->getHeight() <= 256 && in_pTexture->getHeight() > 0),
               "Invalid texture size");

   Surface* pSurface = static_cast<Surface*>(io_pSurface);

   AssertFatal(pSurface->getHandleCache() != NULL, "Error - surface not locked");

   Surface::TextureCache* pTxCache = pSurface->getTextureCache();
   HandleCache* pHandleCache = pSurface->getHandleCache();
   HandleCacheEntry* ent     = pHandleCache->getFreeEntry(pTxCache);
   
   ent->handle = in_handle;
   ent->flags  = 0;
   ent->pBmp   = in_pTexture;
   ent->pLMap  = io_pLightMap;
   ent->size   = 0;
   ent->cb     = 0;
   ent->isTerrain = false;

   ent->coordScale.set(float(in_sizeX) / float(in_pTexture->getWidth()),
                       float(in_sizeY) / float(in_pTexture->getHeight()));
   ent->coordTrans.set(float(in_offsetX) / float(in_pTexture->getWidth()),
                       float(in_offsetY) / float(in_pTexture->getHeight()));

   float scx = float(in_sizeX) / float(1 << in_lightScale);
   float scy = float(in_sizeY) / float(1 << in_lightScale);

   float dim = max(getNextPow2(max(io_pLightMap->size.x, io_pLightMap->size.y)), 8UL);

   float baseoffX = 0.5f / dim;
   float baseoffY = 0.5f / dim;

   Point2F lmapScale;
   Point2F lmapTrans;
   lmapScale.set(scx / dim, scy / dim);
   lmapTrans.set(baseoffX + (lmapScale.x * io_pLightMap->offset.x) / float(in_sizeX),
                 baseoffY + (lmapScale.y * io_pLightMap->offset.y) / float(in_sizeY));

   ent->m_col0.set(lmapScale.x, 0);
   ent->m_col1.set(0,           lmapScale.y);
   ent->m_col2.set(lmapTrans.x, lmapTrans.y);

   pHandleCache->HashInsert(ent);
   precacheTextureHandle(pSurface, ent);
}

void 
RegisterTextureTer(GFXSurface*           io_pSurface,
                   GFXTextureHandle      in_handle,
                   int                   /*in_sizeX*/,
                   int                   /*in_sizeY*/,
                   GFXLightMap*          io_pLightMap,
                   const GFXBitmap*      in_pTexture,
                   int                   /*in_mipLevel*/,
                   GFXBitmap::CacheInfo* io_pCacheInfo,
                   const RectI&          in_rSubSection,
                   const int             in_flags)
{
   AssertFatal(in_rSubSection.isValidRect(), "Error, bad subsection");
   AssertFatal(dynamic_cast<Surface*>(io_pSurface) != NULL, "Error, not an opengl surface!");
   AssertFatal((in_pTexture->getWidth()  <= 256 && in_pTexture->getWidth()  > 0 &&
                in_pTexture->getHeight() <= 256 && in_pTexture->getHeight() > 0),
               "Invalid texture size");

   Surface* pSurface = static_cast<Surface*>(io_pSurface);

   AssertFatal(pSurface->getHandleCache() != NULL, "Error - surface not locked");

   Surface::TextureCache* pTxCache = pSurface->getTextureCache();
   HandleCache* pHandleCache = pSurface->getHandleCache();
   HandleCacheEntry* ent     = pHandleCache->getFreeEntry(pTxCache);
   
   ent->handle    = in_handle;
   ent->flags     = 0;
   ent->pBmp      = in_pTexture;
   ent->pLMap     = io_pLightMap;
   ent->size      = 0;
   ent->cb        = 0;
   ent->isTerrain = true;

   ent->coordScale.set(1.0f, 1.0f);
   ent->coordTrans.set(0.0f, 0.0f);

   Int32 realSizeX = io_pLightMap->size.x ? io_pLightMap->size.x : 256;
   Int32 realSizeY = io_pLightMap->size.y ? io_pLightMap->size.y : 256;
   if (realSizeX == 256) {
      AssertFatal(io_pCacheInfo->bitmapSequenceNum == 0xfffffffd,
                  "WTF!");
   }

   AssertFatal(realSizeX == realSizeY, "Invalid terrain lightmap, must be square");
   AssertFatal((getNextPow2(realSizeX) == UInt32(realSizeX) &&
                getNextPow2(realSizeY) == UInt32(realSizeY)),
               "Invalid terrain lightmap.  Must be pow2.");

   float realDim = float(max(realSizeX, 8L));

   float m0[3][3];
   float temp[3][3];

   Point2F lmapScale, lmapTrans;
   lmapTrans.x = (float(in_rSubSection.upperL.x) + 0.5f) / realDim;
   lmapTrans.y = (float(in_rSubSection.upperL.y) + 0.5f) / realDim;

   lmapScale.x = float(in_rSubSection.lowerR.x - in_rSubSection.upperL.x) / realDim;
   lmapScale.y = float(in_rSubSection.lowerR.y - in_rSubSection.upperL.y) / realDim;

   enum TerrainRotation {
      Plain      = 0,
      Rotate     = 1,
      FlipX      = 2,
      FlipY      = 4,
      RotateMask = 7
   };

   switch (in_flags & RotateMask) {
     // 0
     case Plain:
      ent->m_col0.set(lmapScale.x, 0);
      ent->m_col1.set(0,           -lmapScale.y);
      ent->m_col2.set(lmapTrans.x, lmapTrans.y + lmapScale.y);
      break;

     // 0
     case FlipX:
      ent->m_col0.set(-lmapScale.x,              0);
      ent->m_col1.set(0,                         -lmapScale.y);
      ent->m_col2.set(lmapTrans.x + lmapScale.x, lmapTrans.y + lmapScale.y);
      break;

     // 0
     case FlipY:
      ent->m_col0.set(lmapScale.x, 0);
      ent->m_col1.set(0,           lmapScale.y);
      ent->m_col2.set(lmapTrans.x, lmapTrans.y);
      break;

     // 0
     case FlipY | FlipX:
      ent->m_col0.set(-lmapScale.x,              0);
      ent->m_col1.set(0,                         lmapScale.y);
      ent->m_col2.set(lmapTrans.x + lmapScale.x, lmapTrans.y);
      break;

     // 0
     case Rotate:
      ent->m_col0.set(0,                         -lmapScale.y);
      ent->m_col1.set(-lmapScale.x,              0);
      ent->m_col2.set(lmapTrans.x + lmapScale.x, lmapTrans.y + lmapScale.y);
      break;

     // 0
     case Rotate | FlipX:
      ent->m_col0.set(0,                         -lmapScale.y);
      ent->m_col1.set(lmapScale.x,               0);
      ent->m_col2.set(lmapTrans.x + lmapScale.x, lmapTrans.y);
      break;

     // 0
     case Rotate | FlipY:
      ent->m_col0.set(0,            lmapScale.y);
      ent->m_col1.set(-lmapScale.x, 0);
      ent->m_col2.set(lmapTrans.x,  lmapTrans.y + lmapScale.y);
      break;

     // 0
     case Rotate | FlipX | FlipY:
      ent->m_col0.set(0,           lmapScale.y);
      ent->m_col1.set(lmapScale.x, 0);
      ent->m_col2.set(lmapTrans.x, lmapTrans.y);
      break;
   }

   delete ent->pLightmapCacheInfo;
   ent->pLightmapCacheInfo = io_pCacheInfo;
   AssertFatal(io_pCacheInfo->bitmapSequenceNum == 0xfffffffd, "Bad terrain cacheinfo");

   pHandleCache->HashInsert(ent);
   precacheTextureHandle(pSurface, ent);
}


void 
RegisterTextureCB(GFXSurface*      io_pSurface,
                  GFXTextureHandle in_handle,
                  GFXCacheCallback in_cb,
                  int              in_csizeX,
                  int              /*in_csizeY*/,
                  int              /*in_lightScale*/,
                  GFXLightMap*     io_pLightMap)
{
   Surface* pSurface = static_cast<Surface*>(io_pSurface);

   AssertFatal(pSurface->getHandleCache() != NULL, "Error - surface not locked");
   
   Surface::TextureCache* pTxCache = pSurface->getTextureCache();
   HandleCache* pHandleCache = pSurface->getHandleCache();
   HandleCacheEntry* ent     = pHandleCache->getFreeEntry(pTxCache);
   
   ent->handle = in_handle;
   ent->flags  = 0;
   ent->cb     = in_cb;
   ent->size   = in_csizeX;
   ent->pBmp   = NULL;
   ent->pLMap  = io_pLightMap;
   ent->isTerrain = false;

   ent->coordScale.set(1.0f, 1.0f);
   ent->coordTrans.set(0.0f, 0.0f);

   float spow   = float(max(getNextPow2(io_pLightMap->size.x), 8UL));
   float scale  = (io_pLightMap->size.x - 1) / spow;
   float offset = 0.5f / spow; // half a texel offset;

   Point2F lmapScale, lmapTrans;
   lmapScale.set(scale,  scale);
   lmapTrans.set(offset, offset);

   ent->m_col0.set(lmapScale.x, 0);
   ent->m_col1.set(0,           lmapScale.y);
   ent->m_col2.set(lmapTrans.x, lmapTrans.y);

   pHandleCache->HashInsert(ent);
   precacheTextureHandle(pSurface, ent);
}


void 
RegisterTextureCBTer(GFXSurface*           io_pSurface,
                     GFXTextureHandle      in_handle,
                     GFXCacheCallback      in_cb,
                     int                   in_csizeX,
                     GFXLightMap*          io_pLightMap,
                     GFXBitmap::CacheInfo* io_pCacheInfo,
                     const RectI&          in_rSubSection)
{
   AssertFatal(in_rSubSection.isValidRect(), "Error, bad subsection");
   AssertFatal(dynamic_cast<Surface*>(io_pSurface) != NULL, "Error, not an opengl surface!");
   Surface* pSurface = static_cast<Surface*>(io_pSurface);

   AssertFatal(pSurface->getHandleCache() != NULL, "Error - surface not locked");
   
   Surface::TextureCache* pTxCache = pSurface->getTextureCache();
   HandleCache* pHandleCache = pSurface->getHandleCache();
   HandleCacheEntry* ent     = pHandleCache->getFreeEntry(pTxCache);
   
   ent->handle    = in_handle;
   ent->flags     = 0;
   ent->cb        = in_cb;
   ent->size      = in_csizeX;
   ent->pBmp      = NULL;
   ent->pLMap     = io_pLightMap;
   ent->isTerrain = true;

   ent->coordScale.set(1.0f, 1.0f);
   ent->coordTrans.set(0.0f, 0.0f);

   Int32 realSizeX = io_pLightMap->size.x ? io_pLightMap->size.x : 256;
   Int32 realSizeY = io_pLightMap->size.y ? io_pLightMap->size.y : 256;
   if (realSizeX == 256) {
      AssertFatal(io_pCacheInfo->bitmapSequenceNum == 0xfffffffd,
                  "WTF!");
   }

   AssertFatal(realSizeX == realSizeY, "Invalid terrain lightmap, must be square");
   AssertFatal((getNextPow2(realSizeX) == UInt32(realSizeX) &&
                getNextPow2(realSizeY) == UInt32(realSizeY)),
               "Invalid terrain lightmap.  Must be pow2.");

   float realDim = float(max(realSizeX, 8L));

   float m0[3][3];
   float temp[3][3];

   Point2F lmapScale, lmapTrans;
   lmapTrans.x = (float(in_rSubSection.upperL.x) + 0.5f) / realDim;
   lmapTrans.y = (float(in_rSubSection.upperL.y) + 0.5f) / realDim;

   lmapScale.x = float(in_rSubSection.lowerR.x - in_rSubSection.upperL.x) / realDim;
   lmapScale.y = float(in_rSubSection.lowerR.y - in_rSubSection.upperL.y) / realDim;

   ent->m_col0.set(lmapScale.x, 0);
   ent->m_col1.set(0,           -lmapScale.y);
   ent->m_col2.set(lmapTrans.x, lmapTrans.y + lmapScale.y);

   delete ent->pLightmapCacheInfo;
   ent->pLightmapCacheInfo = io_pCacheInfo;
   AssertFatal(io_pCacheInfo->bitmapSequenceNum == 0xfffffffd, "Bad terrain cacheinfo");

   pHandleCache->HashInsert(ent);
   precacheTextureHandle(pSurface, ent);
}

void
RegisterTextureTerCover(GFXSurface*           io_pSurface,
                        GFXTextureHandle      in_handle,
                        GFXLightMap*          io_pLightMap,
                        const GFXBitmap*      in_pTexture,
                        GFXBitmap::CacheInfo* io_pCacheInfo)
{
   AssertFatal(dynamic_cast<Surface*>(io_pSurface) != NULL, "Error, not an opengl surface!");
   Surface* pSurface = static_cast<Surface*>(io_pSurface);

   Surface::TextureCache* pTxCache = pSurface->getTextureCache();
   HandleCache* pHandleCache       = pSurface->getHandleCache();
   HandleCacheEntry* ent           = pHandleCache->getFreeEntry(pTxCache);

   ent->handle    = in_handle;
   ent->flags     = 0;
   ent->cb        = NULL;
   ent->pBmp      = in_pTexture;
   ent->pLMap     = io_pLightMap;
   ent->isTerrain = true;

   ent->coordScale.set(1.0f, 1.0f);
   ent->coordTrans.set(0.0f, 0.0f);

   Int32 realSizeX = io_pLightMap->size.x ? io_pLightMap->size.x : 256;
   Int32 realSizeY = io_pLightMap->size.y ? io_pLightMap->size.y : 256;
   if (realSizeX == 256) {
      AssertFatal(io_pCacheInfo->bitmapSequenceNum == 0xfffffffd,
                  "WTF!");
   }

   AssertFatal(realSizeX == realSizeY, "Invalid terrain lightmap, must be square");
   AssertFatal((getNextPow2(realSizeX) == UInt32(realSizeX) &&
                getNextPow2(realSizeY) == UInt32(realSizeY)),
               "Invalid terrain lightmap.  Must be pow2.");

   float realDim = float(max(realSizeX, 8L));

   Point2F lmapScale, lmapTrans;
   lmapTrans.x = 0.5f / realDim;
   lmapTrans.y = 0.5f / realDim;

   lmapScale.x = float(realDim - 1) / realDim;
   lmapScale.y = float(realDim - 1) / realDim;

   ent->m_col0.set(lmapScale.x, 0);
   ent->m_col1.set(0,           -lmapScale.y);
   ent->m_col2.set(lmapTrans.x, lmapTrans.y + lmapScale.y);

   delete ent->pLightmapCacheInfo;
   ent->pLightmapCacheInfo = io_pCacheInfo;
   AssertFatal(io_pCacheInfo->bitmapSequenceNum == 0xfffffffd, "Bad terrain cacheinfo");

   pHandleCache->HashInsert(ent);
   precacheTextureHandle(pSurface, ent);
}


Bool 
SetTextureHandle(GFXSurface*      io_pSurface,
                 GFXTextureHandle in_handle)
{
   Surface* pSurface = static_cast<Surface*>(io_pSurface);
   AssertFatal(pSurface->getHandleCache() != NULL, "error, no handle cache");
   
   HandleCache* pHandleCache = pSurface->getHandleCache();

   return pHandleCache->setTextureHandle(in_handle);
}


void 
HandleSetLightMap(GFXSurface*  io_pSurface,
                  int          /*in_lightScale*/,
                  GFXLightMap* io_pLightMap)
{
   Surface*               pSurface      = static_cast<Surface*>(io_pSurface);
   HandleCache*           pHandleCache  = pSurface->getHandleCache();
   Surface::TextureCache* pTextureCache = pSurface->getTextureCache();

   AssertFatal(pHandleCache->getCurrentEntry() != NULL, "No current entry?");
   HandleCacheEntry* pEntry = pHandleCache->getCurrentEntry();

   if (pEntry->pLMap != NULL) {
      pTextureCache->flushLightMap(*pEntry->pLightmapCacheInfo);
      gfxLightMapCache.release(pEntry->pLMap);
   }
   
   pEntry->pLMap  = io_pLightMap;
   pEntry->flags &= ~HandleCacheEntry::LightMapValid;
   precacheTextureHandle(pSurface, pEntry);
}

void 
HandleSetLightMapTer(GFXSurface*           io_pSurface,
                     GFXLightMap*          io_pLightMap,
                     GFXBitmap::CacheInfo* io_pCacheInfo)
{
   Surface*               pSurface      = static_cast<Surface*>(io_pSurface);
   HandleCache*           pHandleCache  = pSurface->getHandleCache();
   Surface::TextureCache* pTextureCache = pSurface->getTextureCache();

   AssertFatal(pHandleCache->getCurrentEntry() != NULL, "No current entry?");
   HandleCacheEntry* pEntry = pHandleCache->getCurrentEntry();

   if (pEntry->pLightmapCacheInfo->bitmapSequenceNum == 0xfffffffd) {
      // Terrain lightmap, don't flush unless the lightmap pointers
      //  are different..
      //
      if (pEntry->pLMap != io_pLightMap) {
         // Flush the sucker!
         pTextureCache->flushLightMap(*pEntry->pLightmapCacheInfo);
         gfxLightMapCache.release(pEntry->pLMap);
      }
   } else {
      // Other lightmap, flush away!
      pTextureCache->flushLightMap(*pEntry->pLightmapCacheInfo);
      gfxLightMapCache.release(pEntry->pLMap);
   }

   // Clear out the cacheinfo pointer, if we have a replacement...
   if (io_pCacheInfo != NULL) {
      if (pEntry->pLightmapCacheInfo->bitmapSequenceNum != 0xfffffffd) {
         delete pEntry->pLightmapCacheInfo;
      }
      pEntry->pLightmapCacheInfo = io_pCacheInfo;
   }
   
   pEntry->pLMap  = io_pLightMap;
   pEntry->flags &= ~HandleCacheEntry::LightMapValid;
   precacheTextureHandle(pSurface, pEntry);
}


void 
HandleSetTextureSize(GFXSurface* io_pSurface,
                     int         in_newSize)
{
   // Set the texturesize of the current texture handle
   // Only used for callback textures

   Surface* pSurface         = static_cast<Surface*>(io_pSurface);
   HandleCache* pHandleCache = pSurface->getHandleCache();

   AssertFatal(pHandleCache->getCurrentEntry() != NULL, "No current entry?");
   HandleCacheEntry* pEntry = pHandleCache->getCurrentEntry();

   if (pEntry->cb) {
      pEntry->size = in_newSize;
   }
}


void 
FlushTextureCache(GFXSurface* io_pSurface)
{
   // Remove all textures from the card...
   //
   Surface* pSurface                    = static_cast<Surface*>(io_pSurface);
   Surface::TextureCache* pTextureCache = pSurface->getTextureCache();
   HandleCache* pHandleCache            = pSurface->getHandleCache();

   if (pHandleCache  != NULL) pHandleCache->flush();
   if (pTextureCache != NULL) pTextureCache->flushCache();

   OGL_ERROR_CHECK(__LINE__);
}


void 
FlushTexture(GFXSurface*      in_pSurface,
             const GFXBitmap* in_pTexture,
             const bool       /*in_reload*/)
{
   if (in_pTexture == NULL)
      return;

//   AssertFatal(in_reload == true, "flush not supported");

   // Remove specific texture from the cache.
   //
   Surface* pSurface = static_cast<Surface*>(in_pSurface);
   pSurface->getTextureCache()->refreshBitmap(in_pTexture, in_pTexture->getCacheInfo());

   OGL_ERROR_CHECK(__LINE__);
}


GFXLightMap* 
HandleGetLightMap(GFXSurface* io_pSurface)
{
   Surface* pSurface         = static_cast<Surface*>(io_pSurface);
   HandleCache* pHandleCache = pSurface->getHandleCache();
   HandleCacheEntry* pEntry  = pHandleCache->getCurrentEntry();

   if (pEntry != NULL) {
      return pEntry->pLMap;
   } else {
      return NULL;
   }
}

void
SetClipPlanes(GFXSurface* io_pSurface,
              const float in_nearDist,
              const float in_farDist)
{
   Surface* pSurface = static_cast<Surface*>(io_pSurface);

   pSurface->m_nearClipPlane = in_nearDist;
   pSurface->m_farClipPlane  = in_farDist;
}

//------------------------------------------------------------------------------
#define _BEGIN_IMPLEMENTED_
//------------------------------------------------------------------------------

void 
Draw3dBegin(GFXSurface* io_pSurface)
{
   // Prepare for drawing 3d
   GFXMetrics.reset();
   ::g_dbgEmit2Pass = ::g_dbgEmitConst = ::g_dbgEmitLmap = ::g_dbgEmitTex = 0;
   ::g_dbgNoTex2Pass = ::g_dbgCbTex2Pass = 0;
   ::g_dbgBadVerts = ::g_dbgFarVerts = 0;
   ::g_dbgDrawPoly = ::g_dbgDrawTri = 0;

   Surface* pSurface = static_cast<Surface*>(io_pSurface);
   Surface::TextureCache* pTxCache = pSurface->getTextureCache();

   pTxCache->enableZBuffering(true, true, GL_LEQUAL);
   pSurface->m_in3dmode = true;

   g_texDownloadThisFrame = 0;
   g_lmDownloadThisFrame  = 0;
   g_lmDownloadBytes      = 0;
   g_oglEntriesTouched    = 0;
   g_oglFrameKey++;

   if (::g_wasmCullOff) {                                 // WASM-PORT: live cull disable (textures stay on)
      glDisable(GL_CULL_FACE);
   } else {
      glEnable(GL_CULL_FACE);
      glFrontFace(::g_wasmFrontFaceCCW ? GL_CCW : GL_CW); // WASM-PORT: live-toggleable winding sense
   }
}


void
Draw3dEnd(GFXSurface* io_pSurface)
{
   Surface* pSurface = static_cast<Surface*>(io_pSurface);

   g_hazeTriCache.flushCache(pSurface);

   if (g_oglFrameKeyNum != 0) {
      g_oglAverageFrameKey = float(g_oglFrameKeyAccum) / float(g_oglFrameKeyNum);
   } else {
      g_oglAverageFrameKey = 0.0f;
   }

   Surface::TextureCache* pTxCache = pSurface->getTextureCache();

   pTxCache->enableZBuffering(true, false, GL_ALWAYS);
   pSurface->m_in3dmode = false;
}


void 
ClearScreen(GFXSurface* io_pSurface,
            DWORD       in_color)
{
   // Mostly obsolete, usually a background fill poly is drawn instead...
   //
   AssertFatal(io_pSurface->getPalette(), "No palette attached");
   // NATIVE-PORT: the AssertFatal above is a no-op in release; with no palette attached yet getPalette()
   // returns NULL and palette[0] derefs near-NULL -> crash (hit during GUI palette load on OpenGL).
   if(!io_pSurface->getPalette()) { glClearColor(0.0f,0.0f,0.0f,0.0f); glClear(GL_COLOR_BUFFER_BIT); return; }
   PALETTEENTRY* pColors = io_pSurface->getPalette()->palette[0].color;
   glClearColor(GLfloat(pColors[in_color].peRed)   / 255.0f,
                GLfloat(pColors[in_color].peGreen) / 255.0f,
                GLfloat(pColors[in_color].peBlue)  / 255.0f,
                GLfloat(0.0f));
   glClear(GL_COLOR_BUFFER_BIT);

   OGL_ERROR_CHECK(__LINE__);
}


void 
ClearZBuffer(GFXSurface* /*io_pSurface*/)
{
   // Mostly obsolete, usually a background fill poly is drawn instead...
   //
   glClearDepth(0.0f);
   glClear(GL_DEPTH_BUFFER_BIT);

   OGL_ERROR_CHECK(__LINE__);
}


void 
SetFillColorCF(GFXSurface*   io_pSurface,
               const ColorF* in_pColor)
{
   static_cast<Surface*>(io_pSurface)->m_fillColor = *in_pColor;
}

void 
SetHazeColorCF(GFXSurface*   io_pSurface,
               const ColorF* in_pColor)
{
   static_cast<Surface*>(io_pSurface)->m_hazeColor = *in_pColor;

   GLfloat colorParams[4] = {
      in_pColor->red,
      in_pColor->green,
      in_pColor->blue,
      1.0f
   };
   glFogfv(GL_FOG_COLOR, colorParams);

   OGL_ERROR_CHECK(__LINE__);
}

void 
SetConstantShadeCF(GFXSurface*   io_pSurface,
                   const ColorF* in_pShadeColor)
{
   static_cast<Surface*>(io_pSurface)->m_constantShadeColor = *in_pShadeColor;
}


void 
SetConstantHaze(GFXSurface* io_pSurface,
                float       in_haze)
{
   static_cast<Surface*>(io_pSurface)->m_constantHaze = in_haze;

//   // One part in 1000 should be accurate enough...
//   //
//   GLfloat start = (in_haze) * -1000.0f;
//   GLfloat end   = (1.0f - in_haze) * 1000.0f;
//
//   glFogf(GL_FOG_START, start);
//   glFogf(GL_FOG_END,   end);
}


void 
SetConstantAlpha(GFXSurface* io_pSurface,
                 float       in_alpha)
{
   static_cast<Surface*>(io_pSurface)->m_constantAlpha = in_alpha;
}


void 
DrawPoint(GFXSurface*    io_pSurface,
          const Point2I* in_pt,
          float          in_w,
          DWORD          in_color)
{
   Surface* pSurface = static_cast<Surface*>(io_pSurface);
   Surface::TextureCache* pTxCache = pSurface->getTextureCache();
   AssertFatal(pSurface->getPalette(), "No palette attached");

   // Calc z value...
   float oglZ  = (2.0f * in_w) - 1.0f;

   pTxCache->enableTexUnits(false);
   pTxCache->setBlendMode(GL_ONE, GL_ZERO);
   pTxCache->setTransparent(false);
   setupGLConstantIndexedColor(pSurface, in_color);

   glBegin(GL_POINTS);
      glVertex3f(GLfloat(in_pt->x) + 0.5,
                 GLfloat(in_pt->y) + 0.5,
                 oglZ);
   glEnd();

   OGL_ERROR_CHECK(__LINE__);
}


void 
DrawLine2d(GFXSurface*    io_pSurface,
           const Point2I* in_st,
           const Point2I* in_en,
           DWORD          in_color)
{
   Surface* pSurface = static_cast<Surface*>(io_pSurface);
   Surface::TextureCache* pTxCache = pSurface->getTextureCache();
   AssertFatal(pSurface->getPalette(), "No palette attached");

   Point2I start = *in_st, end = *in_en;

   if(!rectClip(&start, &end, io_pSurface->getClipRect() ))
         return;

   pTxCache->enableTexUnits(false);
   pTxCache->setBlendMode(GL_ONE, GL_ZERO);
   pTxCache->setTransparent(false);
   setupGLConstantIndexedColor(pSurface, in_color);
   glBegin(GL_LINES);
      glVertex2f(GLfloat(start.x) + 0.5f,
                 GLfloat(start.y) + 0.5f);
      glVertex2f(GLfloat(end.x)   + 0.5f,
                 GLfloat(end.y)   + 0.5f);
   glEnd();
   glBegin(GL_POINTS);
      glVertex2f(GLfloat(start.x) + 0.45,
                 GLfloat(start.y) + 0.45);
      glVertex2f(GLfloat(end.x) + 0.45,
                 GLfloat(end.y) + 0.45);
   glEnd();

   OGL_ERROR_CHECK(__LINE__);
}


void 
DrawRect2d(GFXSurface*  io_pSurface,
           const RectI* in_pRect,
           DWORD        in_color)
{
   DrawLine2d(io_pSurface, &in_pRect->upperL, &Point2I(in_pRect->lowerR.x, in_pRect->upperL.y), in_color);
   DrawLine2d(io_pSurface, &Point2I(in_pRect->lowerR.x, in_pRect->upperL.y), &in_pRect->lowerR, in_color);
   DrawLine2d(io_pSurface, &in_pRect->lowerR, &Point2I(in_pRect->upperL.x, in_pRect->lowerR.y), in_color);
   DrawLine2d(io_pSurface, &Point2I(in_pRect->upperL.x, in_pRect->lowerR.y), &in_pRect->upperL, in_color);
}


void 
DrawRect_f(GFXSurface*  io_pSurface,
           const RectI* in_pRect,
           float        in_w,
           DWORD        in_color)
{
   RectI clippedRect = *in_pRect;
   if(!rectClip(&clippedRect, io_pSurface->getClipRect()))
      return;

   Surface* pSurface = static_cast<Surface*>(io_pSurface);
   Surface::TextureCache* pTxCache = pSurface->getTextureCache();
   AssertFatal(pSurface->getPalette(), "No palette attached");

   DGLVertex4F* pVerts = pSurface->getCurrentVertex();

   pVerts[0].x = GLfloat(clippedRect.upperL.x + 0);
   pVerts[0].y = GLfloat(clippedRect.upperL.y + 0);
   pVerts[1].x = GLfloat(clippedRect.lowerR.x + 1);
   pVerts[1].y = GLfloat(clippedRect.upperL.y + 0);
   pVerts[2].x = GLfloat(clippedRect.lowerR.x + 1);
   pVerts[2].y = GLfloat(clippedRect.lowerR.y + 1);
   pVerts[3].x = GLfloat(clippedRect.upperL.x + 0);
   pVerts[3].y = GLfloat(clippedRect.lowerR.y + 1);

   float oglZ  = (2.0f * in_w) - 1.0f;

   int i;
   for(i = 0; i < 4; i++)
      pVerts[i].z = oglZ;

   for (i = 0; i < 4; i++)
      pSurface->incCurrVertexIndex();

   pTxCache->enableTexUnits(false);

   if (pSurface->m_fillMode      != GFX_ALPHA_FILL &&
       pSurface->m_constantAlpha == 1.0f)
      pTxCache->setBlendMode(GL_ONE, GL_ZERO);
   else
      pTxCache->setBlendMode(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

   pTxCache->setTransparent(false);
   // WASM-PORT: set the constant color BEFORE glBegin. Emscripten's glemu uses
   // an interleaved immediate-mode buffer: a glColor issued INSIDE begin/end
   // writes one interleaved color slot and bumps the vertex counter rather than
   // latching a "current color" for all following vertices (desktop GL
   // semantics). One color + N vertices therefore desyncs the buffer and
   // nothing renders. Called outside begin/end it sets glemu's clientColor,
   // applied as a constant attribute to every vertex — the behavior intended
   // here. (DrawLine2d already orders it this way, which is why lines worked.)
   setupGLConstantIndexedColor(pSurface, in_color);
   glBegin(GL_TRIANGLE_FAN);
   for (i = 0; i < pSurface->getCurrVertexIndex(); i++) {
      glVertex3fv((GLfloat*)&pSurface->m_pVertexArray[i]);
   }
   glEnd();
   OGL_ERROR_CHECK(__LINE__);

   pSurface->clearCurrVertexIndex();
}


void 
SetZTest(GFXSurface* io_pSurface,
         int         in_enable)
{
   Surface* pSurface = static_cast<Surface*>(io_pSurface);
   Surface::TextureCache* pTxCache = pSurface->getTextureCache();

   pSurface->m_zTest = (GFXZBufferMode)in_enable;

#ifdef FAKE_W_BUFFER
   const GLenum test = GL_GEQUAL;
#else
   const GLenum test = GL_LEQUAL;
#endif

   switch (in_enable) {
     case GFX_NO_ZTEST:
      pTxCache->enableZBuffering(false, false, test);
      break;

     case GFX_ZTEST:
      pTxCache->enableZBuffering(true, false, test);
      break;

     case GFX_ZTEST_AND_WRITE:
      pTxCache->enableZBuffering(true, true, test);
      break;

     case GFX_ZWRITE:
     case GFX_ZALWAYSBEHIND:
      pTxCache->enableZBuffering(true, true, GL_ALWAYS);
      break;

     default:
      AssertFatal(0, "unknown zBuffer mode in OGL driver");
   }

   OGL_ERROR_CHECK(__LINE__);
}


// Fill modes, shade, haze and alpha sources are listed in inc\d_defs.h
//
void 
SetAlphaSource(GFXSurface*    io_pSurface,
               GFXAlphaSource in_as)
{
   Surface* pSurface = static_cast<Surface*>(io_pSurface);
   Surface::TextureCache* pTxCache = pSurface->getTextureCache();
   pSurface->m_alphaSource = in_as;
   pSurface->m_constantAlpha = 1.0f;

   // GFX_ALPHA_NONE:     Draw fully opaque
   //          _CONSTANT: Draw at level set by SetConstantAlpha
   //          _VERTEX:   Unused currently
   //          _TEXTURE:  Draw translucent texture
   //          _FILL:     Draw a solid color index from the Palette transColor
   //                      (g_pal.h : 112) table.  (Index is send through
   //                      setFillColorI (alpha value is in peFlags)
   //
   switch (in_as) {
     case GFX_ALPHA_NONE:
      pTxCache->setBlendMode(GL_ONE, GL_ZERO);
      break;

     case GFX_ALPHA_CONSTANT:
     case GFX_ALPHA_VERTEX:
      pTxCache->setBlendMode(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      pTxCache->setTexMode(GL_MODULATE);
      break;

     case GFX_ALPHA_FILL:
      pTxCache->setBlendMode(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      break;

     case GFX_ALPHA_TEXTURE:
      pTxCache->setBlendMode(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      pTxCache->setTexMode(GL_MODULATE);
      break;
     
     case GFX_ALPHA_ADD:
      if (g_prefOGLNoAddFade == true) {
         pTxCache->setBlendMode(GL_ONE, GL_ONE);
      } else {
         pTxCache->setBlendMode(GL_SRC_ALPHA, GL_ONE);
      }
      pTxCache->setTexMode(GL_MODULATE);
      break;

     case GFX_ALPHA_SUB:
      pTxCache->setBlendMode(GL_ZERO, GL_ONE_MINUS_SRC_COLOR);
      pTxCache->setTexMode(GL_MODULATE);
      break;

     default:
      AssertFatal(0, "Unknown alphaSource");
   }

   OGL_ERROR_CHECK(__LINE__);
}


void 
SetShadeSource(GFXSurface*    io_pSurface,
               GFXShadeSource in_ss)
{
   // GFX_SHADE_NONE:     Draw at full brightness
   //          _CONSTANT: Draw at level set by SetConstantShadeCF
   //          _VERTEX:   Gouraud shaded by values sent to AddVertexVTC
   //
   //
   Surface* pSurface = static_cast<Surface*>(io_pSurface);
   Surface::TextureCache* pTxCache = pSurface->getTextureCache();

   pSurface->m_shadeSource = in_ss;
   pSurface->m_constantShadeColor.set(1, 1, 1);

   switch (in_ss) {
     case GFX_SHADE_NONE:
      pSurface->m_colorEnabled = false;
      pTxCache->setTexMode(GL_REPLACE);
      break;

     case GFX_SHADE_CONSTANT:
      pSurface->m_colorEnabled = false;
      pTxCache->setTexMode(GL_MODULATE);
      break;

     case GFX_SHADE_VERTEX:
      pSurface->m_colorEnabled = true;
      pTxCache->setTexMode(GL_MODULATE);
      break;

     default:
      AssertFatal(0, "unknown shadeSource");
      break;
   }

   OGL_ERROR_CHECK(__LINE__);
}


void 
SetHazeSource(GFXSurface*   io_pSurface,
              GFXHazeSource in_hs)
{
   // GFX_HAZE_NONE:     Draw with no haze
   //         _CONSTANT: Draw at level set by SetConstantHaze
   //         _VERTEX:   Gouraud shaded by values sent to AddVertexVTC
   //
   Surface* pSurface = static_cast<Surface*>(io_pSurface);
   Surface::TextureCache* pTxCache = pSurface->getTextureCache();
   pSurface->m_hazeSource = in_hs;

   pTxCache->enableFog(false);

   OGL_ERROR_CHECK(__LINE__);
}


void 
SetTextureWrap(GFXSurface* io_pSurface,
               Bool        in_wrapEnable)
{
   // Probably not necessary in hardware.  No texture coordinates that would
   //  cause edge clamping are passed.
   //
   Surface* pSurface = static_cast<Surface*>(io_pSurface);
   
   if (pSurface->getTextureCache()->supportsEdgeClamp() == false) {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, in_wrapEnable == false ? GL_CLAMP : GL_REPEAT);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, in_wrapEnable == false ? GL_CLAMP : GL_REPEAT);
   } else {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, in_wrapEnable == false ? GL_CLAMP_TO_EDGE : GL_REPEAT);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, in_wrapEnable == false ? GL_CLAMP_TO_EDGE : GL_REPEAT);
   }

   OGL_ERROR_CHECK(__LINE__);
}


void 
DrawBitmap2d_f(GFXSurface*      io_pSurface,
               const GFXBitmap* in_pBM,
               const Point2I*   in_at,
               GFXFlipFlag      in_flip)
{
   if(!setupBitmapPoints(io_pSurface,
                         in_at->x, in_at->y,
                         in_pBM->getWidth(), in_pBM->getHeight())) {
      return;
   }
   
   if (in_flip != GFX_FLIP_NONE) {
      float coords[4];
      coords[0] = 0.0f;
      coords[1] = 0.0f;
      coords[2] = float(in_pBM->getWidth());
      coords[3] = float(in_pBM->getHeight());
      
      float temp;
      if ((in_flip & GFX_FLIP_X) != 0) {
         temp = coords[2];
         coords[2] = coords[0];
         coords[0] = temp;
      }
      if ((in_flip & GFX_FLIP_Y) != 0) {
         temp = coords[1];
         coords[1] = coords[3];
         coords[3] = temp;
      }
      setupBitmapTexCoords(io_pSurface, in_pBM, coords[0], coords[1], coords[2], coords[3]);
   } else {
      setupBitmapTexCoords(io_pSurface, in_pBM, 0.0f, 0.0f, float(in_pBM->getWidth()), float(in_pBM->getHeight()));
   }
   Surface* pSurface      = static_cast<Surface*>(io_pSurface);
   for (int i = 0; i < 4; i++)
      pSurface->incCurrVertexIndex();
   
   bitmapDraw(io_pSurface, in_pBM);
   OGL_ERROR_CHECK(__LINE__);
}


void 
DrawBitmap2d_rf(GFXSurface*      io_pSurface,
                const GFXBitmap* in_pBM,
                const RectI*     in_subRegion,
                const Point2I*   in_at,
                GFXFlipFlag      in_flip)
{
   if(!setupBitmapPoints(io_pSurface,
                         in_at->x, in_at->y,
                         in_subRegion->len_x() + 1, in_subRegion->len_y() + 1)) {
      return;
   }
   
   if (in_flip != GFX_FLIP_NONE) {
      float coords[4];
      coords[0] = float(in_subRegion->upperL.x);
      coords[1] = float(in_subRegion->upperL.y);
      coords[2] = float(in_subRegion->lowerR.x + 1);
      coords[3] = float(in_subRegion->lowerR.y + 1);
      
      float temp;
      if ((in_flip & GFX_FLIP_X) != 0) {
         temp = coords[2];
         coords[2] = coords[0];
         coords[0] = temp;
      }
      if ((in_flip & GFX_FLIP_Y) != 0) {
         temp = coords[1];
         coords[1] = coords[3];
         coords[3] = temp;
      }
      setupBitmapTexCoords(io_pSurface, in_pBM, coords[0], coords[1], coords[2], coords[3]);
   } else {
      setupBitmapTexCoords(io_pSurface, in_pBM, 
                           float(in_subRegion->upperL.x), float(in_subRegion->upperL.y),
                           float(in_subRegion->lowerR.x) + 1, float(in_subRegion->lowerR.y + 1));
   }
   Surface* pSurface      = static_cast<Surface*>(io_pSurface);
   for (int i = 0; i < 4; i++)
      pSurface->incCurrVertexIndex();

   bitmapDraw(io_pSurface, in_pBM);
   OGL_ERROR_CHECK(__LINE__);
}


void 
DrawBitmap2d_sf(GFXSurface*      io_pSurface,
                const GFXBitmap* in_pBM,
                const Point2I*   in_at,
                const Point2I*   in_stretch,
                GFXFlipFlag      in_flip)
{
   if(!setupBitmapPoints(io_pSurface,
                         in_at->x, in_at->y,
                         in_stretch->x, in_stretch->y)) {
      return;
   }
   
   if (in_flip != GFX_FLIP_NONE) {
      float coords[4];
      coords[0] = 0.0f;
      coords[1] = 0.0f;
      coords[2] = float(in_pBM->getWidth());
      coords[3] = float(in_pBM->getHeight());
      
      float temp;
      if ((in_flip & GFX_FLIP_X) != 0) {
         temp = coords[2];
         coords[2] = coords[0];
         coords[0] = temp;
      }
      if ((in_flip & GFX_FLIP_Y) != 0) {
         temp = coords[1];
         coords[1] = coords[3];
         coords[3] = temp;
      }
      setupBitmapTexCoords(io_pSurface, in_pBM, coords[0], coords[1], coords[2], coords[3]);
   } else {
      setupBitmapTexCoords(io_pSurface, in_pBM, 0.0f, 0.0f, float(in_pBM->getWidth()), float(in_pBM->getHeight()));
   }
   Surface* pSurface      = static_cast<Surface*>(io_pSurface);
   for (int i = 0; i < 4; i++)
      pSurface->incCurrVertexIndex();

   bitmapDraw(io_pSurface, in_pBM);
   OGL_ERROR_CHECK(__LINE__);
}


void
SetZMode(GFXSurface* /*io_pSurface*/,
         bool        /*wBuffer*/)
{
   //
}


//------------------------------------------------------------------------------
// All functions below this point are probably unnecessary for hardware...
//
//--------------------------------------
void 
HandleSetMipLevel(GFXSurface*  /*io_pSurface*/,
                  int          /*in_mipLevel*/)
{
   // Not necessary for hardware in which mips are downloaded and selected
   //  automatically
}

void 
AddVertexV(GFXSurface*    io_pSurface,
           const Point3F* in_pVert,
           DWORD          in_sofwareEdgeKey)
{
   // Obsolete: pass to full function...
   AddVertexVTC(io_pSurface, in_pVert, NULL, NULL, in_sofwareEdgeKey);
}

void 
AddVertexVT(GFXSurface*    io_pSurface,
            const Point3F* in_pVert,
            const Point2F* in_pTex,
            DWORD          in_sofwareEdgeKey)
{
   // Obsolete: pass to full function...
   AddVertexVTC(io_pSurface, in_pVert, in_pTex, NULL, in_sofwareEdgeKey);
}

void 
AddVertexVC(GFXSurface*          io_pSurface,
            const Point3F*       in_pVert,
            const GFXColorInfoF* in_pColor,
            DWORD                in_sofwareEdgeKey)
{
   // Obsolete: pass to full function...
   AddVertexVTC(io_pSurface, in_pVert, NULL, in_pColor, in_sofwareEdgeKey);
}

//------------------------------------------------------------------------------
// void SetFillColorI(GFXSurface* io_pSurface, Int32 in_index)
// void SetHazeColorI(GFXSurface* io_pSurface, Int32 in_index)
// void SetConstantShadeF(GFXSurface* io_pSurface, float in_shade);
//
//  These functions are actually mostly useless in hardware, default is to just
// convert them to their absolute color equivalent, and pass them to the
// corresponding CF function.
//
//------------------------------------------------------------------------------
//
void 
SetFillColorI(GFXSurface* io_pSurface,
              Int32       in_index,
              DWORD       in_paletteIndex)
{
   // Recommended form for hardware...
   //
   AssertFatal(io_pSurface->getPalette() != NULL, "No palette attached to surface");

   GFXPalette::MultiPalette* pMultiPalette =
      io_pSurface->getPalette()->findMultiPalette(in_paletteIndex);

   Surface* pSurface = static_cast<Surface*>(io_pSurface);
   Surface::TextureCache* pTxCache = pSurface->getTextureCache();

   float gamma = pTxCache->m_gamma;

   PALETTEENTRY* pColors = pMultiPalette->color;

   ColorF fillColor;
   fillColor.set(pow(float(pColors[in_index].peRed)   / 255.0f, 1.0f / gamma),
                 pow(float(pColors[in_index].peGreen) / 255.0f, 1.0f / gamma),
                 pow(float(pColors[in_index].peBlue)  / 255.0f, 1.0f / gamma));

   if (pSurface->m_alphaSource == GFX_ALPHA_FILL) {
      SetConstantAlpha(io_pSurface, float(pColors[in_index].peFlags) / 255.0f);
   }

   SetFillColorCF(io_pSurface, &fillColor);
}

void 
SetHazeColorI(GFXSurface* io_pSurface,
              Int32       in_index)
{
   // Recommended form for hardware...
   //
   AssertFatal(io_pSurface->getPalette() != NULL, "No palette attached to surface");

   PALETTEENTRY* pColors = io_pSurface->getPalette()->palette[0].color;
   ColorF hazeColor;
   hazeColor.set(float(pColors[in_index].peRed)   / 255.0f,
                 float(pColors[in_index].peGreen) / 255.0f,
                 float(pColors[in_index].peBlue)  / 255.0f);
   SetHazeColorCF(io_pSurface, &hazeColor);
}

void 
SetConstantShadeF(GFXSurface* io_pSurface,
                  float       in_shade)
{
   // Recommended form for hardware...
   //
   ColorF shadeColor;
   shadeColor.set(in_shade, in_shade, in_shade);
   SetConstantShadeCF(io_pSurface, &shadeColor);
}

void 
SetTexturePerspective(GFXSurface* /*io_pSurface*/,
                      Bool        /*perspTex*/)
{
   // Maybe not necessary for accelerated surface?  All vertices have w
   //  infomation passed to AddVertexVTC
   //
   // Always persp correct for OGL
}

void 
HandleSetTextureMap(GFXSurface*      /*io_pSurface*/,
                    const GFXBitmap* /*in_pTexture*/)
{
   // Set the texturemap of the current texture handle
}

GFXHazeSource
GetHazeSource(GFXSurface* io_pSurface)
{
   Surface* pSurface = static_cast<Surface*>(io_pSurface);
   return pSurface->m_hazeSource;
}

float
GetConstantHaze(GFXSurface* io_pSurface)
{
   Surface* pSurface = static_cast<Surface*>(io_pSurface);

   return pSurface->m_constantHaze;
}

} // namespace OpenGL

FunctionTable opengl_table =
{
   OpenGL::ClearScreen,    

   OpenGL::DrawPoint,
   OpenGL::DrawLine2d,
   OpenGL::DrawRect2d,
   OpenGL::DrawRect_f,

   OpenGL::DrawBitmap2d_f,
   OpenGL::DrawBitmap2d_rf,
   OpenGL::DrawBitmap2d_sf,
   NULL,

   GFXDrawText_p,    // Unnecessary to modify
   GFXDrawText_r,    // Unnecessary to modify

   OpenGL::Draw3dBegin,
   OpenGL::Draw3dEnd,
   OpenGL::AddVertexV,
   OpenGL::AddVertexVT,
   OpenGL::AddVertexVC,
   OpenGL::AddVertexVTC,
   OpenGL::EmitPoly,
   OpenGL::SetShadeSource,
   OpenGL::SetHazeSource,
   OpenGL::SetAlphaSource,
   OpenGL::SetFillColorCF,
   OpenGL::SetFillColorI,
   OpenGL::SetHazeColorCF,
   OpenGL::SetHazeColorI,
   OpenGL::SetConstantShadeCF,
   OpenGL::SetConstantShadeF,
   OpenGL::SetConstantHaze,
   OpenGL::SetConstantAlpha,
   OpenGL::SetTransparency,
   OpenGL::SetTextureMap,
   OpenGL::SetFillMode,
   OpenGL::SetTexturePerspective,

   OpenGL::RegisterTexture,
   OpenGL::RegisterTextureCB,
   OpenGL::SetTextureHandle,
   GFXAllocateLightMap,          // Unnecessary to modify
   OpenGL::HandleGetLightMap,
   OpenGL::HandleSetLightMap,
   OpenGL::HandleSetTextureMap,
   OpenGL::HandleSetMipLevel,
   OpenGL::HandleSetTextureSize,
   OpenGL::FlushTextureCache,
   OpenGL::SetZTest,
   OpenGL::ClearZBuffer,
   OpenGL::SetTextureWrap,

   OpenGL::FlushTexture,
   OpenGL::SetZMode,

   OpenGL::SetClipPlanes,

   OpenGL::GetHazeSource,
   OpenGL::GetConstantHaze
};

