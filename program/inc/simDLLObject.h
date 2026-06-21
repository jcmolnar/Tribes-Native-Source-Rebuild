#pragma once

// WASM-PORT: header reconstructed from simDLLObject.cpp usage (0.8.1 shipped an empty
// SimDLLObject.h). This is a Win32 DLL loader (LoadLibrary/GetProcAddress) and is inert
// under wasm — the shim's windows.h stubs the Win32 calls; nothing actually loads a DLL
// in the browser. Reconstructed only so the module links.

#include "simBase.h"
#include "tVector.h"
#include "windows.h"

#ifndef MaxPathLength
#define MaxPathLength 260
#endif

class SimDLLObject : public SimSet
{
	typedef SimSet Parent;

	DECLARE_PERSISTENT(SimDLLObject);

	struct Entry
	{
		char    fileName[MaxPathLength];
		HMODULE hDLL;
		int     refCount;
	};

	struct DLLList : public Vector<Entry>
	{
		static CRITICAL_SECTION cs;

		DLLList();
		~DLLList();

		HMODULE open(const char* file);
		void    close(HMODULE handle);
	};

	static DLLList list;
	HMODULE hDLL;

public:
	SimDLLObject();
	~SimDLLObject();

	bool processArguments(int argc, const char** argv);
};
