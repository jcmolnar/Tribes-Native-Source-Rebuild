#pragma once

// WASM-PORT: header reconstructed from FearGuiRecordings.cpp usage (0.8.1 shipped an empty
// FearGuiRecordings.h). The demo/recording picker — combo trio over FGComboBox/FGComboPopUp
// (FearGuiCombo.h). FGRecordingList scans recordings\*.rec (FindFirstFile) into its own
// PlaybackRep list and can pick a random unplayed one. Extends SimGui::ArrayCtrl directly
// (own storage; getCellText is a new method — see GenericListCtrl note in FearGuiGenericCB.h).
// PlaybackRep is public: the .cpp keeps a file-scope Vector<FGRecordingList::PlaybackRep>.

#include "FearGuiCombo.h"
#include "m_random.h"

namespace FearGui
{
	class FGRecordingComboBox;

	class FGRecordingList : public SimGui::ArrayCtrl
	{
		typedef SimGui::ArrayCtrl Parent;

		DECLARE_PERSISTENT(FGRecordingList);

		Random rand;

	public:
		struct PlaybackRep
		{
			char name[256];
			bool alreadyPlayed;
		};

		bool onAdd() override;
		void onWake() override;
		void setSelectedRecording(const char* recordingName);
		const char* getSelectedRecording(void);
		const char* getSelectedRecordingName(void);
		void selectRandomRecording(void);
		const char* getCellText(GFXSurface*, const Point2I& cell, const Point2I&, const Point2I&);
	};

	class FGRecordingComboBox : public FGComboBox
	{
		typedef FGComboBox Parent;

		DECLARE_PERSISTENT(FGRecordingComboBox);

	public:
		bool onAdd() override;
		void openPopUpCtrl() override;
		const char* getSelectedRecording(void);
		void selectRandomRecording(void);
		void updateFromArrayCtrl() override;
	};

	class FGRecordingPopUp : public FGComboPopUp
	{
		typedef FGComboPopUp Parent;

	public:
		FGRecordingPopUp(FGRecordingComboBox* parent) : Parent(parent)
		{
		}

		bool onAdd() override;
		const char* getSelectedRecording(void);
		void selectRandomRecording(void);
	};
}
