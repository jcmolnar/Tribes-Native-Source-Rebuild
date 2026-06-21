//----------------------------------------------------------------------------


//----------------------------------------------------------------------------


//----------------------------------------------------------------------------

#include <g_surfac.h>
#include <g_bitmap.h>
#include <ts.h>
#include "itrmetrics.h"
#include "itrgeometry.h"
#include "itrlighting.h"
#include "itrinstance.h"
#include "itrrender.h"
#include "gOGLSfc.h"
#include "gOGLTx.h"
#include "m_base.h"
#ifdef __EMSCRIPTEN__
#include <stdio.h>    // WASM-PORT diag [ITRMAT]
#include <string.h>   // WASM-PORT diag [ITRMAT] memset
#include <math.h>     // WASM-PORT cull: sqrtf for distance-to-box
#include <emscripten.h> // WASM-PORT perf: emscripten_get_now for phase timers
// WASM-PORT: master switch for the verbose per-frame render diagnostics ([ITR], [ITRMAT], [PATHS],
// [ITRMAT-SUM], ...). Default OFF — these flood the console (and the [ITR] line runs a per-interior
// surface-count loop every frame). Flip on at runtime via Module._wasmSetDiag(1) when debugging.
int g_wasmDiag = 0;

// WASM-PORT city-view perf cull. With ~32 streamed interiors in the frustum, each running a full
// BSP walk + two OGL surface passes every frame, looking AT a city costs ~92ms/frame (11fps) vs
// 100+ looking away. These gates short-circuit ITRRender::render BEFORE the BSP walk:
//  - g_wasmInteriorFrustumCull (default ON): test the whole interior's bounding sphere against the
//    camera frustum (same SphereF/testVisibility the per-leaf PVS already trusts) and skip if wholly
//    outside the view cone. Always correct — only ever skips interiors that emit nothing anyway.
//  - g_wasmInteriorCullDist (default 0 = OFF): a hard draw-distance LOD. If >0, skip any interior
//    whose bounding box is farther than this many world units from the camera. Tunable live via
//    Module._wasmSetInteriorCull(dist) so the heavy-view render ms can be measured against it.
// Counters are zeroed per-frame by the caller and surfaced on the [PROF] line (no eval needed).
int g_wasmInteriorFrustumCull = 1;
int g_wasmInteriorCullDist    = 0;
int g_wasmItrRendered      = 0;
int g_wasmItrCulledFrustum = 0;
int g_wasmItrCulledDist    = 0;

// WASM-PORT perf probe: split the per-frame interior render cost between "rebuild churn" and "raw
// geometry volume". g_wasmSurfMiss = surfaces whose GL handle missed the cache this frame (the
// else/registerTexture path => buildLightMap + full RGBA texture re-upload, the expensive case). A
// high miss count across frames == the 2048-entry HandleCache is thrashing on the city's surfaces, so
// every surface rebuilds+re-uploads every frame (a FIXABLE bug). Near-zero miss == the cost is just
// the sheer poly throughput (g_wasmSurfDraw polys / g_wasmSurfVerts verts), which only LOD/cull cuts.
int g_wasmSurfMiss  = 0;
int g_wasmSurfDraw  = 0;
int g_wasmSurfVerts = 0;

// WASM-PORT animated-lightmap throttle. Interior surfaces lit by an animated light (flickering
// torches/fires) are invalidated EVERY frame (installLightState -> setLightMapNotValid), so every
// visible such surface rebuilds its lightmap and does a full-RGBA glTexImage2D re-upload every frame
// -- the dominant city-view cost (~519 rebuilds/frame == ~93ms). g_wasmLmapThrottle caps how often a
// given surface's lightmap is actually rebuilt: 1 = every frame (stock), N = at most once every N
// frames, STAGGERED by surface index so the cost spreads evenly (no periodic hitch). Between rebuilds
// the surface reuses its already-uploaded GL lightmap (the handle stays cached), so it stays lit --
// the flicker animation just updates at framerate/N, which is visually fine. g_wasmLmapBuild counts
// the lightmap rebuilds actually performed this frame (probe + throttle verification).
int g_wasmFrameNo       = 0;
int g_wasmLmapThrottle  = 1;   // (legacy modulo knob, no longer gating)
int g_wasmLmapBuild     = 0;
int g_wasmLmapBudget    = 0;   // max lightmap rebuilds per FRAME (0 = unlimited/stock)
int g_wasmLmapBuiltCount = 0;  // rebuilds done so far THIS frame (reset by caller)

