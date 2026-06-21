//------------------------------------------------------------------------------
// Description 
//    
// $Workfile$
// $Revision$
// $Author  $
// $Modtime $
//
//------------------------------------------------------------------------------
#include "g_bitmap.h"
#include "g_pal.h"
#include "gfxMetrics.h"
#include "p_txcach.h"

#include "gOGLTx.h"
#include "gOGLSfc.h"
#include <gl/glu.h>

#ifdef DEBUG
#define OGL_ERROR_CHECK(line) while(checkOGLError(line));
#else
#define OGL_ERROR_CHECK(line) ;
#endif

UInt32 g_oglFrameKeyAccum   = 0;
UInt32 g_oglFrameKeyNum     = 0;
float  g_oglAverageFrameKey = 0.0f;

bool g_prefOGLAlwaysRGBA       = false;
bool g_prefOGLUse32BitTex      = false;
bool g_prefOGLNoPackedTex      = false;
bool g_prefOGLNoMipmapping     = false;

UInt32 g_totalNumBytes = 0;

UInt32 g_handleSearches  = 0;
UInt32 g_handleCompares  = 0;
float  g_handleSearchLen = 0.0f;



UInt32 g_texDownloadThisFrame = 0;
UInt32 g_lmDownloadThisFrame = 0;
UInt32 g_lmDownloadBytes     = 0;
#ifdef __EMSCRIPTEN__
// WASM-PORT diagnostics: persistent (cumulative) lightmap-upload telemetry surfaced in the [PATHS]
// console line so it survives scrollback. g_lmDumpTotal counts dumpOGLLightmap calls ever;
// g_lmLastErr is the GL error after the last sub-upload; g_lmMaxLum is the brightest texel luminance
// ever converted (proves whether the lightmap DATA is bright or black).
UInt32 g_lmDumpTotal = 0;
UInt32 g_lmLastErr   = 0;
UInt32 g_lmMaxLum    = 0;
UInt32 g_texLastErr  = 0;  // GL error after the last base (dumpOGLTextureNormal) texture upload
// WASM-PORT diag: why walls/skins render white. Persistent (survive console scroll). For base [N]
// and callback [D] texture uploads: last requested vs resolved palette index (req!=got => getMPCache
// fallback), packed RGB of source-index-0 resolved through that palette, and uni=1 if the source
// indices are uniform (empty/flat texture). Surfaced in the [PATHS] line.
Int32  g_texnReq = -99, g_texnGot = -99; UInt32 g_texnRGB = 0; int g_texnUni = -1;
Int32  g_p1136NB = -1; UInt32 g_p1136Ctr = 0;  // skin palette 1136: non-black entry count + center-pixel rgb (persisted for PATHS)
Int32  g_texdReq = -99, g_texdGot = -99; UInt32 g_texdRGB = 0; int g_texdUni = -1;
UInt32 g_texPalMiss = 0;  // cumulative count of getMPCache palette-fallbacks across both paths
// WASM-PORT diag [TEXCORRUPT]: the user reports textures bound to the WRONG surfaces (player skin on
// the floor, wrong wood, white walls). The HandleCache containment guards already document a "latent
// heap overflow that corrupts the HandleCache". g_hcWildHits counts how often a guard catches a wild
// (out-of-m_array) entry/CacheInfo pointer -> >0 PROVES the cache is being corrupted. g_tbMax* track
// the largest texture written into the 256x256x4 translation buffer; tbMaxExtent > 262144 PROVES a
// translation-buffer heap overflow (the prime corruption source). Surfaced in [PATHS].
UInt32 g_hcWildHits = 0;
UInt32 g_tbMaxW = 0, g_tbMaxH = 0, g_tbMaxExtent = 0;
#endif
// WASM-PORT: when set, setTexture/setLightmap NEVER skip the GL bind on a m_currentTexName match.
// Tests/fixes the floor<->player texture cross-binding: when two bitmaps resolve to the same GL
// texname (cache slot reuse), the redundant-bind skip draws the surface with whatever is already
// bound. Toggle via Module._wasmSetForceBind(1). Default 0 (stock skip behavior).
int g_wasmForceBind = 0;
UInt32 g_oglFrameKey = 1;
UInt32 g_oglEntriesTouched = 0;

namespace {

const GLuint SGIS_TEXTURE_0 = 0x835E;
const GLuint SGIS_TEXTURE_1 = 0x835F;

#undef GL_UNSIGNED_SHORT_5_5_5_1
#undef GL_UNSIGNED_SHORT_4_4_4_4
#undef GL_UNSIGNED_SHORT_1_5_5_5_REV
#undef GL_BGRA
/* WASM-PORT: modern gl.h #defines these */
const GLenum GL_UNSIGNED_SHORT_5_5_5_1     = 0x8034;
const GLenum GL_UNSIGNED_SHORT_4_4_4_4     = 0x8033;
const GLenum GL_UNSIGNED_SHORT_1_5_5_5_REV = 0x8366;

const GLenum GL_BGRA                       = 0x80E1;

#undef GL_CLAMP_TO_EDGE /* WASM-PORT */
const GLenum GL_CLAMP_TO_EDGE              = 0x812F;

#ifndef COLOR_INDEX8_EXT
const GLuint COLOR_INDEX8_EXT = 0x80E5;
#endif


inline bool
checkOGLError(const int in_lineNo)
{
#ifdef DEBUG
   GLenum error = glGetError();
   AssertWarn(error == GL_NO_ERROR, avar("%s: real line: %d", OpenGL::translateOpenGLError(error), in_lineNo));
   return error != GL_NO_ERROR;
#else
   return false;
#endif   
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

inline bool
isPow2(UInt32 size)
{
   return getNextPow2(size) == size;
}

#ifndef max
inline UInt32 
max(UInt32 one, UInt32 two)
{
   return one > two ? one : two;
}
#endif

} // namespace {}


namespace OpenGL {

UInt32            Surface::TextureCache::sm_currFlushNum        = 1;
char const* const Surface::TextureCache::sm_pMultiTextureExtStringARB = "GL_ARB_multitextureBOOGY";
char const* const Surface::TextureCache::sm_pMultiTextureExtStringSGI = "GL_SGIS_multitexture";
char const* const Surface::TextureCache::sm_pPackedPixelsExtString    = "GL_EXT_packed_pixels";
char const* const Surface::TextureCache::sm_pEdgeClampString          = "GL_EXT_texture_edge_clamp";

UInt32       Surface::TextureCache::sm_cacheMagic        = 0xDEAD;
const UInt32 Surface::TextureCache::csm_sizeIndices[6] = {    8,   16,   32,  64, 128, 256 };
const UInt32 Surface::TextureCache::csm_cacheDSize[6]  = { 2048, 1024, 1024, 512,  16,   4 };


inline UInt32
Surface::TextureCache::getEntryArena(const GFXBitmap::CacheInfo& in_rCI)
{
   return (in_rCI.cacheDefined0 >> 16) & 0x000000FF;
}

inline Surface::TextureCache::DynamicCacheEntry*
Surface::TextureCache::getCacheEntry(const GFXBitmap::CacheInfo& in_rCI)
{
   DCEntryVector* pVector = &(m_dynamicEntries[getEntryArena(in_rCI)]);

   return &((*pVector)[in_rCI.cacheDefined0 & 0x0000FFFF]);
}

UInt32
Surface::TextureCache::getArena(const UInt32 in_dim) const
{
   for (int i = 0; i < sizeof(csm_sizeIndices) / sizeof(UInt32); i++) {
      if (in_dim <= csm_sizeIndices[i])
         return i;
   }
   AssertFatal(0, "Should never be here, texture dim too large");
   return 0xF00FF00F;
}


GLuint
Surface::TextureCache::getTexName(const GFXBitmap::CacheInfo& in_rCI) const
{
   // NOTE: Assumes that the cacheinfo is current.  MUST check before
   //  calling this function...
   //
   if ((in_rCI.bitmapSequenceNum & 0xfffffff0) == 0xfffffff0) {
      // Calback or lightmap.
      //
      DynamicCacheEntry* pEntry = const_cast<Surface::TextureCache *>(this)->getCacheEntry(in_rCI);
      return pEntry->texName;
   } else {
      // Normal.
      //
      return in_rCI.cacheDefined1;
   }
}

bool
Surface::TextureCache::isCurrentDynamic(const GFXBitmap::CacheInfo& in_rCI) const
{
   UInt32 cacheMagic = in_rCI.cacheDefined2 & 0x0000ffff;
   if (cacheMagic != sm_cacheMagic)
      return false;

   DynamicCacheEntry* pEntry = const_cast<Surface::TextureCache *>(this)->getCacheEntry(in_rCI);

   return (in_rCI.cacheDefined1 == pEntry->wrapNumber);
}

bool
Surface::TextureCache::isFromCurrentRound(const GFXBitmap::CacheInfo& in_rCI) const
{
   // Isn't callback?
   if (in_rCI.bitmapSequenceNum != 0xffffffff) {
      return in_rCI.cacheDefined0 == sm_currFlushNum;
   } else {
      return isCurrentDynamic(in_rCI);
   }
}

inline bool
Surface::TextureCache::isFromCurrentRoundLM(const GFXBitmap::CacheInfo& in_rCI) const
{
   return isCurrentDynamic(in_rCI);
}

bool
Surface::TextureCache::isCurrent(GFXBitmap::CacheInfo& in_rCacheInfo)
{
   return isFromCurrentRound(in_rCacheInfo);
}

bool
Surface::TextureCache::isCurrentLM(GFXBitmap::CacheInfo& in_rCacheInfo)
{
   return isFromCurrentRoundLM(in_rCacheInfo);
}


Surface::TextureCache::TextureCache(Surface* io_pSurface)
 : m_pSurface(io_pSurface),
   m_currFrameKey(0),
   m_currNormalNames(128),
   m_gamma(1.0f),
   m_supportsPackedPixels(false),
   m_supportsEdgeClamp(false)
{
   sm_cacheMagic++;

   m_pPalette = NULL;
   clearState();

   // WASM-PORT [TEXCORRUPT]: was new UInt8[256*256*4]. EVERY texture upload writes padW*padH*pixelMul
   // bytes into this buffer; ANY texture whose padded dims exceed 256 overflowed it and corrupted the
   // adjacent heap (the HandleCache), binding wrong textures to surfaces (skin-on-floor, white walls).
   // Size for up to 1024x1024 RGBA; [PATHS] g_tbMaxExtent confirms whether anything still exceeds this.
   m_pTranslationBuffer = new UInt8[1024 * 1024 * 4];

   memset(&m_multiTexExtSGI, 0, sizeof(m_multiTexExtSGI));
   memset(&m_multiTexExtARB, 0, sizeof(m_multiTexExtARB));
   determineExtensions();

   GLenum dstFmt;
   if (g_prefOGLUse32BitTex)
      dstFmt = GL_RGBA8;
   else {
      if (g_prefOGLNoPackedTex)
         dstFmt = GL_RGB;
      else
         dstFmt = GL_RGB5_A1;
   }

   GLenum srcFmt = GL_RGBA;
   if (g_prefOGLNoPackedTex)
      srcFmt = GL_RGB;

   UInt8* pDummyData = new UInt8[256 * 256 * 4];
   memset(pDummyData, 0xFF, 256 * 256 * 2);

   for (int i = 0; i < sizeof(csm_sizeIndices) / sizeof(UInt32); i++) {
      DCEntryVector& rVector = m_dynamicEntries[i];

      rVector.setSize(csm_cacheDSize[i]);
      for (int j = 0; j < rVector.size(); j++) {
         DynamicCacheEntry& rCEntry = rVector[j];

         rCEntry.pNext = &(rVector[j + 1]);
         rCEntry.pPrev = &(rVector[j - 1]);
         rCEntry.wrapNumber = 1;

         glGenTextures(1, &rCEntry.texName);
         glBindTexture(GL_TEXTURE_2D, rCEntry.texName);
         glTexImage2D(GL_TEXTURE_2D,
                      0,
                      dstFmt,
                      csm_sizeIndices[i], csm_sizeIndices[i],
                      0,
                      srcFmt,
                      GL_UNSIGNED_BYTE,
                      pDummyData);

         rCEntry.frameKey   = 0xFFFFFFFF;
      }

      g_totalNumBytes += csm_sizeIndices[i] * csm_sizeIndices[i] * rVector.size() * 2;

      rVector[rVector.size() - 1].pPrev = &(rVector[rVector.size() - 2]);
      rVector[rVector.size() - 1].pNext = NULL;
      rVector[0].pPrev = &m_freeListSentrys[i];

      m_freeListSentrys[i].pNext = &(rVector[0]);
      m_freeListSentrys[i].pPrev = (DynamicCacheEntry*)0xffffffff;

      m_inuseSentryHeads[i].pNext = &m_inuseSentryTails[i];
      m_inuseSentryHeads[i].pPrev = (DynamicCacheEntry*)0xffffffff;
      m_inuseSentryTails[i].pPrev = &m_inuseSentryHeads[i];
      m_inuseSentryTails[i].pNext = (DynamicCacheEntry*)0xffffffff;
   }
   delete [] pDummyData;
}

Surface::TextureCache::~TextureCache()
{
   AssertFatal(m_currNormalNames.size() == 0,
               "Cache hasn't been properly flushed!");

   delete [] m_pTranslationBuffer;
   m_pTranslationBuffer = 0;
   clearState();

   TextureNameVector tempNameVector;
   for (int i = 0; i < sizeof(csm_sizeIndices) / sizeof(UInt32); i++) {
      DCEntryVector& rVector = m_dynamicEntries[i];

      for (int j = 0; j < rVector.size(); j++) {
         tempNameVector.push_back(rVector[j].texName);
      }
   }
   glDeleteTextures(tempNameVector.size(),
                    tempNameVector.begin());

   m_pSurface = NULL;
}

void
Surface::TextureCache::determineExtensions()
{
   const char* pExtString = m_pSurface->m_pExtensionsString;

   if (strstr(pExtString, sm_pMultiTextureExtStringARB) != NULL) {
      // ARB Multitexture supported
   } else if (strstr(pExtString, sm_pMultiTextureExtStringSGI) != NULL) {
      // SGIS Multitexture supported
      //
      m_multiTexExtSGI.glMTexCoord2fv  = (void (__stdcall*)(GLenum, GLfloat*)) wglGetProcAddress("glMultiTexCoord2fvSGIS");
      m_multiTexExtSGI.glMTexCoord4fv  = (void (__stdcall*)(GLenum, GLfloat*)) wglGetProcAddress("glMultiTexCoord4fvSGIS");
      m_multiTexExtSGI.glSelectTexture = (void (__stdcall*)(GLenum))           wglGetProcAddress("glSelectTextureSGIS");
   } else {
      // Multitexture not supported
   }

   if (strstr(pExtString, sm_pPackedPixelsExtString) != NULL)
      m_supportsPackedPixels = true;
   else
      m_supportsPackedPixels = false;

   if (strstr(pExtString, sm_pEdgeClampString) != NULL)
      m_supportsEdgeClamp = true;
   else
      m_supportsEdgeClamp = false;
}

void
Surface::TextureCache::setTexMode(const GLenum in_mode0,
                                  const GLenum in_mode1)
{
   if (m_texUnitMode[0] != in_mode0) {
      setTexUnit(0);
      glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, in_mode0);
   }

