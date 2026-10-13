#include "PIL/InputDevice.h"
#include <windows.h>
#include <windowsx.h>

namespace MC::PIL {

namespace {
// Keys whose virtual-key codes are not in a contiguous run. Letters, digits and F1-F12 are
// contiguous on both sides and are mapped by offset instead.
struct KeyMapping { Key key; int nativeKey; };
constexpr KeyMapping kMappedKeys[] = {
	{ Key::Escape, VK_ESCAPE }, { Key::Enter, VK_RETURN }, { Key::Tab, VK_TAB },
	{ Key::Space, VK_SPACE }, { Key::Backspace, VK_BACK }, { Key::Delete, VK_DELETE },
	{ Key::Insert, VK_INSERT }, { Key::Home, VK_HOME }, { Key::End, VK_END },
	{ Key::PageUp, VK_PRIOR }, { Key::PageDown, VK_NEXT },
	{ Key::Left, VK_LEFT }, { Key::Right, VK_RIGHT }, { Key::Up, VK_UP }, { Key::Down, VK_DOWN },
	{ Key::Shift, VK_SHIFT }, { Key::Control, VK_CONTROL }, { Key::Alt, VK_MENU },
};

constexpr int Offset(Key key, Key first) { return static_cast<int>(key) - static_cast<int>(first); }
constexpr Key KeyAt(Key first, int offset) { return static_cast<Key>(static_cast<int>(first) + offset); }

// The virtual-key code for a Key; 0 for Key::Unknown and Key::Count.
int NativeFromKey(Key key)
{
	if (key >= Key::A && key <= Key::Z) return 'A' + Offset(key, Key::A);
	if (key >= Key::Num0 && key <= Key::Num9) return '0' + Offset(key, Key::Num0);
	if (key >= Key::F1 && key <= Key::F12) return VK_F1 + Offset(key, Key::F1);
	for (const KeyMapping& m : kMappedKeys)
		if (m.key == key) return m.nativeKey;
	return 0;
}
}

Key KeyFromNative(std::uintptr_t nativeKey)
{
	if (nativeKey >= 'A' && nativeKey <= 'Z') return KeyAt(Key::A, static_cast<int>(nativeKey - 'A'));
	if (nativeKey >= '0' && nativeKey <= '9') return KeyAt(Key::Num0, static_cast<int>(nativeKey - '0'));
	if (nativeKey >= VK_F1 && nativeKey <= VK_F12) return KeyAt(Key::F1, static_cast<int>(nativeKey - VK_F1));
	for (const KeyMapping& m : kMappedKeys)
		if (static_cast<std::uintptr_t>(m.nativeKey) == nativeKey) return m.key;
	return Key::Unknown;
}

bool PollKeyDown(Key key, bool orPressedSinceLastPoll)
{
	const int nativeKey = NativeFromKey(key);
	if (nativeKey == 0)
		return false;
	// 0x8000: the key is down now. 0x0001: it was pressed since the previous call.
	const int mask = orPressedSinceLastPoll ? 0x8001 : 0x8000;
	return (GetAsyncKeyState(nativeKey) & mask) != 0;
}

std::optional<InputEvent> DecodeInputMessage(unsigned message, std::uintptr_t wParam, std::intptr_t lParam)
{
	InputEvent e;
	switch (message)
	{
	case WM_KEYDOWN:	// wParam is the virtual-key code; bit 30 of lParam is set on an auto-repeat
		e.type = InputEvent::Type::KeyDown;
		e.key = KeyFromNative(wParam);
		e.repeat = (lParam & (1 << 30)) != 0;
		return e;
	case WM_KEYUP:	// a key-up never repeats
		e.type = InputEvent::Type::KeyUp;
		e.key = KeyFromNative(wParam);
		return e;
	case WM_MOUSEWHEEL:	// lParam holds screen coordinates here, so no position is decoded
		e.type = InputEvent::Type::MouseWheel;
		e.wheel = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
		return e;
	case WM_LBUTTONDOWN: e.type = InputEvent::Type::MouseDown; e.button = MouseButton::Left; break;
	case WM_RBUTTONDOWN: e.type = InputEvent::Type::MouseDown; e.button = MouseButton::Right; break;
	case WM_MBUTTONDOWN: e.type = InputEvent::Type::MouseDown; e.button = MouseButton::Middle; break;
	case WM_LBUTTONUP:   e.type = InputEvent::Type::MouseUp;   e.button = MouseButton::Left; break;
	case WM_RBUTTONUP:   e.type = InputEvent::Type::MouseUp;   e.button = MouseButton::Right; break;
	case WM_MBUTTONUP:   e.type = InputEvent::Type::MouseUp;   e.button = MouseButton::Middle; break;
	case WM_MOUSEMOVE:   e.type = InputEvent::Type::MouseMove; break;
	default:
		return std::nullopt;
	}
	// Every mouse message above carries the button state after the event in wParam, and the
	// client-area position in lParam (GET_X/Y_LPARAM keep the sign, which LOWORD/HIWORD lose).
	e.buttonsDown[static_cast<int>(MouseButton::Left)] = (wParam & MK_LBUTTON) != 0;
	e.buttonsDown[static_cast<int>(MouseButton::Right)] = (wParam & MK_RBUTTON) != 0;
	e.buttonsDown[static_cast<int>(MouseButton::Middle)] = (wParam & MK_MBUTTON) != 0;
	e.x = GET_X_LPARAM(lParam);
	e.y = GET_Y_LPARAM(lParam);
	return e;
}

}  // namespace MC::PIL