// WASM-PORT perf phase timers (ms, accumulated across all interiors per frame, zeroed by the caller,
// surfaced on [PROF]). A = visibility build (leaf/PVS/planes/surfaces/points BSP walk). B =
// renderSurfacesOGL(false) (startRender light-affection + lightmap/texture setup). C =
// renderSurfacesOGL(true) (vertex transform via pointMap + drawPoly emit). Whichever dominates is the
// real city-view cost now that lightmap rebuilds are shown to be only ~25%.
double g_wasmTphaseS = 0.0;   // set sizing/clearing (surfaceSet/planeSet/pointSet/pointMap, full geom)
double g_wasmTphaseA = 0.0;
double g_wasmTphaseB = 0.0;
double g_wasmTphaseC = 0.0;
#endif

//----------------------------------------------------------------------------
ITRBitVector ITRRender::surfaceSet;
ITRBitVector ITRRender::planeSet;
ITRBitVector ITRRender::pointSet;

UInt32 ITRRender::PrefInteriorTextureDetail = 0;   // 0 = highest -> 8 == lowest

#ifdef __EMSCRIPTEN__
// WASM-PORT diag [ITRMAT]: persistent interior material->texture resolution summary (white walls =
// noTex). One-shot console prints raced the spawn, so accumulate into globals surfaced in [PATHS]:
// distinct materials seen, how many resolve a NULL texture, and the first NULL material's index,
// flags (type: 2=RGB 3=Texture) and map filename. g_itrMatNull>0 with flags=3 => that wall texture
// failed rm.load (asset/VOL gap); flags!=3 => the material legitimately has no texture.
UInt32 g_itrMatTot = 0, g_itrMatNull = 0;
Int32  g_itrFirstNullIdx = -1, g_itrFirstNullFlags = -1;
char   g_itrFirstNullMap[64] = "";
#endif

namespace OpenGL {

void
externCheckCache(OpenGL::Surface* pSurface);

}


namespace {

int getPower(int x)
{
   // Returns 2^n (the highest bit).
   AssertFatal( x >= 0, "itrRender: negative value passed to getPower()" );
   int i = 0;
   if (x)
      do
         i++;
      while (x >>= 1);
   return i;
}

}; // namespace {} 


int ITRRender::getOutsideVisibility(const Point3F& in_cameraPos, ITRInstance* inst)
{
   instance = inst;
   geometry = instance->getGeometry();

   int leafIndex;
   if ((leafIndex = geometry->externalLeaf(in_cameraPos)) == 0)
      leafIndex = findLeaf(in_cameraPos);
   ITRGeometry::BSPLeafWrap leafWrap(geometry, leafIndex);

   int camPosOutsideBits = geometry->getOutsideBits(leafWrap);

   return camPosOutsideBits;
}