   if (m_texUnitMode[1] != in_mode1) {
      if (supportsSGIMultiTexture() == true) {
         setTexUnit(1);
         glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, in_mode1);
      }
   }

   m_texUnitMode[0] = in_mode0;
   m_texUnitMode[1] = in_mode1;
}

void
Surface::TextureCache::enableTexUnits(const bool in_texUnit0,
                                      const bool in_texUnit1)
{
   GeneralState desired0 = in_texUnit0 ? Enabled : Disabled;
   GeneralState desired1 = in_texUnit1 ? Enabled : Disabled;

   if (m_texUnitState[0] != desired0) {
      setTexUnit(0);
      if (in_texUnit0) {
         glEnable(GL_TEXTURE_2D);
      } else {
         glDisable(GL_TEXTURE_2D);
      }
   }

   if (m_texUnitState[1] != desired1) {
      if (supportsSGIMultiTexture() == true) {
         setTexUnit(1);
         if (in_texUnit1) {
            glEnable(GL_TEXTURE_2D);
         } else {
            glDisable(GL_TEXTURE_2D);
         }
      }
   }
         
   m_texUnitState[0] = desired0;
   m_texUnitState[1] = desired1;
}

GLuint
Surface::TextureCache::chooseDynamicName(GFXBitmap::CacheInfo& in_rCacheInfo,
                                         const UInt32          in_arena)
{
   DynamicCacheEntry* pEntry;
   if (m_freeListSentrys[in_arena].pNext != NULL) {
      // Still a few textures left on the free list...
      //
      pEntry = m_freeListSentrys[in_arena].pNext;
      m_freeListSentrys[in_arena].pNext = pEntry->pNext;

      if (pEntry->pNext)
         pEntry->pNext->pPrev = &m_freeListSentrys[in_arena];

      pEntry->frameKey = m_currFrameKey;
   } else {
      // Reuse LRU
      //
      pEntry = m_inuseSentryTails[in_arena].pPrev;
      AssertFatal(pEntry != &m_inuseSentryHeads[in_arena], "Bogus!  Tail shouldn't point to head here");

      g_oglFrameKeyAccum += m_currFrameKey - pEntry->frameKey;
      g_oglFrameKeyNum++;

      pEntry->pPrev->pNext = pEntry->pNext;
      pEntry->pNext->pPrev = pEntry->pPrev;

      pEntry->wrapNumber++;
      pEntry->frameKey = m_currFrameKey;
   }
   AssertFatal(pEntry != NULL, "No Entry?");

   in_rCacheInfo.cacheDefined0 = ((in_arena << 16) & 0x00FF0000) |
                                 (pEntry - m_dynamicEntries[in_arena].begin()) & 0x0000FFFF;

   in_rCacheInfo.cacheDefined1 = pEntry->wrapNumber;
   in_rCacheInfo.cacheDefined2 = ((in_rCacheInfo.cacheDefined2 & 0xffff0000) |
                                  sm_cacheMagic);

   // Insert our entry at the head of the use list...
   //
   pEntry->pNext                      = m_inuseSentryHeads[in_arena].pNext;
   pEntry->pNext->pPrev               = pEntry;
   m_inuseSentryHeads[in_arena].pNext = pEntry;
   pEntry->pPrev                      = &m_inuseSentryHeads[in_arena];

   return pEntry->texName;
}

void
Surface::TextureCache::touchEntryIfNecessary(GFXBitmap::CacheInfo& in_rCacheInfo)
{
   if ((in_rCacheInfo.bitmapSequenceNum & 0xfffffff0) == 0xfffffff0) {
      // Necessary...
      if ((in_rCacheInfo.cacheDefined2 & 0x0000ffff) == sm_cacheMagic) {
         // Really necessary...
         DynamicCacheEntry* pEntry = getCacheEntry(in_rCacheInfo);
         UInt32         arenaIndex = getEntryArena(in_rCacheInfo);

         pEntry->pPrev->pNext = pEntry->pNext;
         pEntry->pNext->pPrev = pEntry->pPrev;

         pEntry->pNext                        = m_inuseSentryHeads[arenaIndex].pNext;
         pEntry->pPrev                        = &m_inuseSentryHeads[arenaIndex];
         pEntry->pNext->pPrev                 = pEntry;
         m_inuseSentryHeads[arenaIndex].pNext = pEntry;
      }
   }
}

