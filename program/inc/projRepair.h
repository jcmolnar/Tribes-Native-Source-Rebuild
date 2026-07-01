#pragma once
#include "projLightning.h"

class RepairEffect : public Lightning
{
	typedef Lightning Parent;
	DECLARE_PERSISTENT(RepairEffect);

public:
	struct RepairEffectData : LightningData
	{
		RepairEffectData();
	};

	struct RepairRenderImage : public LightningRenderImage
	{
		void updateSegments();
		~RepairRenderImage();
		void createSinSegments(const int in_line);
		void render(TS::RenderContext& io_rRC);

		// NATIVE-PORT: was an unimplemented stub (throw std::exception()) -> uncaught
		// C++ throw = client CRASH the moment a repair beam rendered in view.
		// RepairEffect is a ghosted class (IMPLEMENT_PERSISTENT_TAG), so any player
		// using a repair gun/kit near you reaches this through render(). j is the
		// point index along the beam, i the line index; the segment arrays are the
		// inherited m_lines[] filled by createSinSegments, so delegate to the same
		// (bounds-clamped) Line::getPoint interpolation the lightning beam uses.
		Point3F getPoint(int j, int i, DWORD currentTime, const TMat3F& mat3F, float length)
		{
			if (i < 0 || i >= 8 || pLightning == NULL || pLightning->m_pLightningData == NULL)
				return Point3F(0, 0, 0);
			int numPoints = (1 << pLightning->m_pLightningData->segmentDivisions) + 1;
			return m_lines[i].getPoint(j, numPoints, currentTime, mat3F, length);
		}


	};

	bool m_targetEvaluated;

	RepairEffect(int in_datFileId);
	RepairEffect();
	int getDatGroup();
	void serverProcess(DWORD in_currTime);
	void determineTarget();
	void resetEndPoint();
	void shutOffProjectile();
	bool onSimRenderQueryImage(SimRenderQueryImage* query);
	bool processQuery(SimQuery* query);
};
