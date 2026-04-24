#include "MCEngine.h"
#include <WindowsX.h>
#include "MC_Picker.h"

void MCEngine::OnMouseDown(WPARAM btnState, int x, int y)
{
	if ((btnState & MK_LBUTTON) != 0) {
		if (mSceneImageHovered) {
			MC_Picker& picker = MC_Picker::GetPicker(*this);
			picker.PickRenderItem(GetSceneMousePos(), mAllRitems);
		}
	}
	mLastMousePos.x = x;
	mLastMousePos.y = y;
	SetCapture(mhMainWnd);
}

void MCEngine::OnMouseUp(WPARAM btnState, int x, int y)
{
	ReleaseCapture();
}

void MCEngine::OnMouseMove(WPARAM btnState, int x, int y)
{
	if (!mSceneImageHovered)
	{
		mLastMousePos.x = x;
		mLastMousePos.y = y;
		return;
	}

	if ((btnState & MK_LBUTTON) != 0)
	{
		// Make each pixel correspond to a quarter of a degree.

		// Update angles based on input to orbit camera around box.
		// mTheta += dx;
		// mPhi += dy;

		// Restrict the angle mPhi.
		// mPhi = MathHelper::Clamp(mPhi, 0.1f, MathHelper::Pi - 0.1f);
	}
	else if ((btnState & MK_MBUTTON) != 0)
	{
		// Make each pixel correspond to 0.005 unit in the scene.
		float dx = 0.005f * static_cast<float>(x - mLastMousePos.x);
		float dy = 0.005f * static_cast<float>(y - mLastMousePos.y);
		mMainCamera->Strafe(-5.0f * (dx));
		mMainCamera->Climb(5.0f * (dy));
		mCameraDirty = true;
	}
	else if ((btnState & MK_RBUTTON) != 0)
	{
		float dx = XMConvertToRadians(0.25f * static_cast<float>(x - mLastMousePos.x));
		float dy = XMConvertToRadians(0.25f * static_cast<float>(y - mLastMousePos.y));
		mMainCamera->Pitch(dy);
		mMainCamera->RotateY(dx);
		mCameraDirty = true;
	}

	mLastMousePos.x = x;
	mLastMousePos.y = y;
}
void MCEngine::OnMouseScroll(WPARAM btnState, float delta)
{
	if ((btnState & MK_RBUTTON) != 0) {

	}
	mMainCamera->Walk(2.0f * delta);
	mCameraDirty = true;

	// mMainCamera->Zoom(-delta*0.5f);
}
void MCEngine::OnKeyboardInput(const GameTimer& gt)
{
	if (GetAsyncKeyState('1') & 0x8000)
		mIsWireframe = true;
	else
		mIsWireframe = false;
	if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
		mMainCamera->doubleSpeed = true;
	else
		mMainCamera->doubleSpeed = false;
	const float dt = gt.DeltaTime();
	if (GetAsyncKeyState('W') & 0x8001 && mSceneImageSelected) {
		mMainCamera->Walk(10.0f * dt);
		mCameraDirty = true;
	}
	if (GetAsyncKeyState('S') & 0x8001 && mSceneImageSelected) {
		mMainCamera->Walk(-10.0f * dt);
		mCameraDirty = true;
	}
	if (GetAsyncKeyState('A') & 0x8001 && mSceneImageSelected) {
		mMainCamera->Strafe(-10.0f * dt);
		mCameraDirty = true;
	}
	if (GetAsyncKeyState('D') & 0x8001 && mSceneImageSelected) {
		mMainCamera->Strafe(10.0f * dt);
		mCameraDirty = true;
	}
}