void
Surface::TextureCache::cacheBitmap(const GFXBitmap*      in_pBitmap,
                                   GFXBitmap::CacheInfo& in_rCacheInfo,
                                   int                   in_texUnit,
                                   const bool            in_clampMode)
{
   AssertFatal(in_pBitmap != NULL, "Error, no bitmap");
   AssertFatal(in_pBitmap->getWidth() <= 256 && in_pBitmap->getHeight() <= 256,
               "Maximum width of bitmap is 256");

   GLenum magFilter, minFilter;

   if (isFromCurrentRound(in_rCacheInfo) == false) {
      setTexUnit(in_texUnit);
      if (in_rCacheInfo.bitmapSequenceNum != 0xffffffff) {
         // Bitmap has not been set in this cache round, download it
         m_currNormalNames.increment();
         GLuint& rName = m_currNormalNames.last();
         glGenTextures(1, &rName);

         in_rCacheInfo.cacheDefined0 = sm_currFlushNum;
         in_rCacheInfo.cacheDefined1 = rName;
         glBindTexture(GL_TEXTURE_2D, rName);
         m_currentTexName[in_texUnit] = rName;

         if ((in_pBitmap->attribute & BMA_TRANSPARENT) == 0) magFilter = GL_LINEAR;
         else                                                magFilter = GL_NEAREST;

         if (g_prefOGLNoMipmapping == false) {
            if (in_pBitmap->detailLevels != 1) minFilter = GL_LINEAR_MIPMAP_NEAREST;
            else                               minFilter = GL_LINEAR;
         } else {
            minFilter = GL_LINEAR;
         }

         if (supportsPackedPixels() == false || (g_prefOGLUse32BitTex || g_prefOGLNoPackedTex)) {
            if (in_pBitmap->attribute      & BMA_TRANSLUCENT) dumpOGLTextureAlpha(in_pBitmap, in_clampMode);
            else if (in_pBitmap->attribute & BMA_TRANSPARENT) dumpOGLTextureTransparent(in_pBitmap, in_clampMode);
            else                                              dumpOGLTextureNormal(in_pBitmap, in_clampMode);
         } else {
            dumpOGLTexturePacked(in_pBitmap,
                                 (in_pBitmap->attribute & BMA_TRANSPARENT) != 0,
                                 in_clampMode);
         }
      } else {
         // Ah, callback texture!
         //
         UInt32 arena = getArena(max(getNextPow2(in_pBitmap->getWidth()),
                                     getNextPow2(in_pBitmap->getHeight())));

         GLuint texName;
         texName = chooseDynamicName(in_rCacheInfo, arena);

         glBindTexture(GL_TEXTURE_2D, texName);
         m_currentTexName[in_texUnit] = texName;

         magFilter = GL_LINEAR;
         minFilter = GL_LINEAR;

         if (supportsPackedPixels() == false || (g_prefOGLUse32BitTex || g_prefOGLNoPackedTex)) {
            dumpOGLTextureNormalDyn(in_pBitmap, in_clampMode);
         } else {
            dumpOGLTexturePackedDyn(in_pBitmap, false, in_clampMode);
         }
      }
   }

   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilter);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
}

// Callbacks are never refreshed, we don't need to worry about them...
void
Surface::TextureCache::refreshBitmap(const GFXBitmap*      in_pBitmap,
                                     GFXBitmap::CacheInfo& in_rCacheInfo)
{
   AssertFatal(in_pBitmap != NULL, "Error, no bitmap");

   if (isFromCurrentRound(in_rCacheInfo) == false) {
      // Bitmap has not been set in this cache round, download it
      cacheBitmap(in_pBitmap, in_rCacheInfo, 0, true);
   } else {
      setTexUnit(0);
      glBindTexture(GL_TEXTURE_2D, getTexName(in_rCacheInfo));
      m_currentTexName[0] = getTexName(in_rCacheInfo);

      if (supportsPackedPixels() == false || (g_prefOGLUse32BitTex || g_prefOGLNoPackedTex)) {
         if (in_pBitmap->attribute      & BMA_TRANSLUCENT) refreshOGLTextureAlpha(in_pBitmap, true);
         else if (in_pBitmap->attribute & BMA_TRANSPARENT) refreshOGLTextureTransparent(in_pBitmap, true);
         else                                              refreshOGLTextureNormal(in_pBitmap, true);
      } else {
         refreshOGLTexturePacked(in_pBitmap,
                                 (in_pBitmap->attribute & BMA_TRANSPARENT) != 0,
                                 true);
      }
   }
}

bool
Surface::TextureCache::setTexture(GFXBitmap::CacheInfo& in_rCacheInfo, int in_texUnit)
{
   if (isFromCurrentRound(in_rCacheInfo) == false)
      return false;

   touchEntryIfNecessary(in_rCacheInfo);
   if (!::g_wasmForceBind && m_currentTexName[in_texUnit] == (int)getTexName(in_rCacheInfo))
      return true;

   // Bind the texture
   setTexUnit(in_texUnit);
   glBindTexture(GL_TEXTURE_2D, getTexName(in_rCacheInfo));
   m_currentTexName[in_texUnit] = getTexName(in_rCacheInfo);
   OGL_ERROR_CHECK(__LINE__);

   return true;
}

bool
Surface::TextureCache::setLightmap(GFXBitmap::CacheInfo& in_rCacheInfo, int in_texUnit)
{
   if (isFromCurrentRoundLM(in_rCacheInfo) == false)
      return false;

   touchEntryIfNecessary(in_rCacheInfo);
   if (!::g_wasmForceBind && m_currentTexName[in_texUnit] == (int)getTexName(in_rCacheInfo))
      return true;

   // Bind the texture
   setTexUnit(in_texUnit);
   glBindTexture(GL_TEXTURE_2D, getTexName(in_rCacheInfo));
   m_currentTexName[in_texUnit] = getTexName(in_rCacheInfo);
   OGL_ERROR_CHECK(__LINE__);

   return true;
}

void
Surface::TextureCache::cacheLightmap(GFXLightMap*          io_pLightmap,
                                     GFXBitmap::CacheInfo& in_rCacheInfo,
                                     int                   in_texUnit)
{
   AssertFatal(io_pLightmap != NULL, "Error, no bitmap");

   if (isFromCurrentRoundLM(in_rCacheInfo) == false) {
      // Lightmap has not been set in this cache round, download it
      UInt32 realX = (io_pLightmap->size.x != 0) ? io_pLightmap->size.x : 256;
      UInt32 realY = (io_pLightmap->size.y != 0) ? io_pLightmap->size.y : 256;

      UInt32 arena = getArena(max(getNextPow2(realX),
                                  getNextPow2(realY)));
      GLuint name = chooseDynamicName(in_rCacheInfo, arena);

      setTexUnit(in_texUnit);
      glBindTexture(GL_TEXTURE_2D, name);
      m_currentTexName[in_texUnit] = getTexName(name);

      if (supportsPackedPixels() == false || (g_prefOGLUse32BitTex || g_prefOGLNoPackedTex))
         dumpOGLLightmap(io_pLightmap);
      else
         dumpOGLLightmapPacked(io_pLightmap);

      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      OGL_ERROR_CHECK(__LINE__);
   }
   OGL_ERROR_CHECK(__LINE__);
}

void
Surface::TextureCache::flushLightMap(GFXBitmap::CacheInfo& in_rCacheInfo)
{
   if (isFromCurrentRoundLM(in_rCacheInfo) == false)
      return;

   int name = getTexName(in_rCacheInfo);
   if (name == m_currentTexName[0]) m_currentTexName[0] = -1;
   if (name == m_currentTexName[1]) m_currentTexName[1] = -1;

   in_rCacheInfo.cacheDefined0 = 0xffffffff;
   in_rCacheInfo.cacheDefined1 = 0;
   in_rCacheInfo.cacheDefined2 = 0;
}

void
Surface::TextureCache::flushCache()
{
   sm_currFlushNum++;

   glDeleteTextures(m_currNormalNames.size(),
                    m_currNormalNames.begin());
   m_currNormalNames.clear();
   OGL_ERROR_CHECK(__LINE__);

   for (int i = 0; i < sizeof(csm_sizeIndices) / sizeof(UInt32); i++) {
      while (m_inuseSentryHeads[i].pNext != &m_inuseSentryTails[i]) {
         DynamicCacheEntry* pEntry = m_inuseSentryHeads[i].pNext;

         pEntry->wrapNumber++;

         AssertFatal(pEntry->pPrev == &m_inuseSentryHeads[i], "Hrmph.");
         pEntry->pNext->pPrev        = pEntry->pPrev;
         m_inuseSentryHeads[i].pNext = pEntry->pNext;
         pEntry->pPrev               = &m_freeListSentrys[i];
         pEntry->pNext               = m_freeListSentrys[i].pNext;
         m_freeListSentrys[i].pNext  = pEntry;
         if (pEntry->pNext)
            pEntry->pNext->pPrev     = pEntry;
      }
   }

   clearState();
}

void
Surface::TextureCache::setGamma(const float in_gamma)
{
   AssertFatal(in_gamma != 0.0f, "bad gamma!");
   m_gamma = in_gamma;

   setPalette(m_pPalette);
}

void
Surface::TextureCache::setPalette(GFXPalette* in_pPalette)
{
   if (in_pPalette == NULL) {
      m_pPalette = NULL;
      return;
   }
   m_pPalette = in_pPalette;

   int i;
   GLubyte gammaMap[256];
   float oog = 1.0f / m_gamma;
   for (i = 0; i < 256; i++) {
      gammaMap[i] = GLubyte(pow((float(i) / 255.0f), oog) * 255.0f);
   }

   for (i = 0; i < 16; i++) m_multiPalettes[i].paletteIndex = -3;

   for (i = 0; i < in_pPalette->numPalettes; i++) {
      m_multiPalettes[i].paletteIndex = m_pPalette->palette[i].paletteIndex;

      for (int j = 0; j < 256; j++) {
         PALETTEENTRY& rColor = m_pPalette->palette[i].color[j];

         m_multiPalettes[i].data[j * 4 + 0] = gammaMap[rColor.peRed];
         m_multiPalettes[i].data[j * 4 + 1] = gammaMap[rColor.peGreen];
         m_multiPalettes[i].data[j * 4 + 2] = gammaMap[rColor.peBlue];
         m_multiPalettes[i].data[j * 4 + 3] = rColor.peFlags;

         m_multiPalettes[i].dataTransparent[j * 4 + 0] = gammaMap[rColor.peRed];
         m_multiPalettes[i].dataTransparent[j * 4 + 1] = gammaMap[rColor.peGreen];
         m_multiPalettes[i].dataTransparent[j * 4 + 2] = gammaMap[rColor.peBlue];
         m_multiPalettes[i].dataTransparent[j * 4 + 3] = (j == 0 ? 0 : 255);

         if (m_pPalette->palette[i].paletteType == GFXPalette::ShadeHazePaletteType   ||
             m_pPalette->palette[i].paletteType == GFXPalette::AdditivePaletteType    ||
             m_pPalette->palette[i].paletteType == GFXPalette::SubtractivePaletteType ||
             m_pPalette->palette[i].paletteType == GFXPalette::NoRemapPaletteType) {
            // 5R - 5G - 5B - 1A
            //
            m_multiPalettes[i].dataPacked[j] = ((gammaMap[rColor.peRed]   << 8) & 0xF800) |
                                               ((gammaMap[rColor.peGreen] << 3) & 0x07C0) |
                                               ((gammaMap[rColor.peBlue]  >> 2) & 0x003E) |
                                               0x0001;
            m_multiPalettes[i].dataPackedTransparent[j] = ((gammaMap[rColor.peRed]   << 8) & 0xF800) |
                                                          ((gammaMap[rColor.peGreen] << 3) & 0x07C0) |
                                                          ((gammaMap[rColor.peBlue]  >> 2) & 0x003E) |
                                                          (j == 0 ? 0x0000 : 0x0001);
           
            m_multiPalettes[i].packedType            = GL_UNSIGNED_SHORT_5_5_5_1;
            m_multiPalettes[i].packedTransparentType = GL_UNSIGNED_SHORT_5_5_5_1;
         }
         else if (m_pPalette->palette[i].paletteType == GFXPalette::TranslucentPaletteType) {
            // 4R - 4G - 4B - 4A
            m_multiPalettes[i].dataPacked[j] = ((gammaMap[rColor.peRed]   << 8) & 0xF000) |
                                               ((gammaMap[rColor.peGreen] << 4) & 0x0F00) |
                                               ((gammaMap[rColor.peBlue]  >> 0) & 0x00F0) |
                                               ((rColor.peFlags >> 4)           & 0x000F);
            m_multiPalettes[i].dataPackedTransparent[j] = ((gammaMap[rColor.peRed]   << 8) & 0xF000) |
                                                          ((gammaMap[rColor.peGreen] << 4) & 0x0F00) |
                                                          ((gammaMap[rColor.peBlue]  >> 0) & 0x00F0) |
                                                          ((rColor.peFlags >> 4)           & 0x000F);

            m_multiPalettes[i].packedType            = GL_UNSIGNED_SHORT_4_4_4_4;
            m_multiPalettes[i].packedTransparentType = GL_UNSIGNED_SHORT_4_4_4_4;
         }
         else {
            AssertFatal(0, avar("Bad palette type: %d", m_pPalette->palette[i].paletteType));
         }
      }
   }
}


