#pragma once

// WASM-PORT: header reconstructed from Door.cpp usage (0.8.1 shipped an empty door.h).
// Door is a MoveableBase (like Elevator) whose onFirst/onLast fire onClose/onOpen
// scripts. Stored as a Moveable datablock (no dedicated DataBlockManager enum).
#include "MoveableBase.h"

class Door : public MoveableBase
{
	typedef MoveableBase Parent;

	DECLARE_PERSISTENT(Door);

public:
	Door();

protected:
	int getDatGroup() override;
	Error read(StreamIO& sio, int iVer, int iUsr) override;
	Error write(StreamIO& sio, int iVer, int iUsr) override;

	void onFirst() override;
	void onLast() override;

public:

	virtual ~Door();
};
