// 26 april 2015
#include "uipriv_windows.h"

#define containerClass L"libui_uiContainerClass"

HWND initialParent;

struct container {
	HWND hwnd;
	uiContainer *parent;
	int hidden;
	HBRUSH brush;
};

static HWND realParent(HWND hwnd)
{
	HWND parent;
	int class;

	parent = hwnd;
	for (;;) {
		parent = GetAncestor(parent, GA_PARENT);
		// skip groupboxes; they're (supposed to be) transparent
		// skip uiContainers; they don't draw anything
		class = windowClassOf(parent, L"button", containerClass, NULL);
		if (class != 0 && class != 1)
			break;
	}
	return parent;
}

struct parentDraw {
	HDC cdc;
	HBITMAP bitmap;
	HBITMAP prevbitmap;
};

static void parentDraw(HDC dc, HWND parent, struct parentDraw *pd)
{
	RECT r;

	if (GetClientRect(parent, &r) == 0)
		logLastError("error getting parent's client rect in parentDraw()");
	pd->cdc = CreateCompatibleDC(dc);
	if (pd->cdc == NULL)
		logLastError("error creating compatible DC in parentDraw()");
	pd->bitmap = CreateCompatibleBitmap(dc, r.right - r.left, r.bottom - r.top);
	if (pd->bitmap == NULL)
		logLastError("error creating compatible bitmap in parentDraw()");
	pd->prevbitmap = SelectObject(pd->cdc, pd->bitmap);
	if (pd->prevbitmap == NULL)
		logLastError("error selecting bitmap into compatible DC in parentDraw()");
	SendMessageW(parent, WM_PRINTCLIENT, (WPARAM) (pd->cdc), PRF_CLIENT);
}

static void endParentDraw(struct parentDraw *pd)
{
	if (SelectObject(pd->cdc, pd->prevbitmap) != pd->bitmap)
		logLastError("error selecting previous bitmap back into compatible DC in endParentDraw()");
	if (DeleteObject(pd->bitmap) == 0)
		logLastError("error deleting compatible bitmap in endParentDraw()");
	if (DeleteDC(pd->cdc) == 0)
		logLastError("error deleting compatible DC in endParentDraw()");
}

// see http://www.codeproject.com/Articles/5978/Correctly-drawn-themed-dialogs-in-WinXP
static HBRUSH getControlBackgroundBrush(HWND hwnd, HDC dc)
{
	HWND parent;
	RECT hwndScreenRect;
	struct parentDraw pd;
	HBRUSH brush;

	parent = realParent(hwnd);

	parentDraw(dc, parent, &pd);
	brush = CreatePatternBrush(pd.bitmap);
	if (brush == NULL)
		logLastError("error creating pattern brush in getControlBackgroundBrush()");
	endParentDraw(&pd);

	// now figure out where the control is relative to the parent so we can align the brush properly
	if (GetWindowRect(hwnd, &hwndScreenRect) == 0)
		logLastError("error getting control window rect in getControlBackgroundBrush()");
	// this will be in screen coordinates; convert to parent coordinates
	mapWindowRect(NULL, parent, &hwndScreenRect);
	if (SetBrushOrgEx(dc, -hwndScreenRect.left, -hwndScreenRect.top, NULL) == 0)
		logLastError("error setting brush origin in getControlBackgroundBrush()");

	return brush;
}

static void paintContainerBackground(HWND hwnd, HDC dc, RECT *paintRect)
{
	HWND parent;
	RECT paintRectParent;
	struct parentDraw pd;

	parent = realParent(hwnd);
	parentDraw(dc, parent, &pd);

	paintRectParent = *paintRect;
	mapWindowRect(hwnd, parent, &paintRectParent);
	if (BitBlt(dc, paintRect->left, paintRect->top, paintRect->right - paintRect->left, paintRect->bottom - paintRect->top,
		pd.cdc, paintRectParent.left, paintRectParent.top,
		SRCCOPY) == 0)
		logLastError("error drawing parent background over uiContainer in paintContainerBackground()");

	endParentDraw(&pd);
}