//------------------------------------------------------------------------------
//--------------------------------------
// Handle Cache Implementation
//--------------------------------------
//
HandleCacheEntry::HandleCacheEntry()
 : bitmapCacheInfo(0xffffffff)
{
   pLightmapCacheInfo = new GFXBitmap::CacheInfo(0xfffffffe);
   isTerrain = false;
   frameKey = 0;
}

HandleCacheEntry::~HandleCacheEntry()
{
   if (pLightmapCacheInfo && pLightmapCacheInfo->bitmapSequenceNum != 0xfffffffd)
      delete pLightmapCacheInfo;
   pLightmapCacheInfo = NULL;
}

inline UInt32
HandleCache::HashHandle(GFXTextureHandle in_tex)
{
   UInt32 k = (in_tex.key[0] >> 5) ^ ((in_tex.key[1] >> 11) ^ in_tex.key[1]);

   return k % m_entryTableSize;
}

inline bool
HandleCache::HandleEqual(const GFXTextureHandle& in_t1,
                         const GFXTextureHandle& in_t2)
{
   return (in_t1.key[0] == in_t2.key[0]) && (in_t1.key[1] == in_t2.key[1]);
}


void
HandleCache::touch(HandleCacheEntry* io_pEntry)
{
   io_pEntry->prev->next = io_pEntry->next;
   io_pEntry->next->prev = io_pEntry->prev;

   io_pEntry->next       = &m_freeListTail;
   io_pEntry->prev       = m_freeListTail.prev;
   io_pEntry->prev->next = io_pEntry;
   m_freeListTail.prev   = io_pEntry;

   if (io_pEntry->frameKey != g_oglFrameKey)
      g_oglEntriesTouched++;
   io_pEntry->frameKey = g_oglFrameKey;
}

#ifdef __EMSCRIPTEN__
extern "C" void* sbrk(long);   // WASM-PORT: heap-top probe for the getFreeEntry containment guard
#endif

HandleCacheEntry*
HandleCache::getFreeEntry(Surface::TextureCache* io_pTextureCache)
{
   HandleCacheEntry* ret = m_freeListHead.next;
   AssertFatal((ret->prev == &m_freeListHead) && (m_freeListHead.next == ret),
               "Error");
   touch(ret);

#ifdef __EMSCRIPTEN__
   // WASM-PORT: containment guard. The same latent heap overflow that corrupts the HandleCache hash
   // chain (see setTextureHandle below) can leave the evicted LRU entry's pLightmapCacheInfo a
   // non-allocation (wild) pointer. The "delete ret->pLightmapCacheInfo" below then traps "memory
   // access out of bounds" INSIDE free() during ITRRender interior texturing (TSControl::onRender ->
   // InteriorShape::render -> ITRRender::renderSurfacesOGL -> RegisterTexture -> getFreeEntry). The
   // read at bitmapSequenceNum survives (it's a readable address) but free() faults on the bogus
   // heap metadata. If ret is outside m_array, or its CacheInfo isn't a plausible heap pointer, skip
   // the flush+free (leak the wild pointer — it was never a real allocation) and install a fresh
   // CacheInfo so the entry stays usable. Containment, not cure — the upstream overflow is the NEXT.
   bool wpSafe = true;
   {
      char* heapTop = (char*)sbrk(0);
      if (ret < m_array || ret >= m_array + m_numHandles)
         wpSafe = false;
      else {
         char* ci = (char*)ret->pLightmapCacheInfo;
         if (!(ci >= (char*)1024 && ci < heapTop))
            wpSafe = false;
      }
      if (!wpSafe) g_hcWildHits++;   // WASM-PORT diag [TEXCORRUPT]: HandleCache entry was clobbered
   }
   if (wpSafe && ret->pLightmapCacheInfo->bitmapSequenceNum != 0xfffffffd) {
      io_pTextureCache->flushLightMap(*(ret->pLightmapCacheInfo));
      if(ret->pLMap!= NULL) {
         gfxLightMapCache.release(ret->pLMap);
         ret->pLMap = NULL;
      }

      delete ret->pLightmapCacheInfo;
   } else {
      // Terrain special lightmap, OR (wasm) a clobbered entry whose CacheInfo we must not free.
      ret->pLMap = NULL;
   }
#else
   if (ret->pLightmapCacheInfo->bitmapSequenceNum != 0xfffffffd) {
      io_pTextureCache->flushLightMap(*(ret->pLightmapCacheInfo));
      if(ret->pLMap!= NULL) {
         gfxLightMapCache.release(ret->pLMap);
         ret->pLMap = NULL;
      }

      delete ret->pLightmapCacheInfo;
   } else {
      // Terrain special lightmap
      ret->pLMap = NULL;
   }
#endif
   ret->pLightmapCacheInfo = new GFXBitmap::CacheInfo(0xfffffffe);
   ret->bitmapCacheInfo.clear();

   int index = HashHandle(ret->handle);
   HandleCacheEntry** walk = &m_entryTable[index];
   while (*walk)
   {
      if(*walk == ret)
      {
         *walk = ret->hashLink;
         break;
      }
      walk = &((*walk)->hashLink);
   }
   ret->hashLink = NULL;

   m_currentEntry = ret;
   return ret;
}

void HandleCache::HashInsert(HandleCacheEntry *tce)
{
   int index = HashHandle(tce->handle);
   tce->hashLink       = m_entryTable[index];
   m_entryTable[index] = tce;
}

Bool HandleCache::setTextureHandle(GFXTextureHandle handle)
{
   int index = HashHandle(handle);
   m_currentEntry = m_entryTable[index];

   int comp = 0;
   while (m_currentEntry) {
      comp++;
#ifdef __EMSCRIPTEN__
      // WASM-PORT: defend against a corrupted hash bucket/chain. A bucket head or hashLink can be a
      // WILD (non-NULL, out-of-heap) pointer in ITRRender's interior texturing — dereferencing
      // m_currentEntry->handle then traps "memory access out of bounds" (the bucket gets a bad
      // pointer when an entry is evicted/rehashed under a changed handle). Desktop tolerated the
      // wild read; wasm faults. Validate that the entry points into m_array (the only legal entries)
      // before touching it; anything else is treated as a miss so the caller re-caches the texture.
      // Also cap the walk length as a backstop.
      if (m_currentEntry < m_array || m_currentEntry >= (m_array + m_numHandles) ||
          comp > m_numHandles) {
         g_hcWildHits++;   // WASM-PORT diag [TEXCORRUPT]: hash chain had a wild pointer
         m_currentEntry = NULL;
         break;
      }
#endif
      if(HandleEqual(m_currentEntry->handle, handle))
         break;
      m_currentEntry = m_currentEntry->hashLink;
   }
   g_handleSearches++;
   g_handleCompares += comp;
   g_handleSearchLen = float(g_handleCompares) / float(g_handleSearches);

   if (m_currentEntry != NULL) {
      touch(m_currentEntry);
      return TRUE;
   }

   return FALSE;
}

HandleCache::HandleCache(int numHandles, int handleHashTableSize)
  : m_numHandles(numHandles)
{
   m_array = new HandleCacheEntry[m_numHandles];

   for(int i = 0; i < m_numHandles; i++) {
      m_array[i].next = m_array + i + 1;
      m_array[i].prev = m_array + i - 1;
      m_array[i].pLMap = NULL;
   }
   m_array[0].prev                = &m_freeListHead;
   m_array[m_numHandles - 1].next = &m_freeListTail;

   m_freeListHead.next = &m_array[0];
   m_freeListTail.prev = &m_array[m_numHandles - 1];

   m_freeListHead.prev = m_freeListTail.next = NULL;

   m_freeListHead.pLMap = (GFXLightMap*)0xffffffff;
   m_freeListTail.pLMap = (GFXLightMap*)0xffffffff;

   m_entryTableSize = handleHashTableSize;
   m_entryTable = new HandleCacheEntry*[m_entryTableSize];
   for(int j = 0; j < m_entryTableSize; j++)
      m_entryTable[j] = NULL;

   m_currentEntry = NULL;
}

HandleCache::~HandleCache()
{
   delete[] m_array;
   delete[] m_entryTable;
}

void HandleCache::flush()
{
   m_currentEntry = NULL;
   for(int i = 0; i < m_entryTableSize; i++)
      m_entryTable[i] = NULL;
   
   // Whip through and release all the lightmaps...
   //
   for (int j = 0; j < m_numHandles; j++) {
      HandleCacheEntry& rCEntry = m_array[j];
      AssertFatal(rCEntry.pLightmapCacheInfo != NULL, "No lmap cache info?");

      if (rCEntry.pLightmapCacheInfo->bitmapSequenceNum != 0xfffffffd) {
         // Not a terrain lightmap, we can flush this normally...
         //
         if (rCEntry.pLMap != NULL) {
            gfxLightMapCache.release(rCEntry.pLMap);
            rCEntry.pLMap = NULL;
         }

         rCEntry.pLightmapCacheInfo->clear();
      } else {
         // Terrain lightmap.  Don't release, replace CacheInfo with
         //  normal cacheinfo...
         //
         rCEntry.pLightmapCacheInfo = new GFXBitmap::CacheInfo(0xfffffffe);
         rCEntry.pLMap = NULL;
      }

      rCEntry.hashLink = NULL;
      rCEntry.bitmapCacheInfo.clear();
   }
}


