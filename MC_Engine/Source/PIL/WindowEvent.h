#pragma once
#include "PIL/InputDevice.h"
#include <cstdint>
#include <optional>

namespace MC::PIL {

// @brief	One window event, decoded from the OS by DecodeWindowMessage. PlatformSystem reacts
// @brief	to these; it never sees an OS message.
struct WindowEvent {
	enum class Type {
		Activated,		// the window became the active window
		Deactivated,	// another window became active
		Minimized,		// size changes: clientWidth/clientHeight hold the new client size
		Maximized,
		Restored,		// restored from minimized or maximized, or resized by code
		EnterSizeMove,	// the user started dragging the border or the title bar
		ExitSizeMove,	// the user let go
		CloseRequested,	// the close button, Alt+F4 or NativeWindow::RequestClose
		Destroyed,		// the window is being destroyed; the quit request follows
		FocusLost,		// keyboard focus moved to another window
		DisplayChanged,	// the window moved (perhaps to another display), or display settings changed
		Input			// a keyboard or mouse event, in input
	};

	Type type = Type::Input;
	int clientWidth = 0;
	int clientHeight = 0;
	InputEvent input;
};

// @brief	Decodes one native window message (Windows: the window procedure's message, wParam
// @brief	and lParam) into a WindowEvent. Returns nullopt for messages that are not one.
// @brief	Pure: it only reads its arguments.
std::optional<WindowEvent> DecodeWindowMessage(unsigned message, std::uintptr_t wParam, std::intptr_t lParam);

}  // namespace MC::PIL