void ITRRender::render(TSRenderContext& rc,
                       ITRInstance*     inst)
{
   // Make sure our pref is in range...
   if (PrefInteriorTextureDetail < 0)      PrefInteriorTextureDetail = 0;
   else if (PrefInteriorTextureDetail > 8) PrefInteriorTextureDetail = 8;

   instance = inst;
   geometry = instance->getGeometry();
   lighting = instance->getLighting();
   materialList = instance->getMaterialList();
   renderContext = &rc;
   
   textureScale = 1.0f / geometry->textureScale;

   //TSRenderInterface::getLocked()->setHazeLevel(0);
   renderContext = &rc;
   
   camera = rc.getCamera();
   pointArray = rc.getPointArray();
   gfxSurface = rc.getSurface();

   gfxSurface->setFillMode(GFX_FILL_TWOPASS);
   gfxSurface->setShadeSource(GFX_SHADE_NONE);
   gfxSurface->setAlphaSource(GFX_ALPHA_NONE);
   gfxSurface->setTransparency(false);

   // DMM - State order dependancy fix
   extern void GFXShadeHazeChanged(GFXSurface*);
   GFXShadeHazeChanged(gfxSurface);


   pointArray->reset();
   pointArray->useTextures(geometry->point2List.address());
   pointArray->useIntensities(false);
   pointArray->useTextures(true);
   pointArray->setVisibility( TS::ClipMask );

   // If the interior was built at high detail level (ie. shared vertices inserted)
   //  then we must render it using perspective correction.  If not, try to render
   //  it using trifans, and non-perspective renderers...
   //
   if (geometry->testFlag(ITRGeometry::LowDetailInterior) == false)
      gfxSurface->setTexturePerspective(true);
   else
      gfxSurface->setTexturePerspective(false);

   // Get camera position
   TMat3F tco = camera->getTOC();
   tco.inverse();
   cameraPos = tco.p;

#ifdef __EMSCRIPTEN__
   // WASM-PORT city-view perf cull — short-circuit the whole interior before the BSP walk + the two
   // renderSurfacesOGL passes (the 92ms-at-a-city cost). The camera transform stack already has this
   // interior's transform pushed (RenderImage::render), so geometry->box and the frustum test below
   // are both in interior-local space — exactly like processPVS's per-leaf test.
   {
      const Box3F& gb = geometry->box;

      // Hard draw-distance LOD (opt-in). Nearest distance from the camera to the interior's box.
      if (::g_wasmInteriorCullDist > 0) {
         float dx = cameraPos.x < gb.fMin.x ? gb.fMin.x - cameraPos.x
                  : cameraPos.x > gb.fMax.x ? cameraPos.x - gb.fMax.x : 0.0f;
         float dy = cameraPos.y < gb.fMin.y ? gb.fMin.y - cameraPos.y
                  : cameraPos.y > gb.fMax.y ? cameraPos.y - gb.fMax.y : 0.0f;
         float dz = cameraPos.z < gb.fMin.z ? gb.fMin.z - cameraPos.z
                  : cameraPos.z > gb.fMax.z ? cameraPos.z - gb.fMax.z : 0.0f;
         if (sqrtf(dx*dx + dy*dy + dz*dz) > (float)::g_wasmInteriorCullDist) {
            ::g_wasmItrCulledDist++;
            return;
         }
      }

      // Whole-interior frustum reject (always-on, always correct: an interior wholly outside the
      // view cone emits no surfaces, so skipping its BSP walk changes nothing visible).
      if (::g_wasmInteriorFrustumCull) {
         SphereF sphere;
         Point3F diag;
         sphere.center = diag = gb.fMax;
         diag         -= gb.fMin;
         sphere.center += gb.fMin;
         sphere.center *= 0.5f;
         sphere.radius  = diag.lenf();
         if (camera->testVisibility(sphere) == TS::ClipNoneVis) {
            ::g_wasmItrCulledFrustum++;
            return;
         }
      }
      ::g_wasmItrRendered++;
   }
#endif

   //
   int leafIndex;
   if ((leafIndex = geometry->externalLeaf(cameraPos)) == 0) {
      leafIndex = findLeaf(cameraPos);
   }
   ITRGeometry::BSPLeafWrap leafWrap(geometry, leafIndex);
   outsideBits = geometry->getOutsideBits(leafWrap);

   

#ifdef DEBUG
   ITRMetrics.render.currentLeaf = leafIndex;
   ITRMetrics.render.outsideBits = outsideBits;
#endif

#ifdef __EMSCRIPTEN__
   double _tS0 = emscripten_get_now();
#endif
   surfaceSet.setSize((geometry->surfaceList.size() >> 3) + 1);
   surfaceSet.clear(geometry->surfaceList.size());

   planeSet.setSize((geometry->planeList.size() >> 3) + 1);
   planeSet.clear(geometry->planeList.size());

   pointSet.setSize((geometry->point3List.size() >> 3) + 1);
   pointSet.clear(geometry->point3List.size());

   pointMap.setSize(geometry->point3List.size());

#ifdef __EMSCRIPTEN__
   ::g_wasmTphaseS += emscripten_get_now() - _tS0;
   double _tA0 = emscripten_get_now();
#endif
   leafVisible(leafWrap);
   processPVS(leafWrap);
   if(camera->getCameraType() == TS::PerspectiveCameraType)
      processPlanes(cameraPos);
   processSurfaces();
   processPoints();
#ifdef __EMSCRIPTEN__
   ::g_wasmTphaseA += emscripten_get_now() - _tA0;
#endif

#ifdef __EMSCRIPTEN__
   // WASM-PORT diag: is the interior render even reached, and does the BSP leaf / PVS yield any
   // visible surfaces? (iPoly=0 in the PATHS line => no interior surfaces emit.) Throttled.
   {
      static int s_itrLog = 0;
      if (::g_wasmDiag && (s_itrLog++ % 120) == 0) {
         int visSurf = 0;
         for (ITRBitVector::iterator it(surfaceSet); ++it; ) visSurf++;
         const Box3F& gb = geometry->box;
         bool inBox = (cameraPos.x >= gb.fMin.x && cameraPos.x <= gb.fMax.x &&
                       cameraPos.y >= gb.fMin.y && cameraPos.y <= gb.fMax.y &&
                       cameraPos.z >= gb.fMin.z && cameraPos.z <= gb.fMax.z);
         printf("[WASM-PORT][ITR] leaf=%d outBits=%d totSurf=%d visSurf=%d cam=(%.0f,%.0f,%.0f) "
                "box=[(%.0f,%.0f,%.0f)..(%.0f,%.0f,%.0f)] inBox=%d\n",
                leafIndex, outsideBits, geometry->surfaceList.size(), visSurf,
                cameraPos.x, cameraPos.y, cameraPos.z,
                gb.fMin.x, gb.fMin.y, gb.fMin.z, gb.fMax.x, gb.fMax.y, gb.fMax.z,
                (int)inBox), fflush(stdout);
      }
   }
#endif

   if (dynamic_cast<OpenGL::Surface*>(gfxSurface) == NULL) {
      renderSurfaces();
   } else {
//      OpenGL::Surface* pOGLSurface = static_cast<OpenGL::Surface*>(gfxSurface);
//      if (pOGLSurface->getTextureCache()->supportsSGIMultitexture() == true) {
#ifdef __EMSCRIPTEN__
         double _tB0 = emscripten_get_now();
         renderSurfacesOGL(false);
         double _tB1 = emscripten_get_now();
         ::g_wasmTphaseB += _tB1 - _tB0;
         renderSurfacesOGL(true);
         ::g_wasmTphaseC += emscripten_get_now() - _tB1;
#else
         renderSurfacesOGL(false);
         renderSurfacesOGL(true);
#endif
//      } else {
//
//       // First, build the handles, then render in two
//       //  passes...
//       renderSurfacesOGL(false);
//       renderSurfacesOGLSP();
//      }

      OpenGL::externCheckCache(dynamic_cast<OpenGL::Surface*>(gfxSurface));
   }
}