//------------------------------------------------------------------------------
//-------------------------------------- Util dump functions...
//
void
Surface::TextureCache::dumpOGLTextureNormal(const GFXBitmap* in_pBitmap,
                                            const bool       in_clamp)
{
   g_texDownloadThisFrame++;
   AssertFatal(in_pBitmap != NULL, "No bitmap to dump");
   AssertFatal(m_pPalette != NULL, "No palette set to cache");
   AssertFatal(m_pTranslationBuffer != NULL, "No translation buffer!");

   MPCacher* pMPCache = getMPCache(in_pBitmap->paletteIndex);

#ifdef __EMSCRIPTEN__
   {  // WASM-PORT diag [TEXN] (base path) -> persistent globals (see definitions above).
      const UInt8* p0 = in_pBitmap->pMipBits[0];
      int n = in_pBitmap->getWidth() * in_pBitmap->getHeight();
      UInt8 i0 = p0 ? p0[0] : 0; bool uni = true;
      for (int k = 1; k < n && k < 512 && p0; k++) if (p0[k] != i0) { uni = false; break; }
      const UInt8* pal = &pMPCache->data[i0 * 4];
      g_texnReq = in_pBitmap->paletteIndex; g_texnGot = pMPCache->paletteIndex;
      g_texnRGB = (pal[0] << 16) | (pal[1] << 8) | pal[2]; g_texnUni = uni ? 1 : 0;
      if (g_texnReq != g_texnGot) g_texPalMiss++;
      // WASM-PORT diag [SKINPAL]: when this base-path texture (player skins/shape maps) misses its
      // palette, classify it: dump the bitmap's dims + wanted paletteIndex + uniform-flag, and ALL 16
      // world multipalette indices that ARE available. If 0 is simply absent from the list -> the
      // world .ppl lacks the skin sub-palette (asset/palette-list fix). If the bitmap's own
      // paletteIndex looks defaulted/wrong vs available ones -> a load-time assignment bug. One-shot
      // per distinct (w,h,reqPal) so it names each failing texture once without flooding.
      if (g_texnReq != g_texnGot) {
         static UInt32 s_seen[32]; static int s_nSeen = 0;
         UInt32 sig = (UInt32)((in_pBitmap->getWidth() << 20) ^ (in_pBitmap->getHeight() << 8)
                     ^ (UInt32)(in_pBitmap->paletteIndex & 0xff));
         bool dup = false;
         for (int s = 0; s < s_nSeen; s++) if (s_seen[s] == sig) { dup = true; break; }
         if (!dup && s_nSeen < 32) {
            s_seen[s_nSeen++] = sig;
            printf("[SKINPAL] bmp %dx%d reqPal=%d uni=%d mip0=%p i0=%d | worldPals[",
                   in_pBitmap->getWidth(), in_pBitmap->getHeight(), (int)in_pBitmap->paletteIndex,
                   uni ? 1 : 0, (void*)p0, (int)i0);
            for (int s = 0; s < 16; s++) printf("%d ", (int)m_multiPalettes[s].paletteIndex);
            printf("] mpNumPals=%d\n", m_pPalette ? (int)m_pPalette->numPalettes : -1);
         }
      }
   }
#endif

#ifdef __EMSCRIPTEN__
   {  // WASM-PORT diag [PAL1136]: decisive skin test (option A). The player skin (.pbmp, base.larmor)
      // wants paletteIndex 1136 and getMPCache resolves it (got=1136), yet the model renders dark.
      // texN rgb only sampled the CORNER texel (likely transparent idx0=black). Here: count how many of
      // the resolved palette's 256 entries are non-black, dump the first 4 RGBs, and convert a CENTER
      // pixel. nonBlack high + real center color => palette is fine => darkness is a DRAW/modulation
      // issue in the shape path. nonBlack ~0 => palette 1136 itself is black => PBMP skin-palette not
      // loaded into the multipalette set. One-shot (first 4 skin uploads).
      static int s_p1136 = 0;
      if (in_pBitmap->paletteIndex == 1136 && s_p1136 < 4) {
         s_p1136++;
         int nb = 0;
         for (int e = 0; e < 256; e++) {
            const UInt8* c = &pMPCache->data[e * 4];
            if (c[0] || c[1] || c[2]) nb++;
         }
         const UInt8* d  = pMPCache->data;
         const UInt8* p0 = in_pBitmap->pMipBits[0];
         int n  = in_pBitmap->getWidth() * in_pBitmap->getHeight();
         int ci = p0 ? p0[n / 2] : 0;                 // center-ish pixel index
         const UInt8* cc = &pMPCache->data[ci * 4];
         g_p1136NB  = nb;
         g_p1136Ctr = (cc[0] << 16) | (cc[1] << 8) | cc[2];
         printf("[PAL1136] got=%d nonBlack=%d/256 pal0123=%02x%02x%02x,%02x%02x%02x,%02x%02x%02x,%02x%02x%02x"
                " | centerIdx=%d -> rgb=%02x%02x%02x\n",
                (int)pMPCache->paletteIndex, nb,
                d[0], d[1], d[2], d[4], d[5], d[6], d[8], d[9], d[10], d[12], d[13], d[14],
                ci, cc[0], cc[1], cc[2]);
      }
   }
#endif

   // We have to make sure that the widths are pow2s.
   int padWidth  = getNextPow2(in_pBitmap->getStride());
   int padHeight = getNextPow2(in_pBitmap->getHeight());

#ifdef __EMSCRIPTEN__
   {  // WASM-PORT diag [TEXCORRUPT]: largest write into the (formerly 256x256x4) translation buffer.
      UInt32 ext = (UInt32)padWidth * (UInt32)padHeight * 4;
      if ((UInt32)padWidth  > g_tbMaxW) g_tbMaxW = padWidth;
      if ((UInt32)padHeight > g_tbMaxH) g_tbMaxH = padHeight;
      if (ext > g_tbMaxExtent) g_tbMaxExtent = ext;
   }
#endif

   // And dump.
   GLenum srcFmt = (g_prefOGLAlwaysRGBA ? GL_RGBA : GL_RGB);
   GLenum dstFmt;
   if (g_prefOGLAlwaysRGBA)
      if (g_prefOGLUse32BitTex)
         dstFmt = GL_RGBA8;
      else
         dstFmt = GL_RGBA4;
   else
      if (g_prefOGLUse32BitTex)
         dstFmt = GL_RGB8;
      else
         dstFmt = GL_RGB;

   // Download the miplevels, translating as we go...
   //
   for (int i = 0; i < in_pBitmap->detailLevels; i++) {
      int padMipWidth  = (padWidth  >> i) == 0               ? 1 : (padWidth  >> i);
      int padMipHeight = (padHeight >> i) == 0               ? 1 : (padHeight >> i);
      int mipWidth     = (in_pBitmap->getStride() >> i) == 0 ? 1 : (in_pBitmap->getStride() >> i);
      int mipHeight    = (in_pBitmap->getHeight() >> i) == 0 ? 1 : (in_pBitmap->getHeight() >> i);

      // Translate
      int pixelMul = g_prefOGLAlwaysRGBA ? 4 : 3;

      for (Int32 y = 0; y < mipHeight; y++) {
         UInt8* pDest      = m_pTranslationBuffer + (y * padMipWidth * pixelMul);
         const UInt8* pSrc = in_pBitmap->pMipBits[i] + (y * mipWidth);

         for (Int32 x = 0; x < mipWidth; x++) {
            const UInt8* pSrcPal = &pMPCache->data[pSrc[x] * 4];

            *(pDest++) = *(pSrcPal++);
            *(pDest++) = *(pSrcPal++);
            *(pDest++) = *(pSrcPal++);

            if (g_prefOGLAlwaysRGBA == true)
               *(pDest++) = 255;
         }
      }

#ifdef __EMSCRIPTEN__
      // WASM-PORT: clear errors so g_texLastErr (below) reflects only this base-texture upload.
      while (glGetError() != GL_NO_ERROR) {}
#endif
      glTexImage2D(GL_TEXTURE_2D,
                   i,
                   dstFmt,
                   padMipWidth, padMipHeight,
                   0,
                   srcFmt,
                   GL_UNSIGNED_BYTE,
                   m_pTranslationBuffer);
#ifdef __EMSCRIPTEN__
      g_texLastErr = (UInt32)glGetError();
#endif
   }

   if (supportsEdgeClamp() == false) {
#ifdef __EMSCRIPTEN__
      // GL_CLAMP does not exist in WebGL/ES (INVALID_ENUM). Base textures are pow2-padded so REPEAT is
      // legal, but use CLAMP_TO_EDGE when clamping is requested rather than silently leaving REPEAT.
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, in_clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, in_clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT);
#else
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, in_clamp ? GL_CLAMP : GL_REPEAT);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, in_clamp ? GL_CLAMP : GL_REPEAT);
#endif
   } else {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, in_clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, in_clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT);
   }
}

