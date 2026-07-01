#pragma once

#include "projLaser.h"
#include "ts_RenderContext.h"

class TargetLaser : public LaserProjectile
{
	typedef LaserProjectile Parent;

	DECLARE_PERSISTENT(TargetLaser);

	static const UInt32 csm_updateFrequencyMS;
	static const UInt32 csm_targetFrequencyMS;

	bool m_waitingForShutoff;
	bool m_waitingToTarget;

	SimTime m_interpTimeStart;
	Point3F m_interpolateTo;
	Point3F m_interpolateFrom;
	bool m_waitingToGhost;
	DWORD m_lastGhosted;

	int m_playerId;
	int m_teamId;
	bool m_hitTarget;
	DWORD m_lastTargetSent;	

	enum
	{
		TLPosUpdateMask = LastMaskBit,
		LastMaskBit = LastMaskBit << 1
	};

	

public:

	struct TargetLaserData : LaserData
	{
		
	};

	TargetLaser();
	explicit TargetLaser(const Int32 in_datFileId);
	virtual ~TargetLaser();
	int getDatGroup();
	void clientProcess(DWORD);
	void serverProcess(DWORD in_currTime);
	bool processQuery(SimQuery* query);
	void shutOffProjectile();
	bool onSimRenderQueryImage(SimRenderQueryImage* query);
	void updateImageTransform(const TMat3F& in_rTrans);
	bool onAdd();
	void writeInitialPacket(Net::GhostManager* gm, BitStream* pStream);
	void readInitialPacket(Net::GhostManager* gm, BitStream* pStream);
	DWORD packUpdate(Net::GhostManager* gm, DWORD mask, BitStream* pStream);
	void unpackUpdate(Net::GhostManager* gm, BitStream* pStream);
	// NATIVE-PORT: added const + override -- the base GameBase::getTarget is a
	// const virtual, so the old non-const declaration HID it instead of
	// overriding; virtual dispatch through GameBase* never reached this body.
	bool getTarget(Point3F* out_pTarget, int* out_pTeam) const override;
	bool isSustained() const;
	// NATIVE-PORT: was an unimplemented stub (throw std::exception()) -> crash
	// when the AI targeting loop (aiObj.cpp) checked laser ownership. The
	// owning player's id is m_playerId (read off the ghost initial packet).
	bool wasShotBy(Player* player);   // defined in projTargetLaser.cpp (needs Player complete)
};
