//------------------------------------------------------------------------------
// Description 
//    
// $Workfile$
// $Revision$
// $Author  $
// $Modtime $
//
//------------------------------------------------------------------------------

#include "fearInteriorShape.h"
#include "fearDcl.h"
//#include "fearExplosion.h"

IMPLEMENT_PERSISTENT_TAG(FearInteriorShape, FearInteriorShapePersTag);

//------------------------------------------------------------------------------
//--------------------------------------
// Explosion work...
//--------------------------------------
//
void
FearInteriorShape::spawnExplosion()
{
   if (m_explosionTag != 0) {
//      FearExplosionScoper* explosion = new FearExplosionScoper(m_explosionTag);
//      explosion->setExpPosition(getLinearPosition());
//      explosion->setExpAxis(Point3F(0, 0, 1));
//      manager->addObject(explosion);
   }
}

char*
FearInteriorShape::getExplosionFilter() const
{
   return "IDEXP_ITRSHAPE_*";
}

void FearInteriorShape::initPersistFields()
{
   // WASM-PORT: the source placeholders getShapePosition()/getShapeRotation() were never
   // defined; addField wants a member lvalue for Offset, so map to the inherited fields.
   addField("position", TypePoint3F, Offset(m_shapePosition, FearInteriorShape));
   addField("rotation", TypePoint3F, Offset(m_shapeRotation, FearInteriorShape));
   addField("filename", TypeString, Offset(m_pFileName, FearInteriorShape));
}