void
Surface::TextureCache::dumpOGLTextureNormalDyn(const GFXBitmap* in_pBitmap,
                                               const bool       in_clamp)
{
   g_texDownloadThisFrame++;
   AssertFatal(in_pBitmap != NULL, "No bitmap to dump");
   AssertFatal(m_pPalette != NULL, "No palette set to cache");
   AssertFatal(m_pTranslationBuffer != NULL, "No translation buffer!");

   MPCacher* pMPCache = getMPCache(in_pBitmap->paletteIndex);

#ifdef __EMSCRIPTEN__
   {  // WASM-PORT diag [TEXD] (callback/cbTex path) -> persistent globals.
      const UInt8* p0 = in_pBitmap->pMipBits[0];
      int n = in_pBitmap->getWidth() * in_pBitmap->getHeight();
      UInt8 i0 = p0 ? p0[0] : 0; bool uni = true;
      for (int k = 1; k < n && k < 512 && p0; k++) if (p0[k] != i0) { uni = false; break; }
      const UInt8* pal = &pMPCache->data[i0 * 4];
      g_texdReq = in_pBitmap->paletteIndex; g_texdGot = pMPCache->paletteIndex;
      g_texdRGB = (pal[0] << 16) | (pal[1] << 8) | pal[2]; g_texdUni = uni ? 1 : 0;
      if (g_texdReq != g_texdGot) g_texPalMiss++;
   }
#endif

   // We have to make sure that the widths are pow2s.
   int padWidth  = in_pBitmap->getStride();
   int padHeight = in_pBitmap->getHeight();

#ifdef __EMSCRIPTEN__
   {  // WASM-PORT diag [TEXCORRUPT]: largest write into the translation buffer from the cbTex path.
      UInt32 ext = (UInt32)padWidth * (UInt32)padHeight * 4;
      if ((UInt32)padWidth  > g_tbMaxW) g_tbMaxW = padWidth;
      if ((UInt32)padHeight > g_tbMaxH) g_tbMaxH = padHeight;
      if (ext > g_tbMaxExtent) g_tbMaxExtent = ext;
   }
#endif

#ifdef __EMSCRIPTEN__
   // WASM-PORT: callback/animated interior textures (cbTex) take this dynamic-pool path. Like the
   // lightmap path (see dumpOGLLightmap), the original glTexSubImage2D into the constructor's
   // SIZED-internalformat (GL_RGB5_A1) pool texture throws GL_INVALID_OPERATION under WebGL and no-ops
   // -> the wall/animated texture never uploads and renders flat gray. Fix identically: build RGBA and
   // define the level with a full glTexImage2D (unsized GL_RGBA, valid WebGL1/2), align 1, clamp-to-edge.
   {
      for (Int32 y = 0; y < in_pBitmap->getHeight(); y++) {
         UInt8* pDest      = m_pTranslationBuffer + (y * padWidth * 4);
         const UInt8* pSrc = in_pBitmap->pMipBits[0] + (y * in_pBitmap->getStride());
         for (Int32 x = 0; x < in_pBitmap->getWidth(); x++) {
            const UInt8* pSrcPal = &pMPCache->data[pSrc[x] * 4];
            *(pDest++) = pSrcPal[0]; *(pDest++) = pSrcPal[1]; *(pDest++) = pSrcPal[2]; *(pDest++) = 0xff;
         }
      }
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      while (glGetError() != GL_NO_ERROR) {}
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, padWidth, padHeight, 0,
                   GL_RGBA, GL_UNSIGNED_BYTE, m_pTranslationBuffer);
      g_texLastErr = (UInt32)glGetError();
      glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, in_clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, in_clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT);
      return;
   }
#endif

   // And dump.
   GLenum srcFmt = (g_prefOGLAlwaysRGBA ? GL_RGBA : GL_RGB);

   // Download the miplevels, translating as we go...
   //
   int pixelMul = g_prefOGLAlwaysRGBA ? 4 : 3;

   for (Int32 y = 0; y < in_pBitmap->getHeight(); y++) {
      UInt8* pDest      = m_pTranslationBuffer + (y * padWidth * pixelMul);
      const UInt8* pSrc = in_pBitmap->pMipBits[0] + (y * in_pBitmap->getStride());

      for (Int32 x = 0; x < in_pBitmap->getWidth(); x++) {
         const UInt8* pSrcPal = &pMPCache->data[pSrc[x] * 4];

         *(pDest++) = *(pSrcPal++);
         *(pDest++) = *(pSrcPal++);
         *(pDest++) = *(pSrcPal++);

         if (g_prefOGLAlwaysRGBA == true)
            *(pDest++) = 255;
      }
   }

   glTexSubImage2D(GL_TEXTURE_2D,
                   0,
                   0, 0,
                   padWidth, padHeight,
                   srcFmt,
                   GL_UNSIGNED_BYTE,
                   m_pTranslationBuffer);

   if (supportsEdgeClamp() == false) {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, in_clamp ? GL_CLAMP : GL_REPEAT);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, in_clamp ? GL_CLAMP : GL_REPEAT);
   } else {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, in_clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, in_clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT);
   }
}

void
Surface::TextureCache::dumpOGLTextureAlpha(const GFXBitmap* in_pBitmap,
                                           const bool       in_clamp)
{
   g_texDownloadThisFrame++;
   AssertFatal(in_pBitmap != NULL, "No bitmap to dump");
   AssertFatal(m_pPalette != NULL, "No palette set to cache");
   AssertFatal(m_pTranslationBuffer != NULL, "No translation buffer!");

   MPCacher* pMPCache = getMPCache(in_pBitmap->paletteIndex);

   // We have to make sure that the widths are pow2s.
   int padWidth  = getNextPow2(in_pBitmap->getStride());
   int padHeight = getNextPow2(in_pBitmap->getHeight());

   // Download the miplevels, translating as we go...
   //
   for (int i = 0; i < in_pBitmap->detailLevels; i++) {
      int padMipWidth  = (padWidth  >> i) == 0               ? 1 : (padWidth  >> i);
      int padMipHeight = (padHeight >> i) == 0               ? 1 : (padHeight >> i);
      int mipWidth     = (in_pBitmap->getStride() >> i) == 0 ? 1 : (in_pBitmap->getStride() >> i);
      int mipHeight    = (in_pBitmap->getHeight() >> i) == 0 ? 1 : (in_pBitmap->getHeight() >> i);

      // Translate
      for (Int32 y = 0; y < mipHeight; y++) {
         UInt8* pDest      = m_pTranslationBuffer + (y * padMipWidth * 4);
         const UInt8* pSrc = in_pBitmap->pMipBits[i] + (y * mipWidth);

         for (Int32 x = 0; x < mipWidth; x++) {
            const UInt8* pSrcPal = &pMPCache->data[pSrc[x] * 4];

            *(pDest++) = *(pSrcPal++);
            *(pDest++) = *(pSrcPal++);
            *(pDest++) = *(pSrcPal++);
            *(pDest++) = *(pSrcPal++);
         }
      }

      // And dump.
      glTexImage2D(GL_TEXTURE_2D,
                   i,
                   (g_prefOGLUse32BitTex ? GL_RGBA8 : GL_RGBA),
                   padMipWidth, padMipHeight,
                   0,
                   GL_RGBA,
                   GL_UNSIGNED_BYTE,
                   m_pTranslationBuffer);
   }

   if (supportsEdgeClamp() == false) {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, in_clamp ? GL_CLAMP : GL_REPEAT);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, in_clamp ? GL_CLAMP : GL_REPEAT);
   } else {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, in_clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, in_clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT);
   }
}

void
Surface::TextureCache::dumpOGLTextureTransparent(const GFXBitmap* in_pBitmap,
                                                 const bool       in_clamp)
{
   g_texDownloadThisFrame++;
   AssertFatal(in_pBitmap != NULL, "No bitmap to dump");
   AssertFatal(m_pPalette != NULL, "No palette set to cache");
   AssertFatal(m_pTranslationBuffer != NULL, "No translation buffer!");

   MPCacher* pMPCache = getMPCache(in_pBitmap->paletteIndex);

   // We have to make sure that the widths are pow2s.
   int padWidth  = getNextPow2(in_pBitmap->getStride());
   int padHeight = getNextPow2(in_pBitmap->getHeight());

   // Download the miplevels, translating as we go...
   //
   for (int i = 0; i < in_pBitmap->detailLevels; i++) {
      int padMipWidth  = (padWidth  >> i) == 0               ? 1 : (padWidth  >> i);
      int padMipHeight = (padHeight >> i) == 0               ? 1 : (padHeight >> i);
      int mipWidth     = (in_pBitmap->getStride() >> i) == 0 ? 1 : (in_pBitmap->getStride() >> i);
      int mipHeight    = (in_pBitmap->getHeight() >> i) == 0 ? 1 : (in_pBitmap->getHeight() >> i);

      // Translate
      for (Int32 y = 0; y < mipHeight; y++) {
         UInt8* pDest      = m_pTranslationBuffer + (y * padMipWidth * 4);
         const UInt8* pSrc = in_pBitmap->pMipBits[i] + (y * mipWidth);

         for (Int32 x = 0; x < mipWidth; x++) {
            const UInt8* pSrcPal = &pMPCache->dataTransparent[pSrc[x] * 4];

            *(pDest++) = *(pSrcPal++);
            *(pDest++) = *(pSrcPal++);
            *(pDest++) = *(pSrcPal++);
            *(pDest++) = *(pSrcPal++);
         }
      }

      // And dump.
      glTexImage2D(GL_TEXTURE_2D,
                   i,
                   (g_prefOGLUse32BitTex ? GL_RGBA8 : GL_RGBA),
                   padMipWidth, padMipHeight,
                   0,
                   GL_RGBA,
                   GL_UNSIGNED_BYTE,
                   m_pTranslationBuffer);
   }

   if (supportsEdgeClamp() == false) {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, in_clamp ? GL_CLAMP : GL_REPEAT);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, in_clamp ? GL_CLAMP : GL_REPEAT);
   } else {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, in_clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, in_clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT);
   }
}

