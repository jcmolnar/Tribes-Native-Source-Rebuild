#pragma once

// WASM-PORT: header reconstructed from fearInteriorShape.cpp usage (0.8.1 shipped an
// empty fearInteriorShape.h). FearInteriorShape is the Fear-game wrapper over the engine
// InteriorShape, adding explosion-scoper hooks (spawnExplosion/getExplosionFilter, both
// currently stubbed in the .cpp) and an explosion tag. Position/rotation/filename fields
// live on the InteriorShape base (m_shapePosition / m_shapeRotation / m_pFileName).

#include "interiorShape.h"

class FearInteriorShape : public InteriorShape
{
	typedef InteriorShape Parent;

	DECLARE_PERSISTENT(FearInteriorShape);

public:
	SimTag m_explosionTag;

	FearInteriorShape() : m_explosionTag(0) {}

	virtual void spawnExplosion();
	virtual char* getExplosionFilter() const;

	static void initPersistFields();
};