// from https://msdn.microsoft.com/en-us/library/windows/desktop/dn742486.aspx#sizingandspacing and https://msdn.microsoft.com/en-us/library/windows/desktop/bb226818%28v=vs.85%29.aspx
// this X value is really only for buttons but I don't see a better one :/
#define winXPadding 4
#define winYPadding 4

// abort the resize if something fails and we don't have what we need to do a resize
static HRESULT resize(uiContainer *cc, RECT *r)
{
	struct container *c = (struct container *) (uiControl(cc)->Internal);
	uiSizing d;
	uiSizingSys sys;
	HDC dc;
	HFONT prevfont;
	TEXTMETRICW tm;
	SIZE size;

	dc = GetDC(c->hwnd);
	if (dc == NULL)
		return logLastError("error getting DC in resize()");
	prevfont = (HFONT) SelectObject(dc, hMessageFont);
	if (prevfont == NULL)
		return logLastError("error loading control font into device context in resize()");

	ZeroMemory(&tm, sizeof (TEXTMETRICW));
	if (GetTextMetricsW(dc, &tm) == 0)
		return logLastError("error getting text metrics in resize()");
	if (GetTextExtentPoint32W(dc, L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz", 52, &size) == 0)
		return logLastError("error getting text extent point in resize()");

	sys.BaseX = (int) ((size.cx / 26 + 1) / 2);
	sys.BaseY = (int) tm.tmHeight;
	sys.InternalLeading = tm.tmInternalLeading;

	if (SelectObject(dc, prevfont) != hMessageFont)
		return logLastError("error restoring previous font into device context in resize()");
	if (ReleaseDC(c->hwnd, dc) == 0)
		return logLastError("error releasing DC in resize()");

	// the first control gets the topmost z-order and thus the first tab stop
	sys.InsertAfter = HWND_TOP;

	d.XPadding = uiWindowsDlgUnitsToX(winXPadding, sys.BaseX);
	d.YPadding = uiWindowsDlgUnitsToY(winYPadding, sys.BaseY);
	d.Sys = &sys;
	uiContainerResizeChildren(cc, r->left, r->top, r->right - r->left, r->bottom - r->top, &d);
	return S_OK;
}

static LRESULT CALLBACK containerWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	uiContainer *cc;
	struct container *c;
	CREATESTRUCTW *cs = (CREATESTRUCTW *) lParam;
	HWND control;
	NMHDR *nm = (NMHDR *) lParam;
	RECT r;
	HDC dc;
	PAINTSTRUCT ps;

	cc = uiContainer(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	if (cc == NULL)
		if (uMsg == WM_NCCREATE)
			SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR) (cs->lpCreateParams));
		// DO NOT RETURN DEFWINDOWPROC() HERE
		// see the next block of comments as to why
		// instead, we simply check if c == NULL again later

	switch (uMsg) {
	// these must always be run, even on the initial parent
	// why? http://blogs.msdn.com/b/oldnewthing/archive/2010/03/16/9979112.aspx
	case WM_COMMAND:
		// bounce back to the control in question
		// except if to the initial parent, in which case act as if the message was ignored
		control = (HWND) lParam;
		if (control != NULL && IsChild(initialParent, control) == 0)
			return SendMessageW(control, msgCOMMAND, wParam, lParam);
		break;			// fall through to DefWindowProcW()
	case WM_NOTIFY:
		// same as WM_COMMAND
		control = nm->hwndFrom;
		if (control != NULL && IsChild(initialParent, control) == 0)
			return SendMessageW(control, msgNOTIFY, wParam, lParam);
		break;

	// these are only run if c is not NULL
	case WM_CTLCOLORSTATIC:
	case WM_CTLCOLORBTN:
		if (cc == NULL)
			break;
		c = (struct container *) (uiControl(cc)->Internal);
		if (c->brush != NULL)
			if (DeleteObject(c->brush) == 0)
				logLastError("error deleting old background brush in containerWndProc()");
		if (SetBkMode((HDC) wParam, TRANSPARENT) == 0)
			logLastError("error setting transparent background mode to controls in containerWndProc()");
		c->brush = getControlBackgroundBrush((HWND) lParam, (HDC) wParam);
		return (LRESULT) (c->brush);
	case WM_PAINT:
		if (cc == NULL)
			break;
		c = (struct container *) (uiControl(cc)->Internal);
		dc = BeginPaint(c->hwnd, &ps);
		if (dc == NULL)
			logLastError("error beginning container paint in containerWndProc()");
		r = ps.rcPaint;
		paintContainerBackground(c->hwnd, dc, &r);
		EndPaint(c->hwnd, &ps);
		return 0;
	// tab controls use this to draw the background of the tab area
	case WM_PRINTCLIENT:
		if (cc == NULL)
			break;
		c = (struct container *) (uiControl(cc)->Internal);
		if (GetClientRect(c->hwnd, &r) == 0)
			logLastError("error getting client rect in containerWndProc()");
		paintContainerBackground(c->hwnd, (HDC) wParam, &r);
		return 0;
	case WM_ERASEBKGND:
		// avoid some flicker
		// we draw the whole update area anyway
		return 1;
	case msgUpdateChild:
		if (cc == NULL)
			break;
		c = (struct container *) (uiControl(cc)->Internal);
		if (GetClientRect(c->hwnd, &r) == 0)
			logLastError("error getting client rect for resize in containerWndProc()");
		resize(cc, &r);
		return 0;

	// and this is run only on the initial parent
	// this ensures that all end session handling is done in one place and only once
	case WM_QUERYENDSESSION:
	case msgConsoleEndSession:
		if (cc != NULL)
			break;
		// TODO block handler
		if (shouldQuit()) {
			uiQuit();
			return TRUE;
		}
		return FALSE;
	}

	return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

const char *initContainer(HICON hDefaultIcon, HCURSOR hDefaultCursor)
{
	WNDCLASSW wc;

	ZeroMemory(&wc, sizeof (WNDCLASSW));
	wc.lpszClassName = containerClass;
	wc.lpfnWndProc = containerWndProc;
	wc.hInstance = hInstance;
	wc.hIcon = hDefaultIcon;
	wc.hCursor = hDefaultCursor;
	wc.hbrBackground = (HBRUSH) (COLOR_BTNFACE + 1);
	if (RegisterClassW(&wc) == 0)
		return "registering uiContainer window class";

	initialParent = CreateWindowExW(0,
		containerClass, L"",
		WS_OVERLAPPEDWINDOW,
		0, 0,
		100, 100,
		NULL, NULL, hInstance, NULL);
	if (initialParent == NULL)
		return "creating initial parent window";

	// just to be safe, disable the initial parent so it can't be interacted with accidentally
	// if this causes issues for our controls, we can remove it
	EnableWindow(initialParent, FALSE);
	return NULL;
}

void uninitContainer(void)
{
	if (DestroyWindow(initialParent) == 0)
		logLastError("error destroying initial parent in uninitContainer()");
	if (UnregisterClassW(containerClass, hInstance) == 0)
		logLastError("error unregistering uiContainer window class in uninitContainer()");
}

// subclasses override this and call back here when all children are destroyed
static void containerDestroy(uiControl *cc)
{
	struct container *c = (struct container *) (cc->Internal);

	if (c->parent != NULL)
		complain("attempt to destroy uiContainer %p while it has a parent", cc);
	if (DestroyWindow(c->hwnd) == 0)
		logLastError("error destroying uiContainer window in containerDestroy()");
	uiFree(c);
}

static uintptr_t containerHandle(uiControl *cc)
{
	struct container *c = (struct container *) (cc->Internal);

	return (uintptr_t) (c->hwnd);
}

static void containerSetParent(uiControl *cc, uiContainer *parent)
{
	struct container *c = (struct container *) (cc->Internal);
	uiContainer *oldparent;
	HWND newparent;

	oldparent = c->parent;
	c->parent = parent;
	newparent = initialParent;
	if (c->parent != NULL)
		newparent = (HWND) uiControlHandle(uiControl(c->parent));
	if (SetParent(c->hwnd, newparent) == 0)
		logLastError("error changing uiContainer parent in containerSetParent()");
	if (oldparent != NULL)
		uiContainerUpdate(oldparent);
	if (c->parent != NULL)
		uiContainerUpdate(c->parent);
}

static void containerResize(uiControl *cc, intmax_t x, intmax_t y, intmax_t width, intmax_t height, uiSizing *d)
{
	struct container *c = (struct container *) (cc->Internal);

	moveAndReorderWindow(c->hwnd, d->Sys->InsertAfter, x, y, width, height);
	d->Sys->InsertAfter = c->hwnd;
	SendMessageW(c->hwnd, msgUpdateChild, 0, 0);
}

static int containerVisible(uiControl *cc)
{
	struct container *c = (struct container *) (cc->Internal);

	return !c->hidden;
}

static void containerShow(uiControl *cc)
{
	struct container *c = (struct container *) (cc->Internal);

	ShowWindow(c->hwnd, SW_SHOW);
	// hidden controls don't count in boxes and grids
	c->hidden = 0;
	if (c->parent != NULL)
		uiContainerUpdate(c->parent);
}

static void containerHide(uiControl *cc)
{
	struct container *c = (struct container *) (cc->Internal);

	ShowWindow(c->hwnd, SW_HIDE);
	c->hidden = 1;
	if (c->parent != NULL)
		uiContainerUpdate(c->parent);
}

static void containerEnable(uiControl *cc)
{
	struct container *c = (struct container *) (cc->Internal);
	uiControlSysFuncParams p;

	EnableWindow(c->hwnd, TRUE);
	p.Func = uiWindowsSysFuncContainerEnable;
	uiControlSysFunc(cc, &p);
}

static void containerDisable(uiControl *cc)
{
	struct container *c = (struct container *) (cc->Internal);
	uiControlSysFuncParams p;

	EnableWindow(c->hwnd, FALSE);
	p.Func = uiWindowsSysFuncContainerDisable;
	uiControlSysFunc(cc, &p);
}

static void containerUpdate(uiContainer *cc)
{
	struct container *c = (struct container *) (uiControl(cc)->Internal);

	SendMessageW(c->hwnd, msgUpdateChild, 0, 0);
}

void uiMakeContainer(uiContainer *cc)
{
	struct container *c;

	c = uiNew(struct container);

	c->hwnd = CreateWindowExW(WS_EX_CONTROLPARENT,
		containerClass, L"",
		WS_CHILD | WS_VISIBLE,
		0, 0,
		100, 100,
		initialParent, NULL, hInstance, cc);
	if (c->hwnd == NULL)
		logLastError("error creating uiContainer window in uiMakeContainer()");

	uiControl(cc)->Internal = c;
	uiControl(cc)->Destroy = containerDestroy;
	uiControl(cc)->Handle = containerHandle;
	uiControl(cc)->SetParent = containerSetParent;
	// PreferredSize() is provided by subclasses
	uiControl(cc)->Resize = containerResize;
	uiControl(cc)->Visible = containerVisible;
	uiControl(cc)->Show = containerShow;
	uiControl(cc)->Hide = containerHide;
	uiControl(cc)->Enable = containerEnable;
	uiControl(cc)->Disable = containerDisable;

	// ResizeChildren() is provided by subclasses
	uiContainer(cc)->Update = containerUpdate;
}