void
Surface::TextureCache::dumpOGLLightmap(GFXLightMap* io_pLM)
{
   g_lmDownloadThisFrame++;
   AssertFatal(io_pLM != NULL, "No lightmap to dump");
   AssertFatal(m_pTranslationBuffer != NULL, "No translation buffer!");

   // Translate
   // And dump.
#ifdef __EMSCRIPTEN__
   // WASM-PORT: LIVE lightmap upload path under wasm (supportsPackedPixels()==false -- no
   // GL_EXT_packed_pixels in the WebGL ext string, so cacheLightmap routes here, NOT to the packed
   // variant). Two ES/WebGL-specific bugs made interior lightmaps sample as nothing (black multiply)
   // while base textures rendered fine:
   //  (A) FORMAT MISMATCH: the dynamic-cache pool texture is DEFINED in the ctor as
   //      glTexImage2D(GL_RGB5_A1 internalformat, format=GL_RGBA, type=GL_UNSIGNED_BYTE). The desktop
   //      code SubImage'd it here as GL_RGB (3 components) -- under ES the SubImage format must agree
   //      with the level's format, so GL_RGB vs GL_RGBA throws INVALID_OPERATION and the upload no-ops
   //      (the lightmap keeps its uninitialized/dummy content). Upload GL_RGBA to match the ctor EXACTLY
   //      -- and keep glTexSubImage2D (NOT glTexImage2D) so we do not redefine the shared pool texture's
   //      size/format (the same pool entries are reused for callback/terrain textures via
   //      chooseDynamicName -- redefining storage would break those).
   //  (B) GL_CLAMP wrap: supportsEdgeClamp() is false under WebGL, so the desktop path set wrap=GL_CLAMP,
   //      an enum that does NOT exist in WebGL/ES (INVALID_ENUM). Use GL_CLAMP_TO_EDGE.
   //  (alignment): RGBA is always 4-byte aligned, but set UNPACK_ALIGNMENT=1 defensively for the sub-rect.
   {
      UInt32 realSizeX = (io_pLM->size.x ? io_pLM->size.x : 256);
      UInt32 realSizeY = (io_pLM->size.y ? io_pLM->size.y : 256);
      UInt32 maxLum = 0;

      for (UInt32 y = 0; y < realSizeY; y++) {
         UInt8* pDest       = m_pTranslationBuffer + (y * realSizeX * 4);
         const UInt16* pSrc = &(io_pLM->data) + (y * realSizeX);
         for (UInt32 x = 0; x < realSizeX; x++) {
            UInt16 color = pSrc[x];
            UInt8 r = (((color >> 4) & 0xf0) | 0xf) & 0xff;
            UInt8 g = (((color >> 0) & 0xf0) | 0xf) & 0xff;
            UInt8 b = (((color << 4) & 0xf0) | 0xf) & 0xff;
            *(pDest++) = r; *(pDest++) = g; *(pDest++) = b; *(pDest++) = 0xff;
            UInt32 lum = (UInt32)r + g + b;
            if (lum > maxLum) maxLum = lum;
         }
      }

      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      while (glGetError() != GL_NO_ERROR) {}
      // Full glTexImage2D with an UNSIZED GL_RGBA internalformat (valid in WebGL1 & WebGL2). The
      // constructor pre-allocated this pool texture with a SIZED internalformat (GL_RGB5_A1); a
      // glTexSubImage2D into it threw GL_INVALID_OPERATION (0x0502) under this GL context, no-opping
      // the upload (-> black lightmap -> black interior). Redefining the level here mirrors how the
      // non-packed callback path effectively works and removes any dependence on the ctor allocation.
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, realSizeX, realSizeY, 0,
                   GL_RGBA, GL_UNSIGNED_BYTE, m_pTranslationBuffer);
      GLenum e = glGetError();
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
      g_lmDumpTotal++;
      g_lmLastErr = (UInt32)e;
      if (maxLum > g_lmMaxLum) g_lmMaxLum = maxLum;
   }
   return;
#endif

   int pixelMul = g_prefOGLAlwaysRGBA ? 4 : 3;

   GLenum srcFmt    = (g_prefOGLAlwaysRGBA ? GL_RGBA : GL_RGB);
   UInt32 realSizeX = (io_pLM->size.x ? io_pLM->size.x : 256);
   UInt32 realSizeY = (io_pLM->size.y ? io_pLM->size.y : 256);

   for (UInt32 y = 0; y < realSizeY; y++) {
      UInt8* pDest       = m_pTranslationBuffer + (y * realSizeX * pixelMul);
      const UInt16* pSrc = &(io_pLM->data) + (y * realSizeX);

      for (Int32 x = 0; x < (io_pLM->size.x ? io_pLM->size.x : 256); x++) {
         UInt16 color = pSrc[x];

         *(pDest++) = (((color >> 4) & 0xf0) | 0xf) & 0xff;
         *(pDest++) = (((color >> 0) & 0xf0) | 0xf) & 0xff;
         *(pDest++) = (((color << 4) & 0xf0) | 0xf) & 0xff;

         if (g_prefOGLAlwaysRGBA)
            *(pDest++) = 0xff;
      }
   }

   // And dump.
   glTexSubImage2D(GL_TEXTURE_2D,
                   0,
                   0, 0,
                   realSizeX, realSizeY,
                   srcFmt,
                   GL_UNSIGNED_BYTE,
                   m_pTranslationBuffer);

   if (supportsEdgeClamp() == false) {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
   } else {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   }
}

void
Surface::TextureCache::refreshOGLTextureNormal(const GFXBitmap* in_pBitmap,
                                               const bool       in_clamp)
{
   g_texDownloadThisFrame++;
   AssertFatal(in_pBitmap != NULL, "No bitmap to dump");
   AssertFatal(m_pPalette != NULL, "No palette set to cache");
   AssertFatal(m_pTranslationBuffer != NULL, "No translation buffer!");

   MPCacher* pMPCache = getMPCache(in_pBitmap->paletteIndex);

   // We have to make sure that the widths are pow2s.
   int padWidth  = getNextPow2(in_pBitmap->getStride());
   int padHeight = getNextPow2(in_pBitmap->getHeight());

   // And dump.
   GLenum srcFmt = (g_prefOGLAlwaysRGBA ? GL_RGBA : GL_RGB);

   // Download the miplevels, translating as we go...
   //
   for (int i = 0; i < in_pBitmap->detailLevels; i++) {
      int padMipWidth  = (padWidth  >> i) == 0               ? 1 : (padWidth  >> i);
      int padMipHeight = (padHeight >> i) == 0               ? 1 : (padHeight >> i);
      int mipWidth     = (in_pBitmap->getStride() >> i) == 0 ? 1 : (in_pBitmap->getStride() >> i);
      int mipHeight    = (in_pBitmap->getHeight() >> i) == 0 ? 1 : (in_pBitmap->getHeight() >> i);

      // Translate
      int pixelMul = g_prefOGLAlwaysRGBA ? 4 : 3;

      for (Int32 y = 0; y < mipHeight; y++) {
         UInt8* pDest      = m_pTranslationBuffer + (y * padMipWidth * pixelMul);
         const UInt8* pSrc = in_pBitmap->pMipBits[i] + (y * mipWidth);

         for (Int32 x = 0; x < mipWidth; x++) {
            const UInt8* pSrcPal = &pMPCache->data[pSrc[x] * 4];

            *(pDest++) = *(pSrcPal++);
            *(pDest++) = *(pSrcPal++);
            *(pDest++) = *(pSrcPal++);

            if (g_prefOGLAlwaysRGBA == true)
               *(pDest++) = 255;
         }
      }

      glTexSubImage2D(GL_TEXTURE_2D,
                      i,
                      0, 0,
                      padMipWidth, padMipHeight,
                      srcFmt,
                      GL_UNSIGNED_BYTE,
                      m_pTranslationBuffer);
   }

   if (supportsEdgeClamp() == false) {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, in_clamp ? GL_CLAMP : GL_REPEAT);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, in_clamp ? GL_CLAMP : GL_REPEAT);
   } else {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, in_clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, in_clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT);
   }
}

void
Surface::TextureCache::refreshOGLTextureAlpha(const GFXBitmap* in_pBitmap,
                                              const bool       in_clamp)
{
   g_texDownloadThisFrame++;
   AssertFatal(in_pBitmap != NULL, "No bitmap to dump");
   AssertFatal(m_pPalette != NULL, "No palette set to cache");
   AssertFatal(m_pTranslationBuffer != NULL, "No translation buffer!");

   MPCacher* pMPCache = getMPCache(in_pBitmap->paletteIndex);

   // We have to make sure that the widths are pow2s.
   int padWidth  = getNextPow2(in_pBitmap->getStride());
   int padHeight = getNextPow2(in_pBitmap->getHeight());

   // Download the miplevels, translating as we go...
   //
   for (int i = 0; i < in_pBitmap->detailLevels; i++) {
      int padMipWidth  = (padWidth  >> i) == 0               ? 1 : (padWidth  >> i);
      int padMipHeight = (padHeight >> i) == 0               ? 1 : (padHeight >> i);
      int mipWidth     = (in_pBitmap->getStride() >> i) == 0 ? 1 : (in_pBitmap->getStride() >> i);
      int mipHeight    = (in_pBitmap->getHeight() >> i) == 0 ? 1 : (in_pBitmap->getHeight() >> i);

      // Translate
      for (Int32 y = 0; y < mipHeight; y++) {
         UInt8* pDest      = m_pTranslationBuffer + (y * padMipWidth * 4);
         const UInt8* pSrc = in_pBitmap->pMipBits[i] + (y * mipWidth);

         for (Int32 x = 0; x < mipWidth; x++) {
            const UInt8* pSrcPal = &pMPCache->data[pSrc[x] * 4];

            *(pDest++) = *(pSrcPal++);
            *(pDest++) = *(pSrcPal++);
            *(pDest++) = *(pSrcPal++);
            *(pDest++) = *(pSrcPal++);
         }
      }

      // And dump.
      glTexSubImage2D(GL_TEXTURE_2D,
                      i,
                      0, 0,
                      padMipWidth, padMipHeight,
                      GL_RGBA,
                      GL_UNSIGNED_BYTE,
                      m_pTranslationBuffer);
   }

   if (supportsEdgeClamp() == false) {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, in_clamp ? GL_CLAMP : GL_REPEAT);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, in_clamp ? GL_CLAMP : GL_REPEAT);
   } else {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, in_clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, in_clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT);
   }
}

void
Surface::TextureCache::refreshOGLTextureTransparent(const GFXBitmap* in_pBitmap,
                                                    const bool       in_clamp)
{
   g_texDownloadThisFrame++;
   AssertFatal(in_pBitmap != NULL, "No bitmap to dump");
   AssertFatal(m_pPalette != NULL, "No palette set to cache");
   AssertFatal(m_pTranslationBuffer != NULL, "No translation buffer!");

   MPCacher* pMPCache = getMPCache(in_pBitmap->paletteIndex);

   // We have to make sure that the widths are pow2s.
   int padWidth  = getNextPow2(in_pBitmap->getStride());
   int padHeight = getNextPow2(in_pBitmap->getHeight());

   // Download the miplevels, translating as we go...
   //
   for (int i = 0; i < in_pBitmap->detailLevels; i++) {
      int padMipWidth  = (padWidth  >> i) == 0               ? 1 : (padWidth  >> i);
      int padMipHeight = (padHeight >> i) == 0               ? 1 : (padHeight >> i);
      int mipWidth     = (in_pBitmap->getStride() >> i) == 0 ? 1 : (in_pBitmap->getStride() >> i);
      int mipHeight    = (in_pBitmap->getHeight() >> i) == 0 ? 1 : (in_pBitmap->getHeight() >> i);

      // Translate
      for (Int32 y = 0; y < mipHeight; y++) {
         UInt8* pDest      = m_pTranslationBuffer + (y * padMipWidth * 4);
         const UInt8* pSrc = in_pBitmap->pMipBits[i] + (y * mipWidth);

         for (Int32 x = 0; x < mipWidth; x++) {
            const UInt8* pSrcPal = &pMPCache->dataTransparent[pSrc[x] * 4];

            *(pDest++) = *(pSrcPal++);
            *(pDest++) = *(pSrcPal++);
            *(pDest++) = *(pSrcPal++);
            *(pDest++) = *(pSrcPal++);
         }
      }

      // And dump.
      glTexSubImage2D(GL_TEXTURE_2D,
                      i,
                      0, 0,
                      padMipWidth, padMipHeight,
                      GL_RGBA,
                      GL_UNSIGNED_BYTE,
                      m_pTranslationBuffer);
   }

   if (supportsEdgeClamp() == false) {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, in_clamp ? GL_CLAMP : GL_REPEAT);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, in_clamp ? GL_CLAMP : GL_REPEAT);
   } else {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, in_clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, in_clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT);
   }
}


