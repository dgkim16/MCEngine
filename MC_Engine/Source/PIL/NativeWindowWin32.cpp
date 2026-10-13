#include "PIL/NativeWindow.h"
#include <windows.h>
#include <stdexcept>

namespace MC::PIL {

namespace {
// kWindowClass used in three places
// Linux have no window class, so this gets defined here, not in header
// 1. RegisterClass (wc.lpszClassName) : registers window class (template) under that name
// 2. CreateWindow		finds class (template) by name
// 3. UnregisterClass	removes (template) by name
const wchar_t* const kWindowClass = L"MainWnd";

} // unnamed namespace

// ============================================================================================
// ======== How PumpOneEvent() (EventPumpWin32.cpp) reaches WindowProcedure::Proc() ===========
// ============================================================================================
// NativeWindow ctor : wc.lpfnWndProc = WindowProcedure::Proc;
// when PumpOneEvent calls DispatchMessage(&msg),
// it uses msg.hwnd to find the window object in which the msg is adressed to.
// then looks for that hwnd's lpfnWndProc, which we define as WindowProcedure::Proc in NativeWindow's ctor
// so DispatcheMessage(&msg) calls: Proc(msg.hwnd, msg.message, msg.wParam, msg.lParam);
// wParam & lParam aren't msg's payload, but filled-in values by windows when posting msg
// meaning differ depending on message
// WM_KEYDOWN: wParam is the virtual-key code ('W', VK_ESCAPE). lParam packs the repeat count, the scan code and bit 30 ("the key was already down", meaning an auto-repeat).
// WM_SIZE: wParam says how the window was resized(minimized, maximized, restored).lParam holds the new client width in its low 16 bits and the height in its high 16 bits.
// WM_NCCREATE : lParam points to the CREATESTRUCTW that carries this.

// ============================================================================================
// ====== How NativeWindow()'s CreateWindow(...,this) reaches WindowProcedure::Proc() ===============
// ============================================================================================
// calling CreateWindow(...) results in windows calling Proc() multiple times with different message.
// 1. WM_GETMINMAXINFO	usr-defined slots still empty. so Proc takes DefWindowProcW branch; if(!window)
// 2. WM_NCCREATE		lParam points to the CREATESTRUCTW carrying this; Proc() stores & forwards message to HandleMessage. If the procedure returns FALSE here, creation is cancelled and CreateWindow returns NULL.
// 3. WM_NCCALCSIZE		reach NativeWindow through the stored pointer.
// 4. WM_CREATE			reach NativeWindow through the stored pointer.
// NC means Non-Client, therefore Title bar & Borders, and are crated first

struct WindowProcedure { // becomes friend of NativeWindow. thus can call its HandleMessage()
	static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
	{
		if (message == WM_NCCREATE) { // Win32's 2nd call to Proc via CreateWindow(,this); message = WM_NCCREATE
			const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
			// lParam == int-typed, CREATESTRUCTW address
			SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
			// create->lpCreateParams == last argument provided to CreateWindow(,this), which is 'this' == pointer to that NativeWindow object (instance)
			// therefore create->lpCreateParams is void*
			// GWLP_USERDATA: names a pointer-sized storage slot that every window has and that Windows never uses itself. It's reserved for the application.
			// SetWindowLongPtrW(hwnd,ptr_sized_storage_slot, void*): write value into that slot of window. API stores int, so cast void* to pointer-sized integer LONG_PTR
			// Result: This window itself carries pointer to its NativeWindow object
			// how to access NativeWindow object tied to this window : GetWindowLongPtrW(hwnd, GWLP_USERDATA)
			// The handle is known from here on, before CreateWindow returns: events raised during
			// creation (a DisplayChanged from WM_MOVE, say) may ask the NativeWindow about itself.
			static_cast<NativeWindow*>(create->lpCreateParams)->mHandle = hwnd;
		}
		auto* window = reinterpret_cast<NativeWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
		if (!window)	// Win32's 1st call to Proc via CreateWindow(,this); message = WM_GETMINMAXINFO
			return DefWindowProcW(hwnd, message, wParam, lParam);
		return window->HandleMessage(hwnd, message, wParam, lParam);
	}
	// ============================================================================================
	// ===== Reason to use SetWindowLongPtrW and GetWindowLongPtrW to instead of storing ptr ======
	// ============================================================================================
	// 1. Proc is a static function. Windows call Proc with no object.
	// 2. Storing the pointer in the window's user data is the standard Win32 practice.
	//	  Microsoft's own tutorial, "Managing Application State", stores it at WM_NCCREATE/WM_CREATE
	//    and reads it back with GetWindowLongPtr.
	// 3. Each window maps to its own object.
	//	  A static pointer allows exactly one NativeWindow.
	//    A second window, or a test that creates a temporary one, would silently overwrite it.
	// 4. The pointer's lifetime matches the window's.
	//    The pointer lives exactly as long as the window, and destructor clears it.
	//    A static pointer has to be cleared by hand, or it dangles.
	// 5. No hidden shared state
	//    Nothing outside the window can reach the object through a global.
};

NativeWindow::NativeWindow(void* instance, const std::wstring& title, int clientWidth, int clientHeight, int iconId, EventHandler handler)
	: mInstance(instance), mClientWidth(clientWidth), mClientHeight(clientHeight), mHandler(std::move(handler))
{
	// A handle to the loaded .exe module (!= the 'window', as 1 exe can have multiple windows)
	const HINSTANCE hInstance = static_cast<HINSTANCE>(instance);

	// define template for window class shared by every window created from it. we will create just 1.
	WNDCLASS wc;
	wc.style = CS_HREDRAW | CS_VREDRAW;	// repaints the whole window when its width or height changes.
	wc.lpfnWndProc = WindowProcedure::Proc; // runs for messages dispatched from the queue and for messages sent directly
	wc.cbClsExtra = 0;	// extra storage bytes per class. Built-in GWLP_USERDATA slot holds NativeWindow*, so 0
	wc.cbWndExtra = 0;	// extra storage bytes per window. Built-in GWLP_USERDATA slot holds NativeWindow*, so 0
	wc.hInstance = hInstance; // owning module (.exe) of windows that will be created from this template
	wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(iconId));

