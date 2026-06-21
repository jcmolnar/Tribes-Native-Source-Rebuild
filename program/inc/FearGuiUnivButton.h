#pragma once

#include "simGuiActiveCtrl.h"

namespace FearGui
{
	class FGUniversalButton : public SimGui::ActiveCtrl
	{
		typedef ActiveCtrl Parent;

		DECLARE_PERSISTENT(FGUniversalButton);

	public:
		enum
		{
			BMP_Standard,
			BMP_Pressed = 4,
			BMP_Ghosted,
			BMP_BackgroundOn,
			BMP_BackgroundOff,
			BMP_Count
		};

		virtual bool isSet() const
		{
			return this->mbSet;
		}

		virtual void setMode(const bool mode) const
		{
			// NATIVE-PORT: was an unimplemented stub (throw std::exception()) -> uncaught C++ throw
			// crashed the client when the server-INFO dialog set its favorite/neverPing toggle
			// buttons (dlgJoinGame.cpp:512/516). Set the toggle state (mbSet), as setScriptValue
			// does, and redraw. const_cast because the RE'd signature is const but must mutate.
			FGUniversalButton* self = const_cast<FGUniversalButton*>(this);
			self->mbSet = mode;
			self->setUpdate();
		}

		virtual void setFlashing(const bool value)
		{
			this->mbFlashing = value;
		}

	protected:
		Int32 getMouseCursorTag() override;
		bool onAdd() override;
		void onWake() override;
		DWORD onMessage(SimObject* sender, DWORD msg) override;
		void onMouseDown(const SimGui::Event& event) override;
		const char* getScriptValue() override;
		void setScriptValue(const char* value) override;
		void onPreRender() override;
		void onRender(GFXSurface* sfc, Point2I offset, const Box2I&) override;
		void inspectWrite(Inspect* insp) override;
		void inspectRead(Inspect* insp) override;
		Error write(StreamIO& sio, int version, int user) override;
		Error read(StreamIO& sio, int version, int user) override;

	public: //private:

		bool mbSwitch;
		bool mbSet;
		bool mbFlashing;
		int mFlashTime;

		SimTag mBitmapTag;
		SimTag mRadioTag;

		char mBitmapRootName[256 - 8];

		bool mbLowRes;

		Point2I mHiResPosition;
		Point2I mLoResPosition;

		bool mbMirrorConsoleVar;

		int currentVersion;

		Resource<GFXBitmap> mBitmaps[BMP_Count * 2];
		Resource<GFXBitmap> mRadioCheckBMP;
		Resource<GFXBitmap> mRadioCheckGhostBMP;


	public:
		FGUniversalButton();
		virtual void setBitmaps();
		virtual void setLowRes(bool lores);
		virtual void onAction();

		virtual SimTag getRadioTag() const
		{
			return this->mRadioTag;
		}
	};
}