//------------------------------------------------------------------------------
//-------------------------------------- Packed Pixels ext.
//
void
Surface::TextureCache::dumpOGLTexturePacked(const GFXBitmap* in_pBitmap,
                                            bool             transparent,
                                            const bool       in_clamp)
{
   g_texDownloadThisFrame++;
   AssertFatal(in_pBitmap != NULL, "No bitmap to dump");
   AssertFatal(m_pPalette != NULL, "No palette set to cache");
   AssertFatal(m_pTranslationBuffer != NULL, "No translation buffer!");

   // We have to make sure that the widths are pow2s.
   int padWidth  = getNextPow2(in_pBitmap->getStride());
   int padHeight = getNextPow2(in_pBitmap->getHeight());

   MPCacher* pMPCache = getMPCache(in_pBitmap->paletteIndex);

   // And dump.
   GLenum srcFmt = transparent ? pMPCache->packedTransparentType : pMPCache->packedType;

   GLenum dstFmt;
   if (srcFmt == GL_UNSIGNED_SHORT_5_5_5_1)
      dstFmt = GL_RGB5_A1;
   else
      dstFmt = GL_RGBA4;

   // Download the miplevels, translating as we go...
   //
   for (int i = 0; i < in_pBitmap->detailLevels; i++) {
      int padMipWidth  = (padWidth  >> i) == 0               ? 1 : (padWidth  >> i);
      int padMipHeight = (padHeight >> i) == 0               ? 1 : (padHeight >> i);
      int mipWidth     = (in_pBitmap->getStride() >> i) == 0 ? 1 : (in_pBitmap->getStride() >> i);
      int mipHeight    = (in_pBitmap->getHeight() >> i) == 0 ? 1 : (in_pBitmap->getHeight() >> i);

      // Translate
      int pixelMul = 2;

      UInt16* pTable = transparent ? pMPCache->dataPackedTransparent : pMPCache->dataPacked;

      for (Int32 y = 0; y < mipHeight; y++) {
         UInt16* pDest     = (UInt16*)(m_pTranslationBuffer + (y * padMipWidth * pixelMul));
         const UInt8* pSrc = in_pBitmap->pMipBits[i] + (y * mipWidth);

         for (Int32 x = 0; x < mipWidth; x++) {
            *(pDest++) = pTable[pSrc[x]];
         }
      }

      glTexImage2D(GL_TEXTURE_2D,
                   i,
                   dstFmt,
                   padMipWidth, padMipHeight,
                   0,
                   GL_RGBA,
                   srcFmt,
                   m_pTranslationBuffer);
      g_totalNumBytes += padMipWidth * padMipHeight * 2;
   }

   if (supportsEdgeClamp() == false) {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, in_clamp ? GL_CLAMP : GL_REPEAT);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, in_clamp ? GL_CLAMP : GL_REPEAT);
   } else {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, in_clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, in_clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT);
   }
}

void
Surface::TextureCache::dumpOGLTexturePackedDyn(const GFXBitmap* in_pBitmap,
                                               bool             transparent,
                                               const bool       in_clamp)
{
   g_texDownloadThisFrame++;
   AssertFatal(in_pBitmap != NULL, "No bitmap to dump");
   AssertFatal(m_pPalette != NULL, "No palette set to cache");
   AssertFatal(m_pTranslationBuffer != NULL, "No translation buffer!");

   // We have to make sure that the widths are pow2s.
   int padWidth  = in_pBitmap->getWidth();
   int padHeight = in_pBitmap->getHeight();

   MPCacher* pMPCache = getMPCache(in_pBitmap->paletteIndex);

   // And dump.
   GLenum srcFmt = transparent ? pMPCache->packedTransparentType : pMPCache->packedType;

   // Download the miplevels, translating as we go...
   // Translate
   //
   int pixelMul   = 2;
   UInt16* pTable = pMPCache->dataPacked;

   for (Int32 y = 0; y < in_pBitmap->getHeight(); y++) {
      UInt16* pDest     = (UInt16*)(m_pTranslationBuffer + (y * padWidth * pixelMul));
      const UInt8* pSrc = in_pBitmap->pMipBits[0] + (y * in_pBitmap->getStride());

      for (Int32 x = 0; x < padWidth; x++) {
         *(pDest++) = pTable[pSrc[x]];
      }
   }

   glTexSubImage2D(GL_TEXTURE_2D,
                   0,
                   0, 0,
                   padWidth, padHeight,
                   GL_RGBA,
                   srcFmt,
                   m_pTranslationBuffer);

   if (supportsEdgeClamp() == false) {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, in_clamp ? GL_CLAMP : GL_REPEAT);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, in_clamp ? GL_CLAMP : GL_REPEAT);
   } else {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, in_clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, in_clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT);
   }
}

void
Surface::TextureCache::refreshOGLTexturePacked(const GFXBitmap* in_pBitmap,
                                               bool             transparent,
                                               const bool       in_clamp)
{
   g_texDownloadThisFrame++;
   AssertFatal(in_pBitmap != NULL, "No bitmap to dump");
   AssertFatal(m_pPalette != NULL, "No palette set to cache");
   AssertFatal(m_pTranslationBuffer != NULL, "No translation buffer!");

   MPCacher* pMPCache = getMPCache(in_pBitmap->paletteIndex);

   // We have to make sure that the widths are pow2s.
   int padWidth  = getNextPow2(in_pBitmap->getStride());
   int padHeight = getNextPow2(in_pBitmap->getHeight());

   // And dump.
   GLenum srcFmt = transparent ? pMPCache->packedTransparentType : pMPCache->packedType;
   int pixelMul  = 2;
   UInt16* pTable = transparent ? pMPCache->dataPackedTransparent : pMPCache->dataPacked;

   // Download the miplevels, translating as we go...
   //
   for (int i = 0; i < in_pBitmap->detailLevels; i++) {
      int padMipWidth  = (padWidth  >> i) == 0               ? 1 : (padWidth  >> i);
      int padMipHeight = (padHeight >> i) == 0               ? 1 : (padHeight >> i);
      int mipWidth     = (in_pBitmap->getStride() >> i) == 0 ? 1 : (in_pBitmap->getStride() >> i);
      int mipHeight    = (in_pBitmap->getHeight() >> i) == 0 ? 1 : (in_pBitmap->getHeight() >> i);

      // Translate
      for (Int32 y = 0; y < mipHeight; y++) {
         UInt16* pDest     = (UInt16*)(m_pTranslationBuffer + (y * padMipWidth * pixelMul));
         const UInt8* pSrc = in_pBitmap->pMipBits[0] + (y * mipWidth);

         for (Int32 x = 0; x < mipWidth; x++) {
            *(pDest++) = pTable[pSrc[x]];
         }
      }

      // And dump.
      glTexSubImage2D(GL_TEXTURE_2D,
                      i,
                      0, 0,
                      padMipWidth, padMipHeight,
                      GL_RGBA,
                      srcFmt,
                      m_pTranslationBuffer);
   }
   if (supportsEdgeClamp() == false) {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, in_clamp ? GL_CLAMP : GL_REPEAT);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, in_clamp ? GL_CLAMP : GL_REPEAT);
   } else {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, in_clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, in_clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT);
   }
}

void
Surface::TextureCache::dumpOGLLightmapPacked(GFXLightMap* io_pLM)
{
   g_lmDownloadThisFrame++;
   AssertFatal(io_pLM != NULL, "No lightmap to dump");
   AssertFatal(m_pTranslationBuffer != NULL, "No translation buffer!");

   // We have to make sure that the widths are pow2s.
   int realWidth  = io_pLM->size.x ? io_pLM->size.x : 256;
   int realHeight = io_pLM->size.y ? io_pLM->size.y : 256;

//   int padDim = max(getNextPow2(max(realWidth, realHeight)), 8);

   // Translate
   // And dump.
   int pixelMul = 2;

   for (Int32 y = 0; y < realHeight; y++) {
      UInt16* pDest      = (UInt16*)(m_pTranslationBuffer + (y * realWidth * pixelMul));
      const UInt16* pSrc = &(io_pLM->data) + (y * realWidth);
//      UInt16* pDest      = (UInt16*)(m_pTranslationBuffer + (y * padDim * pixelMul));
//      const UInt16* pSrc = &(io_pLM->data) + (y * realWidth);

      for (Int32 x = 0; x < realWidth; x++) {
         UInt16 color = pSrc[x];
         UInt16 newColor = ((color & 0x0f00) << 4) | 0x0800;
         newColor       |= ((color & 0x00f0) << 3) | 0x0040;
         newColor       |= ((color & 0x000f) << 2) | 0x0003;

         *(pDest++) = newColor;
      }
   }

//   GLenum dstFmt;
//   if (g_prefOGLUse32BitTex)
//      dstFmt = GL_RGBA8;
//   else
//      dstFmt = GL_RGB5;
//
//   // And dump.
//   glTexImage2D(GL_TEXTURE_2D,
//                0,
//                dstFmt,
//                padDim, padDim,
//                0,
//                GL_RGBA,
//                GL_UNSIGNED_SHORT_4_4_4_4,
//                m_pTranslationBuffer);
   glTexSubImage2D(GL_TEXTURE_2D,
                   0,
                   0, 0,
                   realWidth, realHeight,
                   GL_RGBA,
                   GL_UNSIGNED_SHORT_5_5_5_1,
                   m_pTranslationBuffer);

   if (supportsEdgeClamp() == false) {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
   } else {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   }

   g_lmDownloadBytes += (realWidth * realHeight * 2);
//   g_lmDownloadBytes += (padDim * padDim * 2);
}

} // namespace OpenGL