//----------------------------------------------------------------------------
// Flag the data in this leaf as visible.
//
void ITRRender::leafVisible(ITRGeometry::BSPLeafWrap& leafWrap)
{
   ITRMetrics.render.incLeafs();
   surfaceSet.uncompress(&geometry->bitList[leafWrap.getSurfaceIndex()],
                         leafWrap.getSurfaceCount());
   planeSet.uncompress(&geometry->bitList[leafWrap.getPlaneIndex()],
                       leafWrap.getPlaneCount());
}


//----------------------------------------------------------------------------
// Find the leaf that contains the given point.
//
int ITRRender::findLeaf(const Point3F& p,int nodeIndex)
{
   if (nodeIndex < 0) {
      return -(nodeIndex+1);
   }
   //
   ITRGeometry::BSPNode& node = geometry->nodeList[nodeIndex];
   if (geometry->planeList[node.planeIndex].whichSide(p) == TPlaneF::Inside)
      return findLeaf(p,node.front);
   return findLeaf(p,node.back);
}


//----------------------------------------------------------------------------
// Process all the leafs marked as visible from the given leaf.
//
void ITRRender::processPVS(ITRGeometry::BSPLeafWrap& leafWrap)
{
   if (leafWrap.isSolid()) {
      // Solid leaves have no pvs, and therefore, nothing is visible
      //  from them
      return;
   }

   UInt8* pbegin = &geometry->bitList[leafWrap.getPVSIndex()];
   UInt8* pend = pbegin + leafWrap.getPVSCount();
   for (ITRCompressedBitVector::iterator itr(pbegin,pend); ++itr; )
      // Don't bother processing any of the outside entries,
      // they are alway empty.
      if (*itr >= ITRGeometry::ReservedOutsideLeafs) {
         ITRGeometry::BSPLeafWrap vleafWrap(geometry, *itr);
         AssertFatal(vleafWrap.isSolid() == false,
                     "Should never be a solid node in PVS set");
         // Make sure the leaf bounding sphere intersects the
         // camera viewcone.
         const Box3F* pBox;
         vleafWrap.getBoundingBox(pBox);
         SphereF sphere;
         Point3F diag;
         sphere.center = diag = pBox->fMax;
         diag -= pBox->fMin;
         sphere.center += pBox->fMin;
         sphere.center *= 0.5f;
         sphere.radius = diag.lenf();
         if (camera->testVisibility(sphere) != TS::ClipNoneVis)
            leafVisible(vleafWrap);
      }
}


//----------------------------------------------------------------------------
// Remove backfaced planes from the bit set
//
void ITRRender::processPlanes(const Point3F& cp)
{
   for (ITRBitVector::iterator itr(planeSet); ++itr; ) {
      ITRMetrics.render.incPlanes();
      if (geometry->planeList[*itr].whichSide(cp) != TPlaneF::Inside)
         itr.clear();
   }
}  


//----------------------------------------------------------------------------
// Removed backfaced surfaces from the set and build a set of
// points to be transformed
//
void ITRRender::processSurfaces()
{
   for (ITRBitVector::iterator itr(surfaceSet); ++itr; ) {
      ITRGeometry::Surface& surface = geometry->surfaceList[*itr];
      if (!surface.planeFront ^ planeSet.test(surface.planeIndex))
         pointSet.uncompress(&geometry->bitList[surface.pointIndex],
            surface.pointCount);
      else
         itr.clear();
   }
}


//----------------------------------------------------------------------------
// Transform all the points in the point set
//
void ITRRender::processPoints()
{
   for (ITRBitVector::iterator itr(pointSet); ++itr; ) {
      AssertFatal((*itr) < geometry->point3List.size(),
                  "Point out of bounds...");

      Point3F& pp = geometry->point3List[*itr];
      PointM& pm = pointMap[*itr];
      pm.index = pointArray->addPoint(pp);
      pm.distance = m_dist(pp,cameraPos);
   }
}

