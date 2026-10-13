#include "Legacy/MCEngine.h"
#include "imgui.h"
#include "Legacy/MC_Picker.h"

void MCEngine::UpdateInput(float dt)
{
	UpdateInput(mPlatform.GetInput(), dt);
	UpdateCamera();
}

// placeholder code for old code to work with MC::Input & MC::PlatformSystem
void MCEngine::UpdateInput(const MC::Input& input, float dt)
{
	using Button = MC::Input::MouseButton;

	if (input.MouseEnabled())
	{
		if (input.WasMousePressed(Button::Left)) {
			MC_Picker& picker = MC_Picker::GetPicker(*this);
			if (auto* scene = mSceneManager.GetActive()) {
				int picked = picker.PickRenderItemOnScreen(GetSceneMousePos(), *scene);
				// -1 (miss) clears selection; any valid index drives outliner + inspector.
				mSelectedItemIndex = picked;
			}
		}

		// A drag moves the camera by this frame's mouse movement: the middle button pans,
		// the right button looks around. A held left button blocks both.
		const bool mouseMoved = input.MouseDeltaX() != 0 || input.MouseDeltaY() != 0;
		if (mouseMoved && !input.IsMouseDown(Button::Left))
		{
			if (input.IsMouseDown(Button::Middle))
			{
				// Make each pixel correspond to 0.005 unit in the scene.
				float dx = 0.005f * static_cast<float>(input.MouseDeltaX());
				float dy = 0.005f * static_cast<float>(input.MouseDeltaY());
				mMainCamera->Strafe(-5.0f * (dx));
				mMainCamera->Climb(5.0f * (dy));
				mCameraDirty = true;
			}
			else if (input.IsMouseDown(Button::Right))
			{
				float dx = DirectX::XMConvertToRadians(0.25f * static_cast<float>(input.MouseDeltaX()));
				float dy = DirectX::XMConvertToRadians(0.25f * static_cast<float>(input.MouseDeltaY()));
				mMainCamera->Pitch(dy);
				mMainCamera->RotateY(dx);
				mCameraDirty = true;
			}
		}

		// The wheel walks the camera, 2 units per notch.
		if (input.WheelDelta() != 0.0f)
		{
			mMainCamera->Walk(2.0f * input.WheelDelta());
			mCameraDirty = true;
		}
	}

	// ESC closes the window like its close button, but not while an ImGui text field is being
	// edited: ESC then cancels the edit. WantTextInput is true only for an active text field.
	// (WantCaptureKeyboard is not usable here: keyboard navigation is enabled, so it is true
	// whenever any ImGui window has navigation focus, which in the docked editor is nearly always.)
	if (input.IsKeyTriggered(MC::Key::Escape) && !ImGui::GetIO().WantTextInput)
		mPlatform.RequestClose();

#if defined(_DEBUG)
	if (IsHarnessDriving())	// key state is global; a sweep must not see keys typed into other windows
		return;
#endif
	if (input.IsKeyDown(MC::Key::Num1))
		mIsWireframe = true;
	else
		mIsWireframe = false;
	if (input.IsKeyDown(MC::Key::Shift))
		mMainCamera->doubleSpeed = true;
	else
		mMainCamera->doubleSpeed = false;
	// Each key is polled before the focus test, so its "pressed since the last poll" bit is
	// read and cleared every frame, focused or not.
	if (input.IsKeyDown(MC::Key::W, true) && input.KeyboardEnabled()) {
		mMainCamera->Walk(10.0f * dt);
		mCameraDirty = true;
	}
	if (input.IsKeyDown(MC::Key::S, true) && input.KeyboardEnabled()) {
		mMainCamera->Walk(-10.0f * dt);
		mCameraDirty = true;
	}
	if (input.IsKeyDown(MC::Key::A, true) && input.KeyboardEnabled()) {
		mMainCamera->Strafe(-10.0f * dt);
		mCameraDirty = true;
	}
	if (input.IsKeyDown(MC::Key::D, true) && input.KeyboardEnabled()) {
		mMainCamera->Strafe(10.0f * dt);
		mCameraDirty = true;
	}
}
