#pragma once

// WASM-PORT: header reconstructed from FearGuiGenderCombo.cpp usage (0.8.1 shipped an empty
// FearGuiGenderCombo.h). The player-gender dropdown — same combo trio over FGComboBox /
// FGComboPopUp / FGComboList (FearGuiCombo.h) as FearGuiStandardCombo.h. GenderListCtrl is
// a fixed two-row list (MALE/FEMALE) so it needs no Entry storage.

#include "FearGuiCombo.h"

namespace FearGui
{
	class FGGenderComboBox;

	// WASM-PORT: extends SimGui::ArrayCtrl (see GenericListCtrl note in FearGuiGenericCB.h —
	// same getCellText/render caveat; this one is a fixed two-row MALE/FEMALE list).
	class GenderListCtrl : public SimGui::ArrayCtrl
	{
		typedef SimGui::ArrayCtrl Parent;

		DECLARE_PERSISTENT(GenderListCtrl);

	public:
		bool onAdd() override;
		void onWake() override;
		int getSelectedGender(void);
		const char* getCellText(GFXSurface*, const Point2I& cell, const Point2I&, const Point2I&);
	};

	class FGGenderComboBox : public FGComboBox
	{
		typedef FGComboBox Parent;

		DECLARE_PERSISTENT(FGGenderComboBox);

	public:
		bool onAdd() override;
		void openPopUpCtrl() override;
		void updateFromArrayCtrl() override;
	};

	class FGGenderPopUp : public FGComboPopUp
	{
		typedef FGComboPopUp Parent;

	public:
		FGGenderPopUp(FGGenderComboBox* parent) : Parent(parent)
		{
		}

		bool onAdd() override;
	};
}