//----------------------------------------------------------------------------

void ITRRender::renderSurfaces()
{
   static float textureScaleTable[ 1 << ITRGeometry::Surface::textureScaleBits ];
   
   // generate the scale table
   int size = 1 << ( ITRGeometry::Surface::textureScaleBits - 1 );
   for( int i = 0; i < size; i++ ) {
      textureScaleTable[ i + size ] = textureScale * ( 1 << i );
      textureScaleTable[ i ]        = textureScale / ( 1 << i );
   }
   
   instance->startRender ( *renderContext );
   
   for (ITRBitVector::iterator itr(surfaceSet); ++itr; ) {
      ITRMetrics.render.incSurfaces();
      ITRGeometry::Surface& surface = geometry->surfaceList[*itr];

      GFXTextureHandle gfxHandle;
      gfxHandle.key[0] = ((instance->m_instanceKey << 24) |
                          UInt32(instance->getDetailLevel()));
      gfxHandle.key[1] = *itr;

      ITRInstance::Surface& isurface = *instance->getSurface(*itr);
#ifdef __EMSCRIPTEN__
      {  // WASM-PORT diag [ITRMAT]: white walls render noTex. For each DISTINCT material index used by
         // a visible surface, log whether its material resolves a texture (getTextureMap NULL?), its
         // map filename + flags + dims. NULL with a real filename => rm.load failed (asset/VOL gap);
         // NULL with empty/non-texture flags => material has no texture (wrong type or a wrong/garbage
         // surface.material index). One line per distinct material -> no flood.
         static unsigned char s_seenMat[4096]; static bool s_miInit = false;
         if (!s_miInit) { memset(s_seenMat, 0, sizeof(s_seenMat)); s_miInit = true; }
         int mi = surface.material;
         if (mi >= 0 && mi < 4096 && !s_seenMat[mi]) {
            s_seenMat[mi] = 1;
            const GFXBitmap* bm = (*materialList)[mi].getTextureMap();
            g_itrMatTot++;
            if (::g_wasmDiag)
            printf("[ITRMAT] mat=%d %s flags=0x%x map=\"%s\" dim=%dx%d\n",
               mi, bm ? "OK  " : "NULL", (unsigned)(*materialList)[mi].fParams.fFlags,
               (*materialList)[mi].fParams.fMapFile, bm ? bm->getWidth() : -1, bm ? bm->getHeight() : -1);
            if (!bm) {
               g_itrMatNull++;
               if (g_itrFirstNullIdx < 0) {
                  g_itrFirstNullIdx   = mi;
                  g_itrFirstNullFlags = (Int32)((*materialList)[mi].fParams.fFlags);
                  const char* mf = (*materialList)[mi].fParams.fMapFile;
                  if (mf) { strncpy(g_itrFirstNullMap, mf, 63); g_itrFirstNullMap[63] = 0; }
               }
            }
         }
      }
#endif
      if (gfxSurface->setTextureHandle(gfxHandle)) {
         // Make sure light map hasn't changed
         if (!isurface.isLightMapValid()) {
            ITRMetrics.render.incTextureCache();
            GFXLightMap* lightMap = instance->buildLightMap(gfxSurface,*itr, 
                           textureScaleTable[ surface.textureScaleShift ] );
            gfxSurface->handleSetLightMap(
               instance->getLightScale(),lightMap);
            // let buildLightMap() handle this:
            // isurface.setLightMapValid();
         }
         
         // Make sure the texture hasn't changed
         if (!isurface.isTextureValid()) {
            ITRMetrics.render.incTextureCache();
            gfxSurface->handleSetTextureMap(
               const_cast<GFXBitmap*>((*materialList)[surface.material].getTextureMap()));
            isurface.setTextureValid();
         }
      }
      else {
         ITRMetrics.render.incTextureCache();
         GFXLightMap* lightMap = instance->buildLightMap(gfxSurface,*itr, 
                           textureScaleTable[ surface.textureScaleShift ] );
         int maxMip = geometry->highestMipLevel;
         gfxSurface->registerTexture(gfxHandle,
            (surface.textureSize.x+1) << maxMip,
            (surface.textureSize.y+1) << maxMip,
            surface.textureOffset.x,surface.textureOffset.y,
            instance->getLightScale(),lightMap,
            const_cast<GFXBitmap*>((*materialList)
               [surface.material].getTextureMap()),0);
         // let buildLightMap() handle this:
         // isurface.setLightMapValid();
         isurface.setTextureValid();
      }

      //
      static TS::VertexIndexPair ilist[200];
      AssertFatal(surface.vertexCount < 200,
         "ITRRender::renderSurfaces: Poly vertex count too high");
      if (surface.vertexCount >= 200)
         continue;

      ITRGeometry::Vertex* vertex = &geometry->vertexList[surface.vertexIndex];
      TS::TransformedVertex* va = &pointArray->getTransformedVertex(0);

      // Build list of vertices for 3Space and find closest
      // vertex to the camera.
      float distance = 1.0E20f;
      int clipAnd = TS::ClipMask;
      int clipOr = 0;
      for (int i = 0; i < surface.vertexCount; i++) {
         register ITRGeometry::Vertex& vp = vertex[i];
         register PointM& pm = pointMap[vp.pointIndex];
         ilist[i].fTextureIndex = vp.textureIndex;
         ilist[i].fVertexIndex = pm.index;

         if (pm.distance < distance)
            distance = pm.distance;

         register int status = va[pm.index].fStatus;
         clipOr |= status;
         clipAnd &= status;
      }

      // Early out testing and clip flags for 3Space
      if (clipAnd)
         continue;
      pointArray->setVisibility(clipOr);
   
      // Determim mip-level to use based on closest poly vertex
      int mipLevel;
      if (distance > 0.0f && distance != 1e20f) {
         float projRadius = camera->projectRadius(distance, textureScaleTable[surface.textureScaleShift]);
         mipLevel = getPower(int(float(1.0f / projRadius)));
      } else {
         mipLevel = 0;
      }
      mipLevel += PrefInteriorTextureDetail;

//    int mipLevel = (distance > 0.0f && distance != 1e20f) ? getPower(int(1.0f /
//       (camera->projectRadius(distance,textureScaleTable[ surface.textureScaleShift ])))): 0;
////        (camera->projectRadius(distance,textureScale )))): 0;

      // Make sure the miplevel is in range.  Note: this is a little confusing:
      //  highestMipLevel is actually the _smallest_ number allowed...
      //
      if (mipLevel < geometry->highestMipLevel)
         mipLevel = geometry->highestMipLevel;

#ifdef DEBUG
      int bd = (*materialList)[surface.material].
         getTextureMap()->detailLevels - 1;
      if (mipLevel > bd)
         mipLevel = bd;
#endif
      gfxSurface->handleSetMipLevel(mipLevel);

      // 
      ITRMetrics.render.incPolys();
      pointArray->drawPoly(surface.vertexCount,ilist,0);
   }
}


