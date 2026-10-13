#pragma once
#include "PIL/InputDevice.h"
#include <bitset>
#include <cstddef>

namespace MC {

// @brief	This frame's keyboard and mouse state, plus the gameplay gates the editor sets.
// @brief	OS-independent: the PIL decodes OS events into InputEvents and polls the keyboard;
// @brief	Input only keeps state. PlatformSystem feeds it events as they arrive and starts
// @brief	each frame from Tick, so what came from events holds still for the whole frame.
class Input {
public:
	Input() = default;
	~Input() = default;
	Input(const Input& rhs) = delete;
	Input(Input&& rhs) = delete;
	Input& operator=(Input&& rhs) = delete;
	Input& operator=(const Input& rhs) = delete;

	using MouseButton = MC::MouseButton;
	// ---- Keyboard ----
	// @brief	polls the keyboard right now (currently down)
	// @brief	True while the key is down. orPressedSinceLastPoll also counts a press made
	// @brief	between two polls (WASD uses it).
	bool IsKeyDown(Key key, bool orPressedSinceLastPoll = false) const;
	// @brief	Reads this frame's events (triggered; up to down)
	// @brief	True for one frame after the key went from up to down. Auto-repeat does not count.
	bool IsKeyTriggered(Key key) const;
	// @brief	Reads this frame's events (released; down to up)
	// @brief	True for one frame after the key went from down to up.
	bool IsKeyReleased(Key key) const;

	// ---- Mouse, in client-area pixels ----
	int MouseX() const { return mMouseX; }
	int MouseY() const { return mMouseY; }
	// @brief	Mouse movement during the last frame.
	int MouseDeltaX() const { return mFrame.deltaX; }
	int MouseDeltaY() const { return mFrame.deltaY; }
	bool IsMouseDown(MouseButton button) const { return mMouseDown[static_cast<int>(button)]; }
	// @brief	True for one frame after the button went down.
	bool WasMousePressed(MouseButton button) const { return mFrame.mousePressed[static_cast<int>(button)]; }
	// @brief	Wheel notches during the last frame; positive away from the user.
	float WheelDelta() const { return mFrame.wheel; }

	// ---- Gates ----
	// @brief	The editor sets these each frame from viewport focus (keyboard) and hover (mouse).
	// @brief	Without an editor both stay true. Gameplay input checks them.
	void SetGates(bool keyboard, bool mouse) { mKeyboardEnabled = keyboard; mMouseEnabled = mouse; }
	bool KeyboardEnabled() const { return mKeyboardEnabled; }
	bool MouseEnabled() const { return mMouseEnabled; }

private:
	friend class PlatformSystem;
	// @brief	PlatformSystem passes every decoded input event here (PIL::DecodeInputMessage).
	void OnEvent(const InputEvent& event);
	// @brief	PlatformSystem::Tick: publishes what arrived since the previous frame.
	void NewFrame() { mFrame = mPending; mPending = Events{}; }
	// @brief	PlatformSystem's HandleMessage calls this when the window loses keyboard focus.
	// @brief	The key-ups and button-ups then go to another window, so every key still down
	// @brief	gets a synthetic release this frame, and every mouse button is set up.
	void OnFocusLost();

	static constexpr int kMouseButtons = static_cast<int>(MouseButton::Count);
	static constexpr std::size_t kKeys = static_cast<std::size_t>(Key::Count);

	// What arrived during one frame. Events fill mPending; NewFrame publishes it as mFrame.
	struct Events {
		std::bitset<kKeys> keysTriggered;
		std::bitset<kKeys> keysReleased;
		bool mousePressed[kMouseButtons] = {};
		int deltaX = 0;
		int deltaY = 0;
		float wheel = 0.0f;
	};
	Events mFrame;
	Events mPending;

	// Keys down according to the key events: set by a key-down, cleared by its key-up.
	// OnFocusLost releases these, because their key-ups will not arrive.
	std::bitset<kKeys> mKeysDown;

	// Current state, updated by every mouse message.
	bool mMouseDown[kMouseButtons] = {};
	bool mHasMousePosition = false;	// no delta before the first position is known
	int mMouseX = 0;
	int mMouseY = 0;

	bool mKeyboardEnabled = true;
	bool mMouseEnabled = true;

};

} // namespace MC