//--------------------------------------------------------------------------- 


//--------------------------------------------------------------------------- 

#ifndef _SIMMOVEOBJ_H_
#define _SIMMOVEOBJ_H_

#include <sim.h>
#include <move.h>
#include "simContainer.h"
#include "simCollideable.h"


//--------------------------------------------------------------------------- 

class SimMoveObject: public SimCollideable
{
	typedef SimCollideable Parent;

	// WASM-PORT: this header (not program/inc/simMoveobj.h) is the copy actually pulled
	// into the program build — same _SIMMOVEOBJ_H_ guard, and engine\SimObjects\Inc
	// precedes program\inc on the include path. The matching program/code/simMoveobj.cpp
	// is a richer "Fear" version that needs persistence + these extra members. The engine's
	// own simMoveobj.cpp is NOT compiled, so adding DECLARE_PERSISTENT here is safe (the
	// single IMPLEMENT_PERSISTENT lives in program/code/simMoveobj.cpp).
	DECLARE_PERSISTENT(SimMoveObject);

	Point3F throttle;
	Point3F desired_throttle;
	SimCollisionSphereImage collisionImage;
	Resource<SimActionMap> actionMap;

	SimTime lastUpdate;
	float getFarPlane();

	void update(SimTime time);

public:
	MoveLinear movement;
	MoveLinear rotation;

	const Point3F& getPos(void);
	void setPos(Point3F pos);
	void setActionMap(const char* file);

	SimMoveObject();
	~SimMoveObject();
	bool processArguments(int argc, const char** argv);
	bool processEvent(const SimEvent*);
	bool processQuery(SimQuery*);
	bool onAdd();
};


#endif