void ITRRender::renderSurfacesOGL(const bool in_renderPass)
{
   static float textureScaleTable[ 1 << ITRGeometry::Surface::textureScaleBits ];
   
   // generate the scale table
   int size = 1 << ( ITRGeometry::Surface::textureScaleBits - 1 );
   for( int i = 0; i < size; i++ ) {
      textureScaleTable[ i + size ] = textureScale * ( 1 << i );
      textureScaleTable[ i ]        = textureScale / ( 1 << i );
   }
   
   if (in_renderPass == false)
      instance->startRender ( *renderContext );
   
   for (ITRBitVector::iterator itr(surfaceSet); ++itr; ) {
      ITRMetrics.render.incSurfaces();
      ITRGeometry::Surface& surface = geometry->surfaceList[*itr];

      GFXTextureHandle gfxHandle;
      gfxHandle.key[0] = ((instance->m_instanceKey << 24) |
                          UInt32(instance->getDetailLevel()));
      gfxHandle.key[1] = *itr;

      ITRInstance::Surface& isurface = *instance->getSurface(*itr);
#ifdef __EMSCRIPTEN__
      {  // WASM-PORT diag [ITRMAT]: white walls render noTex. For each DISTINCT material index used by
         // a visible surface, log whether its material resolves a texture (getTextureMap NULL?), its
         // map filename + flags + dims. NULL with a real filename => rm.load failed (asset/VOL gap);
         // NULL with empty/non-texture flags => material has no texture (wrong type or a wrong/garbage
         // surface.material index). One line per distinct material -> no flood.
         static unsigned char s_seenMat[4096]; static bool s_miInit = false;
         if (!s_miInit) { memset(s_seenMat, 0, sizeof(s_seenMat)); s_miInit = true; }
         int mi = surface.material;
         if (mi >= 0 && mi < 4096 && !s_seenMat[mi]) {
            s_seenMat[mi] = 1;
            const GFXBitmap* bm = (*materialList)[mi].getTextureMap();
            g_itrMatTot++;
            if (::g_wasmDiag)
            printf("[ITRMAT] mat=%d %s flags=0x%x map=\"%s\" dim=%dx%d\n",
               mi, bm ? "OK  " : "NULL", (unsigned)(*materialList)[mi].fParams.fFlags,
               (*materialList)[mi].fParams.fMapFile, bm ? bm->getWidth() : -1, bm ? bm->getHeight() : -1);
            if (!bm) {
               g_itrMatNull++;
               if (g_itrFirstNullIdx < 0) {
                  g_itrFirstNullIdx   = mi;
                  g_itrFirstNullFlags = (Int32)((*materialList)[mi].fParams.fFlags);
                  const char* mf = (*materialList)[mi].fParams.fMapFile;
                  if (mf) { strncpy(g_itrFirstNullMap, mf, 63); g_itrFirstNullMap[63] = 0; }
               }
            }
         }
      }
#endif
      if (gfxSurface->setTextureHandle(gfxHandle)) {
         if (in_renderPass == false) {
            // Make sure light map hasn't changed
            if (!isurface.isLightMapValid()) {
#ifdef __EMSCRIPTEN__
               // Hard per-frame budget on animated-lightmap rebuilds. Once g_wasmLmapBudget rebuilds
               // have happened this frame (across all interiors), skip the rest: they keep their
               // cached GL lightmap from a prior frame (still lit, just a frozen flicker) and get a
               // turn on a later frame. 0 = unlimited (stock). g_wasmLmapBuiltCount is reset per frame
               // by the caller. This caps the ~0.8ms-each rebuild cost to budget*0.8ms/frame.
               bool doBuild = (::g_wasmLmapBudget <= 0) ||
                              (::g_wasmLmapBuiltCount < ::g_wasmLmapBudget);
               if (doBuild) {
                  ::g_wasmLmapBuild++;
                  ::g_wasmLmapBuiltCount++;
#endif
               ITRMetrics.render.incTextureCache();
               GFXLightMap* lightMap = instance->buildLightMap(gfxSurface,*itr,
                              textureScaleTable[ surface.textureScaleShift ] );
               gfxSurface->handleSetLightMap(
                  instance->getLightScale(),lightMap);
               // let buildLightMap() handle this:
               // isurface.setLightMapValid();
#ifdef __EMSCRIPTEN__
               }
#endif
            }

            // Make sure the texture hasn't changed
            if (!isurface.isTextureValid()) {
               ITRMetrics.render.incTextureCache();
               gfxSurface->handleSetTextureMap(
                  const_cast<GFXBitmap*>((*materialList)[surface.material].getTextureMap()));
               isurface.setTextureValid();
            }
         }
      }
      else {
#ifdef __EMSCRIPTEN__
         ::g_wasmSurfMiss++;   // WASM-PORT perf probe: GL handle cache miss -> full rebuild+upload
         ::g_wasmLmapBuild++;  // this path also (re)builds the lightmap
#endif
         ITRMetrics.render.incTextureCache();
         GFXLightMap* lightMap = instance->buildLightMap(gfxSurface,*itr,
                           textureScaleTable[ surface.textureScaleShift ] );
         int maxMip = geometry->highestMipLevel;
         gfxSurface->registerTexture(gfxHandle,
            (surface.textureSize.x+1) << maxMip,
            (surface.textureSize.y+1) << maxMip,
            surface.textureOffset.x,surface.textureOffset.y,
            instance->getLightScale(),lightMap,
            const_cast<GFXBitmap*>((*materialList)
               [surface.material].getTextureMap()),0);
         // let buildLightMap() handle this:
         // isurface.setLightMapValid();
         isurface.setTextureValid();
      }

      if (in_renderPass == true) {
         //
         static TS::VertexIndexPair ilist[200];
         AssertFatal(surface.vertexCount < 200,
            "ITRRender::renderSurfaces: Poly vertex count too high");
         if (surface.vertexCount >= 200)
            continue;
#ifdef __EMSCRIPTEN__
         ::g_wasmSurfDraw++;                       // WASM-PORT perf probe: polys drawn this frame
         ::g_wasmSurfVerts += surface.vertexCount; // ... and total verts emitted
#endif

         ITRGeometry::Vertex* vertex = &geometry->vertexList[surface.vertexIndex];
         TS::TransformedVertex* va = &pointArray->getTransformedVertex(0);

         // Build list of vertices for 3Space and find closest
         // vertex to the camera.
         float distance = 1.0E20f;
         int clipAnd = TS::ClipMask;
         int clipOr = 0;
         for (int i = 0; i < surface.vertexCount; i++) {
            register ITRGeometry::Vertex& vp = vertex[i];
            register PointM& pm = pointMap[vp.pointIndex];
            ilist[i].fTextureIndex = vp.textureIndex;
            ilist[i].fVertexIndex = pm.index;

            if (pm.distance < distance)
               distance = pm.distance;

            register int status = va[pm.index].fStatus;
            clipOr |= status;
            clipAnd &= status;
         }

         // Early out testing and clip flags for 3Space
         if (clipAnd)
            continue;
         pointArray->setVisibility(clipOr);
   
         // 
         ITRMetrics.render.incPolys();
         pointArray->drawPoly(surface.vertexCount,ilist,0);
      }
   }
}

