#pragma once

// WASM-PORT: header reconstructed from FearGuiGenericCB.cpp usage (0.8.1 shipped an empty
// FearGuiGenericCB.h). A generic string dropdown — the standard combo trio over the
// FGComboBox / FGComboPopUp / FGComboList bases (FearGuiCombo.h), modeled on
// FearGuiStandardCombo.h. GenericListCtrl carries its own Entry storage; selectedCell/
// setSize come from SimGui::ArrayCtrl, message from SimGui::ActiveCtrl.

#include "FearGuiCombo.h"

namespace FearGui
{
	class FGGenericComboBox;

	// WASM-PORT: extends SimGui::ArrayCtrl (NOT FGComboList) — its findEntry returns Point2I
	// where FGComboList/TextList::findEntry returns int, an override clash. So findEntry/
	// addEntry/getCellText here are NEW methods, not overrides. NOTE: cell rendering relies
	// on a base that calls getCellText (FGComboList does, ArrayCtrl doesn't), so when the
	// shell menus are wired, verify this dropdown actually draws its rows (may need an
	// onRenderCell override here).
	class GenericListCtrl : public SimGui::ArrayCtrl
	{
		typedef SimGui::ArrayCtrl Parent;

		DECLARE_PERSISTENT(GenericListCtrl);

		enum { MAX_ENTRY_LENGTH = 255 };

		struct Entry
		{
			char  name[MAX_ENTRY_LENGTH + 1];
			Int32 id;
		};

		Vector<Entry> entries;

	public:
		void clear(void);
		Point2I findEntry(const char* entry);
		void addEntry(const Int32 id, const char* entry);
		void setSelection(const char* entry);
		const char* getSelectedText(Point2I& cell);
		bool onAdd() override;
		void onWake() override;
		const char* getCellText(GFXSurface*, const Point2I& cell, const Point2I&, const Point2I&);
	};

	class FGGenericComboBox : public FGComboBox
	{
		typedef FGComboBox Parent;

		DECLARE_PERSISTENT(FGGenericComboBox);

	public:
		void clear(void);
		Point2I findEntry(const char* entry);
		void addEntry(const Int32 id, const char* entry);
		void setSelection(const char* entry);
		const char* getSelectedText(Point2I& cell);

		bool onAdd() override;
		void openPopUpCtrl() override;
		void updateFromArrayCtrl() override;
	};

	class FGGenericPopUp : public FGComboPopUp
	{
		typedef FGComboPopUp Parent;

	public:
		FGGenericPopUp(FGGenericComboBox* parent) : Parent(parent)
		{
		}

		void clear(void);
		Point2I findEntry(const char* entry);
		void addEntry(const Int32 id, const char* entry);
		void setSelection(const char* entry);
		const char* getSelectedText(Point2I& cell);

		bool onAdd() override;
	};
}
