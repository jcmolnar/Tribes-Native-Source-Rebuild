#pragma once

// WASM-PORT: header reconstructed from FearGuiIRCServersCB.cpp usage (0.8.1 shipped an
// empty FearGuiIRCServersCB.h). The IRC-server picker dropdown — same combo trio over the
// FGComboBox / FGComboPopUp bases (FearGuiCombo.h). Self-contained config UI (server/port/
// room strings persisted to IRCServerList.cs); does NOT depend on the IRC client subsystem.
// IRCServersVarCtrl extends SimGui::ArrayCtrl directly (its own IRCServerRep storage;
// getCellText is a new method — see the GenericListCtrl render caveat in FearGuiGenericCB.h).

#include "FearGuiCombo.h"

namespace FearGui
{
	class FGIRCServersComboBox;

	class IRCServersVarCtrl : public SimGui::ArrayCtrl
	{
		typedef SimGui::ArrayCtrl Parent;

		DECLARE_PERSISTENT(IRCServersVarCtrl);

	public:
		struct IRCServerRep
		{
			char address[256];
			int  port;
			char description[256];
			char defaultRoom[256];
		};

		Vector<IRCServerRep>     entries;
		VectorPtr<IRCServerRep*> entryPtrs;

		~IRCServersVarCtrl(void);

		void addIRCServer(int argc, const char* argv[]);
		void addIRCServer(char* description);
		void removeSelected(void);
		bool onAdd() override;
		void setSelected(const char* address);
		void setSelected(int index);
		bool cellSelected(Point2I cell) override;
		IRCServerRep* getSelectedServer(void);
		void setSelectedServerText(char* text);
		void setSelectedPortText(char* text);
		void setSelectedRoomText(char* text);
		const char* getCellText(GFXSurface*, const Point2I& cell, const Point2I&, const Point2I&);
		void writeScriptFile(void);
	};

	class FGIRCServersComboBox : public FGComboBox
	{
		typedef FGComboBox Parent;

		DECLARE_PERSISTENT(FGIRCServersComboBox);

	protected:
		Resource<GFXBitmap> mTitleBMP;
		Resource<GFXBitmap> mTitleGhostBMP;

	public:
		Point2I getSelected(void) override;
		void setPopUpMessage(Int32 msg);
		bool onAdd() override;
		void openPopUpCtrl() override;
		void updateFromArrayCtrl() override;
		void addIRCServer(char* description);
		IRCServersVarCtrl::IRCServerRep* getSelectedServer(void);
		void removeSelected(void);
		void setSelectedServerText(char* text);
		void setSelectedPortText(char* text);
		void setSelectedRoomText(char* text);
		void writeScriptFile(void);
	};

	class FGIRCServersPopUp : public FGComboPopUp
	{
		typedef FGComboPopUp Parent;

	public:
		FGIRCServersPopUp(FGIRCServersComboBox* parent) : Parent(parent)
		{
		}

		bool onAdd() override;
	};
}