void ITRRender::renderSurfacesOGLSP()
{
   using namespace OpenGL;

   instance->startRender ( *renderContext );
   
   Surface* pSurface               = static_cast<Surface*>(gfxSurface);
   Surface::TextureCache* pTxCache = pSurface->getTextureCache();
   HandleCache* pHCache            = pSurface->getHandleCache();

   gfxSurface->setFillMode(GFX_FILL_TEXTUREP1);

//	Int16		__cdecl m_fpuGetControlState();
//	void		__cdecl m_fpuSetControlState(Int16 state);
   Int16 storeFPUState = m_fpuGetControlState();

   for (ITRBitVector::iterator itr(surfaceSet); ++itr; ) {
      ITRMetrics.render.incSurfaces();
      ITRGeometry::Surface& surface = geometry->surfaceList[*itr];

      GFXTextureHandle gfxHandle;
      gfxHandle.key[0] = ((instance->m_instanceKey << 24) |
                          UInt32(instance->getDetailLevel()));
      gfxHandle.key[1] = *itr;

      if (pSurface->setTextureHandle(gfxHandle) == false) {
         AssertFatal(false, "This should never happen");
      }

      // Here we are rendering the textures
      //
      HandleCacheEntry* pCEntry = pHCache->getCurrentEntry();

      if (pTxCache->setTexture(pCEntry->pBmp->getCacheInfo()) == false) {
         AssertFatal(0, "this should never happen");
      }

      static TS::VertexIndexPair ilist[200];
      AssertFatal(surface.vertexCount < 200, "ITRRender::renderSurfaces: Poly vertex count too high");
      if (surface.vertexCount >= 200)
         continue;

      ITRGeometry::Vertex* vertex = &geometry->vertexList[surface.vertexIndex];
      TS::TransformedVertex* va = &pointArray->getTransformedVertex(0);

      // Build list of vertices for 3Space and find closest
      // vertex to the camera.
      float distance = 1.0E20f;
      int clipAnd = TS::ClipMask;
      int clipOr = 0;
      for (int i = 0; i < surface.vertexCount; i++) {
         register ITRGeometry::Vertex& vp = vertex[i];
         register PointM& pm = pointMap[vp.pointIndex];
         ilist[i].fTextureIndex = vp.textureIndex;
         ilist[i].fVertexIndex = pm.index;

         if (pm.distance < distance)
            distance = pm.distance;

         register int status = va[pm.index].fStatus;
         clipOr |= status;
         clipAnd &= status;
      }

      // Early out testing and clip flags for 3Space
      if (clipAnd)
         continue;
      pointArray->setVisibility(clipOr);
   
      m_fpuSetControlState(storeFPUState);

      // 
      ITRMetrics.render.incPolys();
      pointArray->drawPoly(surface.vertexCount,ilist,0);
   }

   pSurface->setFillMode(GFX_FILL_LIGHTMAP);
   pSurface->setHazeSource(GFX_HAZE_NONE);

   for (ITRBitVector::iterator itr2(surfaceSet); ++itr2; ) {
      ITRMetrics.render.incSurfaces();
      ITRGeometry::Surface& surface = geometry->surfaceList[*itr2];

      GFXTextureHandle gfxHandle;
      gfxHandle.key[0] = ((instance->m_instanceKey << 24) | UInt32(instance->getDetailLevel()));
      gfxHandle.key[1] = *itr2;

      if (pSurface->setTextureHandle(gfxHandle) == false) {
         AssertFatal(false, "This should never happen");
      }

      // Here we are rendering the lightmaps
      //
      HandleCacheEntry* pCEntry = pHCache->getCurrentEntry();

      if (pTxCache->setLightmap(*(pCEntry->pLightmapCacheInfo)) == false) {
         AssertFatal(0, "this should never happen");
      }

      static TS::VertexIndexPair ilist[200];
      if (surface.vertexCount >= 200)
         continue;

      ITRGeometry::Vertex* vertex = &geometry->vertexList[surface.vertexIndex];
      TS::TransformedVertex* va = &pointArray->getTransformedVertex(0);

      // Build list of vertices for 3Space and find closest
      // vertex to the camera.
      float distance = 1.0E20f;
      int clipAnd = TS::ClipMask;
      int clipOr = 0;
      for (int i = 0; i < surface.vertexCount; i++) {
         register ITRGeometry::Vertex& vp = vertex[i];
         register PointM& pm = pointMap[vp.pointIndex];
         ilist[i].fTextureIndex = vp.textureIndex;
         ilist[i].fVertexIndex = pm.index;

         if (pm.distance < distance)
            distance = pm.distance;

         register int status = va[pm.index].fStatus;
         clipOr |= status;
         clipAnd &= status;
      }

      // Early out testing and clip flags for 3Space
      if (clipAnd)
         continue;
      pointArray->setVisibility(clipOr);
   
   m_fpuSetControlState(storeFPUState);

      pointArray->drawPoly(surface.vertexCount,ilist,0);
   }
}
