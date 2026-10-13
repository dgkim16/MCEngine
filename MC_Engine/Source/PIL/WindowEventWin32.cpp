#include "PIL/WindowEvent.h"
#include <windows.h>

namespace MC::PIL {

std::optional<WindowEvent> DecodeWindowMessage(unsigned message, std::uintptr_t wParam, std::intptr_t lParam)
{
	using Type = WindowEvent::Type;
	WindowEvent event;
	switch (message)
	{
	// ============= PAUSING =============
	case WM_ACTIVATE:
		event.type = (LOWORD(wParam) == WA_INACTIVE) ? Type::Deactivated : Type::Activated;
		return event;
	case WM_ENTERSIZEMOVE:
		event.type = Type::EnterSizeMove;
		return event;
	case WM_EXITSIZEMOVE:
		event.type = Type::ExitSizeMove;
		return event;
	// ============= Resizing =============
	case WM_SIZE:	// wParam: how the window was resized; lParam: the new client width (low 16 bits) and height (high 16 bits)
		event.clientWidth = LOWORD(lParam);
		event.clientHeight = HIWORD(lParam);
		switch (wParam) {
		case SIZE_MINIMIZED: event.type = Type::Minimized; return event;
		case SIZE_MAXIMIZED: event.type = Type::Maximized; return event;
		case SIZE_RESTORED:  event.type = Type::Restored;  return event;
		}
		return std::nullopt;	// SIZE_MAXSHOW / SIZE_MAXHIDE concern other windows
	case WM_MOVE:
	case WM_DISPLAYCHANGE:
		event.type = Type::DisplayChanged;
		return event;
	// ============= Closing =============
	case WM_CLOSE:
		event.type = Type::CloseRequested;
		return event;
	case WM_DESTROY:
		event.type = Type::Destroyed;
		return event;
	// Keyboard focus moved to another window: its key-ups and button-ups will not come here.
	case WM_KILLFOCUS:
		event.type = Type::FocusLost;
		return event;
	}

	// ============= INPUT =============
	// Decoded into an engine InputEvent (InputDevice); PlatformSystem hands it to Input.
	if (const auto input = DecodeInputMessage(message, wParam, lParam))
	{
		event.type = Type::Input;
		event.input = *input;
		return event;
	}
	return std::nullopt;
}

}  // namespace MC::PIL
