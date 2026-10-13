#include "Platform/Input.h"

namespace MC {

namespace {
// Bit index of a key in the key bitsets; Key::Unknown is never stored.
constexpr std::size_t KeyIndex(Key key) { return static_cast<std::size_t>(key); }
constexpr bool IsStorableKey(Key key) { return key != Key::Unknown && key < Key::Count; }
}

bool Input::IsKeyDown(Key key, bool orPressedSinceLastPoll) const
{
	return PIL::PollKeyDown(key, orPressedSinceLastPoll);
}

bool Input::IsKeyTriggered(Key key) const
{
	return IsStorableKey(key) && mFrame.keysTriggered[KeyIndex(key)];
}

bool Input::IsKeyReleased(Key key) const
{
	return IsStorableKey(key) && mFrame.keysReleased[KeyIndex(key)];
}

void Input::OnEvent(const InputEvent& event)
{
	using Type = InputEvent::Type;

	switch (event.type)
	{
	case Type::KeyDown:
		if (!IsStorableKey(event.key)) return;
		// ========= TRIGGERED ==========
		if (!event.repeat)
			mPending.keysTriggered.set(KeyIndex(event.key));	// a press, not "holding down"
		mKeysDown.set(KeyIndex(event.key));
		return;
	case Type::KeyUp:
		if (!IsStorableKey(event.key)) return;
		// ========= RELEASED ==========
		mPending.keysReleased.set(KeyIndex(event.key));
		mKeysDown.reset(KeyIndex(event.key));
		return;
	case Type::MouseWheel:
		mPending.wheel += event.wheel;
		return;
	case Type::MouseDown:
		mPending.mousePressed[static_cast<int>(event.button)] = true;
		break;
	case Type::MouseUp:
	case Type::MouseMove:
		break;
	}

	// Every mouse event but the wheel carries the button state after the event.
	for (int b = 0; b < kMouseButtons; ++b)
		mMouseDown[b] = event.buttonsDown[b];

	if (mHasMousePosition) {
		mPending.deltaX += event.x - mMouseX;
		mPending.deltaY += event.y - mMouseY;
	}
	mMouseX = event.x;
	mMouseY = event.y;
	mHasMousePosition = true;
}

void Input::OnFocusLost()
{
	// The key-ups for keys still down go to the window that now has focus: release them here.
	mPending.keysReleased |= mKeysDown;
	mKeysDown.reset();

	// The same goes for mouse buttons released outside the window.
	for (bool& down : mMouseDown)
		down = false;
	// The next position comes from wherever the cursor is when focus returns; a delta from
	// the last position seen here would be a jump.
	mHasMousePosition = false;
}

} // namespace MC