	wc.hCursor = LoadCursor(0, IDC_ARROW);	// 0 measn 'system cursor'
	wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH); // NULL_BRUSH means never erase BG. (this avoids white flash on resize)
	wc.lpszMenuName = 0;	// no menu bar
	wc.lpszClassName = kWindowClass;	// class name used by CreateWindow to find this template

	if (!RegisterClass(&wc))	// hand out template to Windows
		throw std::runtime_error("RegisterClass Failed.");

	// Scale the client size, given at 100% scaling, by the system DPI (96 = 100%), so the
	// window has the same physical size under any display scaling.
	const UINT  sysDpi = GetDpiForSystem();
	const float dpiScale = (sysDpi > 0) ? sysDpi / 96.0f : 1.0f;
	mClientWidth = static_cast<int>(mClientWidth * dpiScale);
	mClientHeight = static_cast<int>(mClientHeight * dpiScale);

	// Compute window rectangle dimensions based on requested client area dimensions.
	RECT R = { 0, 0, mClientWidth, mClientHeight };
	AdjustWindowRect(&R, WS_OVERLAPPEDWINDOW, false);
	int width = R.right - R.left;
	int height = R.bottom - R.top;

	// CreateWindow(....) results in calling WindowProcedure::Proc() with message = WM_NCCREATE.
	// The last argument, `this`, reaches WindowProcedure::Proc with WM_NCCREATE.
	HWND hwnd = CreateWindow(kWindowClass, title.c_str(),
		WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, width, height, 0, 0, hInstance, this);
	if (!hwnd)
	{
		UnregisterClass(kWindowClass, hInstance);
		throw std::runtime_error("CreateWindow Failed.");
	}
	mHandle = hwnd;

	ShowWindow(hwnd, SW_SHOW);
	UpdateWindow(hwnd);
}

