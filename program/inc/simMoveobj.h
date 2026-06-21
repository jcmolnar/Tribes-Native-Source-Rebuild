//--------------------------------------------------------------------------- 


//--------------------------------------------------------------------------- 

#ifndef _SIMMOVEOBJ_H_
#define _SIMMOVEOBJ_H_

#include <sim.h>
#include <move.h>

#include "simaction.h"
#include "simContainer.h"
#include "simCollideable.h"


//--------------------------------------------------------------------------- 

class SimMoveObject: public SimCollideable
{
	typedef SimCollideable Parent;

	DECLARE_PERSISTENT(SimMoveObject);   // WASM-PORT: IMPLEMENT_PERSISTENT in .cpp needs this

	Point3F throttle;
	Point3F desired_throttle;
	SimCollisionSphereImage collisionImage;
	Resource<SimActionMap> actionMap;

	SimTime lastUpdate;                  // WASM-PORT: tracked in .cpp, never declared
	float getFarPlane();                 // WASM-PORT: internal helper defined in .cpp

	void update(SimTime time);

public:
	MoveLinear movement;
	MoveLinear rotation;

	const Point3F& getPos(void);
	void setPos(Point3F pos);
	void setActionMap(const char* file);

	SimMoveObject();
	~SimMoveObject();
	bool processArguments(int argc, const char** argv);   // WASM-PORT: defined in .cpp
	bool processEvent(const SimEvent*);
	bool processQuery(SimQuery*);
	bool onAdd();
};


#endif
