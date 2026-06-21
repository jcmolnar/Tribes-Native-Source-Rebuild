#pragma once

// WASM-PORT: header reconstructed from hover.cpp usage (the 0.8.1 source shipped an
// empty Hover.h; HoverData lived only in the .cpp). Hover is a Vehicle subclass that
// floats at data->hoverHeight over terrain/interiors. Members not found here are
// inherited from Vehicle (speed/desiredSpeed/onGround/updateSkip/lastPlayerMove) or
// GameBase (lastProcessTime).

#include "vehicle.h"

class Player;

class Hover : public Vehicle
{
	typedef Vehicle Parent;

	DECLARE_PERSISTENT(Hover);

public:

	struct HoverData : public VehicleData
	{
		float hoverHeight;
		void pack(BitStream* stream) override;
		void unpack(BitStream* stream) override;
	};

public: //private:

	HoverData* data;

	Player* pilot;
	Player* lastPilot;

	Point3F velocity;
	float bounce;
	float bounceDir;
	float pitch;

	void setDesiredZ(float adjust);
	void dynamics(float adjust);
	void dismount();   // WASM-PORT: pilot eject; stock Vehicle had this, lost in reconstruction

protected:
	bool initResources(GameBaseData* in_data) override;
	void clientProcess(DWORD curTime) override;
	void serverProcess(DWORD curTime) override;
	bool updateMove(PlayerMove* move, float interval) override;
	bool processCollision(SimMovementInfo* info) override;
	Error read(StreamIO& sio, int, int) override;
	Error write(StreamIO& sio, int, int) override;
	int getDatGroup() override;

public:
	Hover();
};