NativeWindow::~NativeWindow()
{
	// must detach this instance from the window it was linked to by Proc()'s SetWindowLongPtrW
	if (mHandle)
	{
		// 1. detach (overwrite) link using SetWindowLongPtrW with 0
		SetWindowLongPtrW(static_cast<HWND>(mHandle), GWLP_USERDATA, 0);
		// 2. destory window
		DestroyWindow(static_cast<HWND>(mHandle));
		// DestroyWindow() sends WM_DESTROY and WM_NCDESTROY synchronously
		// because slot is cleared by detachment, Proc() sends the two messages to DefWindowProcW()
	}
	// remove HINSTANCE template from windows (unregister it)
	UnregisterClass(kWindowClass, static_cast<HINSTANCE>(mInstance));
}

void NativeWindow::SetTitle(const std::wstring& title)
{
	if (mHandle)
		SetWindowTextW(static_cast<HWND>(mHandle), title.c_str());
}

void NativeWindow::RequestClose()
{
	if (mHandle)
		PostMessage(static_cast<HWND>(mHandle), WM_CLOSE, 0, 0);
	// put WM_CLOSE into queue (same effect as pressing X)
}

// ============================================================================================
// ======== How NativeWindow::HandleMessage() handles WM_DESTROY and WM_NCDESTROY =============
// ============================================================================================
// 1. X button, alt f4, or RequestClose() produce WM_CLOSE(). NEVER when destructor destroys window
// 2. HandleMessage reads WM_CLOSE(), and breaks, so DefWindowProc calls DestroyWindow
// 3. DestroyWindow sends WM_DESTROY, then WM_NCDESTROY straight to Proc before it returns.
// 4. User data slot still holds NativeWindow*, so Proc forwards both WM_DESTROY and WM_NCDESTROY to HandleMessage
std::intptr_t NativeWindow::HandleMessage(void* window, unsigned message, std::uintptr_t wParam, std::intptr_t lParam)
{
	// std::function, if no hook installed, returns false when used as condition
	const bool consumed = mNativeHook && mNativeHook(window, message, wParam, lParam);
	if (consumed && message != WM_DESTROY && message != WM_NCDESTROY)
		return 1;

	// The window tracks its own client size for every WM_SIZE, before anyone hears about it.
	if (message == WM_SIZE) {
		mClientWidth = LOWORD(lParam);
		mClientHeight = HIWORD(lParam);
	}

	// Decoding is DecodeWindowMessage's job (WindowEventWin32.cpp); this procedure raises the
	// event, then answers Windows with the result each message expects.
	bool handlerConsumed = false;
	if (const auto event = DecodeWindowMessage(message, wParam, lParam))
		handlerConsumed = Raise(*event);

	switch (message) {
	// ============= Closing =============
	case(WM_CLOSE):
		if (handlerConsumed)
			return 0;	// the handler keeps the window open (confirm-close)
		break;			// DefWindowProc destroys the window
	case(WM_DESTROY):
		PostQuitMessage(0);	// PumpOneEvent reports it as PumpResult::Quit
		return 0;
	case(WM_NCDESTROY):
		mHandle = nullptr;
		break;
	// ============= Messages that need no default handling =============
	case(WM_MOVE):
	case(WM_DISPLAYCHANGE):
	case(WM_LBUTTONDOWN):
	case(WM_MBUTTONDOWN):
	case(WM_RBUTTONDOWN):
	case(WM_LBUTTONUP):
	case(WM_MBUTTONUP):
	case(WM_RBUTTONUP):
	case(WM_MOUSEMOVE):
	case(WM_MOUSEWHEEL):
		return 0;
	// ============= ETC =============
	case(WM_MENUCHAR):
		return MAKELRESULT(0, MNC_CLOSE);	// don't beep on Alt+Enter
	case(WM_GETMINMAXINFO):
	{
		auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
		info->ptMinTrackSize.x = 200;	// the smallest width the user can drag the window to
		info->ptMinTrackSize.y = 200;	// the smallest height
		return 0;
	}
	default:
		break;
	}
	return DefWindowProcW(static_cast<HWND>(window), message, wParam, lParam);
}

}  // namespace MC::PIL
