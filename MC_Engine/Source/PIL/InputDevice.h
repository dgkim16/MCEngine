#pragma once
#include <cstdint>
#include <optional>

namespace MC {

// @brief	Engine key codes, the same on every OS. The PIL's OS file maps native key codes
// @brief	(Windows: virtual-key codes) to and from these.
enum class Key : std::uint8_t {
	Unknown,	// a native key with no engine code
	A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
	Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
	Escape, Enter, Tab, Space, Backspace, Delete, Insert, Home, End, PageUp, PageDown,
	Left, Right, Up, Down,
	Shift, Control, Alt,
	F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
	Count	// the number of codes; not a key
};

enum class MouseButton : std::uint8_t { Left, Right, Middle, Count };

// @brief	One input event, decoded from the OS by PIL::DecodeInputMessage.
struct InputEvent {
	enum class Type { KeyDown, KeyUp, MouseDown, MouseUp, MouseMove, MouseWheel };

	Type type = Type::MouseMove;
	Key key = Key::Unknown;	// KeyDown, KeyUp
	bool repeat = false;	// KeyDown: an auto-repeat while the key is held
	MouseButton button = MouseButton::Left;	// MouseDown, MouseUp
	// Every mouse event except MouseWheel: the button state after the event, and the cursor
	// position in client-area pixels.
	bool buttonsDown[static_cast<int>(MouseButton::Count)] = {};
	int x = 0;
	int y = 0;
	float wheel = 0.0f;	// MouseWheel: notches, positive away from the user
};

namespace PIL {

// @brief	Maps a native key code (Windows: a virtual-key code) to the engine's Key.
// @brief	Returns Key::Unknown for keys without an engine code.
Key KeyFromNative(std::uintptr_t nativeKey);

// @brief	Polls the keyboard at this instant (Windows: GetAsyncKeyState), regardless of
// @brief	which window has focus. orPressedSinceLastPoll also counts a press made since the
// @brief	previous poll of that key, so a quick tap between two frames is not missed.
bool PollKeyDown(Key key, bool orPressedSinceLastPoll);

// @brief	Decodes one native window event (Windows: the window procedure's message, wParam
// @brief	and lParam) into an InputEvent. Returns nullopt for events that are not input.
std::optional<InputEvent> DecodeInputMessage(unsigned message, std::uintptr_t wParam, std::intptr_t lParam);

}  // namespace PIL
}  // namespace MC
