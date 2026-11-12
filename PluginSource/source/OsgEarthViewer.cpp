// osgEarth viewer inside Unity (Direct3D 11).
//
// This is a port of osgearth/src/applications/osgearth_viewer/osgearth_viewer.cpp
// (osgEarth, Copyright Pelican Mapping, MIT License) to the Unity native rendering
// plugin. osgEarth only speaks OpenGL while Unity runs on Direct3D 11, so:
//
//   1. the plugin creates its own (hidden) OpenGL context on Unity's render thread;
//   2. the osgViewer::Viewer is "embedded" in that context and draws into an OpenGL
//      framebuffer object (FBO) instead of a window;
//   3. the FBO color buffer is a D3D11 texture shared with OpenGL through the
//      WGL_NV_DX_interop2 extension (zero copy on the CPU), and every frame it is
//      copied on the GPU into the Texture2D that Unity passed to the plugin.
//      If the interop extension is not available (e.g. hybrid laptops where the GL
//      and D3D devices are different GPUs) we fall back to glReadPixels +
//      UpdateSubresource, which is slower but works everywhere.
//
// Everything runs from GL.IssuePluginEvent() callbacks, i.e. on Unity's render
// thread, which is the only thread that may use the D3D11 immediate context.
// Input events coming from C# (main thread) are queued and replayed into the OSG
// event queue right before each frame.

#include "PlatformBase.h"

#if SUPPORT_OSGEARTH

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>

#include "Unity/IUnityGraphics.h"
#include "Unity/IUnityGraphicsD3D11.h"

// Same includes as osgearth_viewer.cpp (+ a few needed by the embedding code)
#include <osgViewer/Viewer>
#include <osgEarth/EarthManipulator>
#include <osgEarth/ExampleResources>
#include <osgEarth/MapNode>
#include <osgEarth/PhongLightingEffect>
#include <osgEarth/Registry>
#include <osgEarth/ShaderGenerator>
#include <osgEarth/Notify>
#include <osgGA/TrackballManipulator>
#include <osgDB/Registry>
#include <osg/LightSource>

#include <algorithm>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

using namespace osgEarth;
using namespace osgEarth::Util;


// --------------------------------------------------------------------------
// Minimal OpenGL / WGL declarations (Windows' GL/gl.h only exposes GL 1.1).
// Prefixed names avoid clashes with macros coming from OSG's GL headers.

namespace
{
	const GLenum kGL_FRAMEBUFFER = 0x8D40;
	const GLenum kGL_READ_FRAMEBUFFER = 0x8CA8;
	const GLenum kGL_DRAW_FRAMEBUFFER = 0x8CA9;
	const GLenum kGL_RENDERBUFFER = 0x8D41;
	const GLenum kGL_COLOR_ATTACHMENT0 = 0x8CE0;
	const GLenum kGL_DEPTH_STENCIL_ATTACHMENT = 0x821A;
	const GLenum kGL_DEPTH24_STENCIL8 = 0x88F0;
	const GLenum kGL_RGBA8 = 0x8058;
	const GLenum kGL_RGBA = 0x1908;
	const GLenum kGL_BGRA = 0x80E1;
	const GLenum kGL_FRAMEBUFFER_COMPLETE = 0x8CD5;
	const GLenum kGL_MAX_SAMPLES = 0x8D57;
	const GLenum kGL_PIXEL_PACK_BUFFER = 0x88EB;
	const GLenum kGL_PIXEL_PACK_BUFFER_BINDING = 0x88ED;
	const GLenum kWGL_ACCESS_WRITE_DISCARD_NV = 0x0002;

	typedef void   (APIENTRY* PFN_glGenFramebuffers)(GLsizei, GLuint*);
	typedef void   (APIENTRY* PFN_glDeleteFramebuffers)(GLsizei, const GLuint*);
	typedef void   (APIENTRY* PFN_glBindFramebuffer)(GLenum, GLuint);
	typedef void   (APIENTRY* PFN_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
	typedef void   (APIENTRY* PFN_glFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
	typedef GLenum (APIENTRY* PFN_glCheckFramebufferStatus)(GLenum);
	typedef void   (APIENTRY* PFN_glGenRenderbuffers)(GLsizei, GLuint*);
	typedef void   (APIENTRY* PFN_glDeleteRenderbuffers)(GLsizei, const GLuint*);
	typedef void   (APIENTRY* PFN_glBindRenderbuffer)(GLenum, GLuint);
	typedef void   (APIENTRY* PFN_glRenderbufferStorageMultisample)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
	typedef void   (APIENTRY* PFN_glBlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
	typedef void   (APIENTRY* PFN_glBindBuffer)(GLenum, GLuint);

	// WGL_NV_DX_interop / WGL_NV_DX_interop2
	typedef HANDLE (WINAPI* PFN_wglDXOpenDeviceNV)(void* dxDevice);
	typedef BOOL   (WINAPI* PFN_wglDXCloseDeviceNV)(HANDLE hDevice);
	typedef HANDLE (WINAPI* PFN_wglDXRegisterObjectNV)(HANDLE hDevice, void* dxObject, GLuint name, GLenum type, GLenum access);
	typedef BOOL   (WINAPI* PFN_wglDXUnregisterObjectNV)(HANDLE hDevice, HANDLE hObject);
	typedef BOOL   (WINAPI* PFN_wglDXLockObjectsNV)(HANDLE hDevice, GLint count, HANDLE* hObjects);
	typedef BOOL   (WINAPI* PFN_wglDXUnlockObjectsNV)(HANDLE hDevice, GLint count, HANDLE* hObjects);

	struct GLFunctions
	{
		PFN_glGenFramebuffers GenFramebuffers = nullptr;
		PFN_glDeleteFramebuffers DeleteFramebuffers = nullptr;
		PFN_glBindFramebuffer BindFramebuffer = nullptr;
		PFN_glFramebufferTexture2D FramebufferTexture2D = nullptr;
		PFN_glFramebufferRenderbuffer FramebufferRenderbuffer = nullptr;
		PFN_glCheckFramebufferStatus CheckFramebufferStatus = nullptr;
		PFN_glGenRenderbuffers GenRenderbuffers = nullptr;
		PFN_glDeleteRenderbuffers DeleteRenderbuffers = nullptr;
		PFN_glBindRenderbuffer BindRenderbuffer = nullptr;
		PFN_glRenderbufferStorageMultisample RenderbufferStorageMultisample = nullptr;
		PFN_glBlitFramebuffer BlitFramebuffer = nullptr;
		PFN_glBindBuffer BindBuffer = nullptr;

		PFN_wglDXOpenDeviceNV DXOpenDevice = nullptr;
		PFN_wglDXCloseDeviceNV DXCloseDevice = nullptr;
		PFN_wglDXRegisterObjectNV DXRegisterObject = nullptr;
		PFN_wglDXUnregisterObjectNV DXUnregisterObject = nullptr;
		PFN_wglDXLockObjectsNV DXLockObjects = nullptr;
		PFN_wglDXUnlockObjectsNV DXUnlockObjects = nullptr;

		bool hasFBO() const
		{
			return GenFramebuffers && DeleteFramebuffers && BindFramebuffer && FramebufferTexture2D &&
				FramebufferRenderbuffer && CheckFramebufferStatus && GenRenderbuffers && DeleteRenderbuffers &&
				BindRenderbuffer && RenderbufferStorageMultisample && BlitFramebuffer && BindBuffer;
		}
		bool hasInterop() const
		{
			return DXOpenDevice && DXCloseDevice && DXRegisterObject && DXUnregisterObject && DXLockObjects && DXUnlockObjects;
		}
	};

	template<typename T> void LoadGL(T& fn, const char* name)
	{
		PROC p = wglGetProcAddress(name);
		// Some drivers return small integers instead of NULL for unsupported entry points.
		const INT_PTR v = (INT_PTR)p;
		fn = (v == 0 || v == 1 || v == 2 || v == 3 || v == -1) ? nullptr : reinterpret_cast<T>(reinterpret_cast<void*>(p));
	}


	// ----------------------------------------------------------------------
	// Plugin state. Members marked [main] are written from the C# main thread
	// (always under g_Mutex), everything else is owned by Unity's render thread.

	enum MouseEventType { kMouseMove = 0, kMousePress = 1, kMouseRelease = 2, kMouseScroll = 3, kMouseDoubleClick = 4 };
	enum KeyEventType { kKeyPress = 0, kKeyRelease = 1 };

	struct InputEvent
	{
		bool isKey;
		int type;
		float x, y;
		int value; // mouse button (1=left 2=middle 3=right), scroll direction (+1 up / -1 down) or key code
	};

	std::mutex g_Mutex;
	std::string g_CommandLine;               // [main]
	std::string g_DataPath;                  // [main]
	ID3D11Texture2D* g_PendingTexture = nullptr; // [main] AddRef'd, consumed by the render thread
	bool g_HasPendingTexture = false;        // [main]
	std::vector<InputEvent> g_InputEvents;   // [main]
	std::string g_Status = "Idle";
	std::deque<std::string> g_Log;

	void SetStatus(const std::string& s)
	{
		std::lock_guard<std::mutex> lock(g_Mutex);
		g_Status = s;
	}

	void Log(const std::string& msg)
	{
		OutputDebugStringA(("[osgEarth] " + msg + "\n").c_str());
		std::lock_guard<std::mutex> lock(g_Mutex);
		if (g_Log.size() > 1000)
			g_Log.pop_front();
		g_Log.push_back(msg);
	}

	// Routes OSG and osgEarth notifications to the Unity console (polled from C#)
	struct UnityNotifyHandler : public osg::NotifyHandler
	{
		void notify(osg::NotifySeverity severity, const char* message) override
		{
			std::string msg(message ? message : "");
			while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
				msg.pop_back();
			if (msg.empty())
				return;
			const char* prefix = severity <= osg::WARN ? "[W] " : "";
			Log(prefix + msg);
		}
	};


	// ----------------------------------------------------------------------
	// Render-thread state

	struct State
	{
		// Hidden window + OpenGL context
		HWND hwnd = nullptr;
		HDC hdc = nullptr;
		HGLRC hglrc = nullptr;
		GLFunctions gl;

		// D3D11
		ID3D11Device* device = nullptr;      // not owned (Unity's)
		ID3D11Texture2D* unityTex = nullptr; // AddRef'd
		ID3D11Texture2D* sharedTex = nullptr;
		int width = 0, height = 0;
		GLenum readFormat = 0;

		// Interop
		HANDLE interopDevice = nullptr;
		HANDLE interopObject = nullptr;
		bool interopLocked = false;
		bool interopDisabled = false; // set after a failure, so we keep using the fallback

		// GL render targets
		GLuint colorTex = 0;     // GL name of the interop texture
		GLuint colorRB = 0;      // color renderbuffer (fallback path)
		GLuint depthRB = 0;
		GLuint targetFBO = 0;    // final, single-sampled image
		GLuint msaaColorRB = 0;
		GLuint msaaDepthRB = 0;
		GLuint msaaFBO = 0;      // where OSG draws when multisampling is on
		int samples = 0;
		std::vector<unsigned char> pixels; // fallback readback buffer

		// osgEarth
		osg::ref_ptr<osgViewer::Viewer> viewer;
		osg::ref_ptr<osgViewer::GraphicsWindow> gw;
		osg::ref_ptr<osg::LightSource> lightSource; // "--light" mode
		bool failed = false;

		bool usingInterop() const { return interopObject != nullptr; }
		GLuint renderFBO() const { return msaaFBO ? msaaFBO : targetFBO; }
	};

	State s;
	bool s_NotifyInstalled = false;


	// ----------------------------------------------------------------------
	// OpenGL context

	bool MakeCurrent()
	{
		if (wglGetCurrentContext() == s.hglrc)
			return true;
		return wglMakeCurrent(s.hdc, s.hglrc) == TRUE;
	}

	// Restores whatever context was current on this thread (normally none: Unity is on D3D11),
	// so our context is never left bound to Unity's render thread between plugin events.
	struct ScopedContextRestore
	{
		HGLRC rc;
		HDC dc;
		ScopedContextRestore() : rc(wglGetCurrentContext()), dc(wglGetCurrentDC()) {}
		~ScopedContextRestore()
		{
			if (wglGetCurrentContext() != rc)
				wglMakeCurrent(dc, rc);
		}
	};

	bool CreateGLContext()
	{
		const wchar_t* kClassName = L"UnityOsgEarthHiddenGLWindow";
		HINSTANCE hinst = GetModuleHandleW(NULL);

		WNDCLASSEXW wc;
		ZeroMemory(&wc, sizeof(wc));
		wc.cbSize = sizeof(wc);
		wc.style = CS_OWNDC;
		wc.lpfnWndProc = DefWindowProcW;
		wc.hInstance = hinst;
		wc.lpszClassName = kClassName;
		RegisterClassExW(&wc); // fails harmlessly if already registered (e.g. second Play in the Editor)

		// Never shown: it only exists because WGL needs a device context.
		s.hwnd = CreateWindowExW(0, kClassName, L"osgEarth", WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
			0, 0, 16, 16, NULL, NULL, hinst, NULL);
		if (!s.hwnd)
		{
			Log("CreateWindowEx failed");
			return false;
		}
		s.hdc = GetDC(s.hwnd);

		PIXELFORMATDESCRIPTOR pfd;
		ZeroMemory(&pfd, sizeof(pfd));
		pfd.nSize = sizeof(pfd);
		pfd.nVersion = 1;
		pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
		pfd.iPixelType = PFD_TYPE_RGBA;
		pfd.cColorBits = 32;
		pfd.cAlphaBits = 8;
		pfd.cDepthBits = 24;
		pfd.cStencilBits = 8;
		pfd.iLayerType = PFD_MAIN_PLANE;
		int pf = ChoosePixelFormat(s.hdc, &pfd);
		if (pf == 0 || !SetPixelFormat(s.hdc, pf, &pfd))
		{
			Log("SetPixelFormat failed");
			return false;
		}

		// A legacy context gives the highest *compatibility* profile version supported by the
		// driver (4.6 on current hardware), which is what osgEarth/OSG (GL2 profile build) want.
		s.hglrc = wglCreateContext(s.hdc);
		if (!s.hglrc || !MakeCurrent())
		{
			Log("wglCreateContext / wglMakeCurrent failed");
			return false;
		}

		const char* vendor = (const char*)glGetString(GL_VENDOR);
		const char* renderer = (const char*)glGetString(GL_RENDERER);
		const char* version = (const char*)glGetString(GL_VERSION);
		Log(std::string("OpenGL context: ") + (renderer ? renderer : "?") + " / " + (vendor ? vendor : "?") + " / " + (version ? version : "?"));

		GLFunctions& gl = s.gl;
		LoadGL(gl.GenFramebuffers, "glGenFramebuffers");
		LoadGL(gl.DeleteFramebuffers, "glDeleteFramebuffers");
		LoadGL(gl.BindFramebuffer, "glBindFramebuffer");
		LoadGL(gl.FramebufferTexture2D, "glFramebufferTexture2D");
		LoadGL(gl.FramebufferRenderbuffer, "glFramebufferRenderbuffer");
		LoadGL(gl.CheckFramebufferStatus, "glCheckFramebufferStatus");
		LoadGL(gl.GenRenderbuffers, "glGenRenderbuffers");
		LoadGL(gl.DeleteRenderbuffers, "glDeleteRenderbuffers");
		LoadGL(gl.BindRenderbuffer, "glBindRenderbuffer");
		LoadGL(gl.RenderbufferStorageMultisample, "glRenderbufferStorageMultisample");
		LoadGL(gl.BlitFramebuffer, "glBlitFramebuffer");
		LoadGL(gl.BindBuffer, "glBindBuffer");
		LoadGL(gl.DXOpenDevice, "wglDXOpenDeviceNV");
		LoadGL(gl.DXCloseDevice, "wglDXCloseDeviceNV");
		LoadGL(gl.DXRegisterObject, "wglDXRegisterObjectNV");
		LoadGL(gl.DXUnregisterObject, "wglDXUnregisterObjectNV");
		LoadGL(gl.DXLockObjects, "wglDXLockObjectsNV");
		LoadGL(gl.DXUnlockObjects, "wglDXUnlockObjectsNV");

		if (!gl.hasFBO())
		{
			Log("OpenGL 3.0 framebuffer objects are not available");
			return false;
		}
		if (!gl.hasInterop())
			Log("WGL_NV_DX_interop2 not available: using the CPU readback fallback");
		return true;
	}

	void PumpWindowMessages()
	{
		// The hidden window lives on the render thread, which has no message loop.
		MSG msg;
		while (PeekMessageW(&msg, s.hwnd, 0, 0, PM_REMOVE))
			DispatchMessageW(&msg);
	}


	// ----------------------------------------------------------------------
	// Render targets: GL FBO <-> D3D11 texture

	void DestroyTargets()
	{
		GLFunctions& gl = s.gl;
		if (s.interopObject)
		{
			gl.DXUnregisterObject(s.interopDevice, s.interopObject);
			s.interopObject = nullptr;
		}
		if (s.targetFBO) { gl.DeleteFramebuffers(1, &s.targetFBO); s.targetFBO = 0; }
		if (s.msaaFBO) { gl.DeleteFramebuffers(1, &s.msaaFBO); s.msaaFBO = 0; }
		if (s.colorRB) { gl.DeleteRenderbuffers(1, &s.colorRB); s.colorRB = 0; }
		if (s.depthRB) { gl.DeleteRenderbuffers(1, &s.depthRB); s.depthRB = 0; }
		if (s.msaaColorRB) { gl.DeleteRenderbuffers(1, &s.msaaColorRB); s.msaaColorRB = 0; }
		if (s.msaaDepthRB) { gl.DeleteRenderbuffers(1, &s.msaaDepthRB); s.msaaDepthRB = 0; }
		if (s.colorTex) { glDeleteTextures(1, &s.colorTex); s.colorTex = 0; }
		SAFE_RELEASE(s.sharedTex);
	}

	GLuint CreateRenderbuffer(GLenum format, int samples)
	{
		GLuint rb = 0;
		s.gl.GenRenderbuffers(1, &rb);
		s.gl.BindRenderbuffer(kGL_RENDERBUFFER, rb);
		s.gl.RenderbufferStorageMultisample(kGL_RENDERBUFFER, samples, format, s.width, s.height);
		s.gl.BindRenderbuffer(kGL_RENDERBUFFER, 0);
		return rb;
	}

	bool CheckFBO(const char* what)
	{
		GLenum status = s.gl.CheckFramebufferStatus(kGL_FRAMEBUFFER);
		if (status != kGL_FRAMEBUFFER_COMPLETE)
		{
			char buf[128];
			snprintf(buf, sizeof(buf), "%s framebuffer incomplete (0x%04X)", what, (unsigned)status);
			Log(buf);
			return false;
		}
		return true;
	}

	// Tries to make targetFBO render straight into a D3D11 texture shared with OpenGL.
	bool CreateInteropTarget(DXGI_FORMAT sharedFormat)
	{
		GLFunctions& gl = s.gl;
		if (s.interopDisabled || !gl.hasInterop())
			return false;

		if (!s.interopDevice)
		{
			s.interopDevice = gl.DXOpenDevice(s.device);
			if (!s.interopDevice)
			{
				Log("wglDXOpenDeviceNV failed (OpenGL and Direct3D on different GPUs?): using the CPU readback fallback");
				s.interopDisabled = true;
				return false;
			}
		}

		D3D11_TEXTURE2D_DESC td;
		ZeroMemory(&td, sizeof(td));
		td.Width = s.width;
		td.Height = s.height;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = sharedFormat;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		td.MiscFlags = D3D11_RESOURCE_MISC_SHARED; // most compatible choice across GPU vendors
		if (FAILED(s.device->CreateTexture2D(&td, NULL, &s.sharedTex)))
		{
			Log("CreateTexture2D (shared texture) failed");
			return false;
		}

		glGenTextures(1, &s.colorTex);
		s.interopObject = gl.DXRegisterObject(s.interopDevice, s.sharedTex, s.colorTex, GL_TEXTURE_2D, kWGL_ACCESS_WRITE_DISCARD_NV);
		if (!s.interopObject)
		{
			Log("wglDXRegisterObjectNV failed: using the CPU readback fallback");
			s.interopDisabled = true;
			return false;
		}

		bool ok = false;
		if (gl.DXLockObjects(s.interopDevice, 1, &s.interopObject))
		{
			gl.BindFramebuffer(kGL_FRAMEBUFFER, s.targetFBO);
			gl.FramebufferTexture2D(kGL_FRAMEBUFFER, kGL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.colorTex, 0);
			ok = CheckFBO("Interop");
			gl.BindFramebuffer(kGL_FRAMEBUFFER, 0);
			gl.DXUnlockObjects(s.interopDevice, 1, &s.interopObject);
		}
		if (!ok)
		{
			Log("Interop texture cannot be used as render target: using the CPU readback fallback");
			s.interopDisabled = true;
		}
		return ok;
	}

	bool CreateTargets()
	{
		GLFunctions& gl = s.gl;
		DestroyTargets();

		D3D11_TEXTURE2D_DESC desc;
		s.unityTex->GetDesc(&desc);
		s.width = (int)desc.Width;
		s.height = (int)desc.Height;

		DXGI_FORMAT sharedFormat;
		switch (desc.Format)
		{
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			sharedFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
			s.readFormat = kGL_RGBA;
			break;
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			sharedFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
			s.readFormat = kGL_BGRA;
			break;
		default:
			Log("Unsupported Unity texture format (use TextureFormat.RGBA32 or BGRA32)");
			return false;
		}

		// Depth/stencil first, so the completeness check below sees the final attachments.
		gl.GenFramebuffers(1, &s.targetFBO);
		s.depthRB = CreateRenderbuffer(kGL_DEPTH24_STENCIL8, 0);
		gl.BindFramebuffer(kGL_FRAMEBUFFER, s.targetFBO);
		gl.FramebufferRenderbuffer(kGL_FRAMEBUFFER, kGL_DEPTH_STENCIL_ATTACHMENT, kGL_RENDERBUFFER, s.depthRB);
		gl.BindFramebuffer(kGL_FRAMEBUFFER, 0);

		if (!CreateInteropTarget(sharedFormat))
		{
			// Fallback: plain GL renderbuffer, read back with glReadPixels every frame.
			if (s.interopObject) { gl.DXUnregisterObject(s.interopDevice, s.interopObject); s.interopObject = nullptr; }
			if (s.colorTex) { glDeleteTextures(1, &s.colorTex); s.colorTex = 0; }
			SAFE_RELEASE(s.sharedTex);

			s.colorRB = CreateRenderbuffer(kGL_RGBA8, 0);
			gl.BindFramebuffer(kGL_FRAMEBUFFER, s.targetFBO);
			gl.FramebufferRenderbuffer(kGL_FRAMEBUFFER, kGL_COLOR_ATTACHMENT0, kGL_RENDERBUFFER, s.colorRB);
			bool ok = CheckFBO("Readback");
			gl.BindFramebuffer(kGL_FRAMEBUFFER, 0);
			if (!ok)
				return false;
			s.pixels.resize((size_t)s.width * s.height * 4);
		}

		// Optional multisampling (osgearth_viewer's "--samples N" / OSG_MULTI_SAMPLES):
		// OSG draws into an MSAA FBO that is resolved into targetFBO after each frame.
		if (s.samples > 0)
		{
			s.msaaColorRB = CreateRenderbuffer(kGL_RGBA8, s.samples);
			s.msaaDepthRB = CreateRenderbuffer(kGL_DEPTH24_STENCIL8, s.samples);
			gl.GenFramebuffers(1, &s.msaaFBO);
			gl.BindFramebuffer(kGL_FRAMEBUFFER, s.msaaFBO);
			gl.FramebufferRenderbuffer(kGL_FRAMEBUFFER, kGL_COLOR_ATTACHMENT0, kGL_RENDERBUFFER, s.msaaColorRB);
			gl.FramebufferRenderbuffer(kGL_FRAMEBUFFER, kGL_DEPTH_STENCIL_ATTACHMENT, kGL_RENDERBUFFER, s.msaaDepthRB);
			bool ok = CheckFBO("MSAA");
			gl.BindFramebuffer(kGL_FRAMEBUFFER, 0);
			if (!ok)
			{
				gl.DeleteFramebuffers(1, &s.msaaFBO); s.msaaFBO = 0;
				gl.DeleteRenderbuffers(1, &s.msaaColorRB); s.msaaColorRB = 0;
				gl.DeleteRenderbuffers(1, &s.msaaDepthRB); s.msaaDepthRB = 0;
				Log("Multisampling disabled");
			}
		}

		char buf[160];
		snprintf(buf, sizeof(buf), "Render target %dx%d, %s, MSAA %d", s.width, s.height,
			s.usingInterop() ? "D3D11/OpenGL interop" : "CPU readback", s.msaaFBO ? s.samples : 0);
		Log(buf);
		return true;
	}


	// ----------------------------------------------------------------------
	// osgearth_viewer main(), adapted

	std::vector<std::string> SplitCommandLine(const std::string& cmd)
	{
		// Whitespace separated, double quotes group arguments containing spaces.
		std::vector<std::string> out;
		std::string cur;
		bool inQuotes = false, hasToken = false;
		for (char c : cmd)
		{
			if (c == '"') { inQuotes = !inQuotes; hasToken = true; }
			else if (!inQuotes && (c == ' ' || c == '\t' || c == '\r' || c == '\n'))
			{
				if (hasToken) { out.push_back(cur); cur.clear(); hasToken = false; }
			}
			else { cur += c; hasToken = true; }
		}
		if (hasToken)
			out.push_back(cur);
		return out;
	}

	std::string ModuleDirectory(HMODULE module)
	{
		char path[MAX_PATH] = { 0 };
		GetModuleFileNameA(module, path, MAX_PATH);
		std::string p(path);
		size_t slash = p.find_last_of("\\/");
		return slash == std::string::npos ? std::string() : p.substr(0, slash);
	}

	bool CreateViewer()
	{
		std::string commandLine, dataPath;
		{
			std::lock_guard<std::mutex> lock(g_Mutex);
			commandLine = g_CommandLine;
			dataPath = g_DataPath;
		}

		// Build argc/argv as if osgearth_viewer had been started from a console.
		std::vector<std::string> tokens = SplitCommandLine(commandLine);
		tokens.insert(tokens.begin(), "osgearth_viewer");
		std::vector<char*> argv;
		for (std::string& t : tokens)
			argv.push_back(&t[0]);
		argv.push_back(nullptr);
		int argc = (int)tokens.size();

		// Where to look for relative data files (.earth) and OSG plugins (osgPlugins-x.y.z).
		HMODULE self = NULL;
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCSTR)&CreateViewer, &self);
		osgDB::FilePathList& dataPaths = osgDB::Registry::instance()->getDataFilePathList();
		osgDB::FilePathList& libPaths = osgDB::Registry::instance()->getLibraryFilePathList();
		if (!dataPath.empty() && std::find(dataPaths.begin(), dataPaths.end(), dataPath) == dataPaths.end())
			dataPaths.push_front(dataPath);
		for (const std::string& dir : { ModuleDirectory(self), ModuleDirectory(NULL) })
			if (std::find(libPaths.begin(), libPaths.end(), dir) == libPaths.end())
				libPaths.push_back(dir);

		// ---- from here on this follows osgearth_viewer.cpp ----

		osg::ArgumentParser arguments(&argc, argv.data());
		if (arguments.read("--help"))
		{
			Log(MapNodeHelper().usage());
			SetStatus("Usage printed to the log (--help)");
			return false;
		}

		// start up osgEarth
		// (it also creates a temporary GL context to query the GPU capabilities, which is
		// why we make our own context current again right after)
		osgEarth::initialize(arguments);
		if (!MakeCurrent())
		{
			SetStatus("Error: wglMakeCurrent failed");
			return false;
		}

		// NOTE: the Windows DPI function and the XInput GamepadHandler of the original are
		// left out: the viewer has no window of its own here (Unity owns window and input).

		// create a simple view
		s.viewer = new osgViewer::Viewer(arguments);

		// install our default manipulator (do this before calling load)
		s.viewer->setCameraManipulator(new EarthManipulator(arguments));

		// disable the small-feature culling; necessary for some feature rendering
		s.viewer->getCamera()->setSmallFeatureCullingPixelSize(-1.0f);

		// [Unity] embed the viewer in our hidden OpenGL context instead of opening a window,
		// and never let a key (ESC) or a quit event terminate it.
		osgViewer::GraphicsWindow* gw = s.viewer->setUpViewerAsEmbeddedInWindow(0, 0, s.width, s.height);
		s.gw = gw;
		s.viewer->setKeyEventSetsDone(0);
		s.viewer->setQuitEventSetsDone(false);
		gw->getEventQueue()->getCurrentEventState()->setMouseYOrientation(osgGA::GUIEventAdapter::Y_INCREASING_DOWNWARDS);
		gw->getEventQueue()->windowResize(0, 0, s.width, s.height);

		// load an earth file, and support all or our example command-line options
		auto node = MapNodeHelper().load(arguments, s.viewer.get());
		if (node.valid())
		{
			if (MapNode::get(node))
			{
				s.viewer->setSceneData(node);
			}
			else
			{
				// not an earth file? Just view as a normal OSG node or image with basic lighting
				s.viewer->setCameraManipulator(new osgGA::TrackballManipulator);

				if (arguments.read("--light"))
				{
					osg::LightSource* lightSource = new osg::LightSource();
					lightSource->getLight()->setAmbient(osg::Vec4(0.75f, 0.75f, 0.75f, 1.0f));

					auto group = new PhongLightingGroup();
					group->addChild(lightSource);
					group->addChild(node);

					ShaderGenerator gen;
					gen.run(group);

					s.viewer->setSceneData(group);
					s.lightSource = lightSource; // updated every frame, see RenderFrame()
				}
				else
				{
					ShaderGenerator gen;
					gen.run(node);
					s.viewer->setSceneData(node);
				}
			}
		}
		else
		{
			Log(MapNodeHelper().usage());
			SetStatus("Nothing to view: check the command line / .earth file path (see the log)");
			s.gw = nullptr;
			s.viewer = nullptr;
			return false;
		}

		// MSAA requested on the command line (--samples N) or via OSG_MULTI_SAMPLES?
		GLint maxSamples = 0;
		glGetIntegerv(kGL_MAX_SAMPLES, &maxSamples);
		s.samples = std::min((int)osg::DisplaySettings::instance()->getNumMultiSamples(), (int)maxSamples);

		// [Unity] viewer.run() is replaced by one viewer.frame() per Unity frame (RenderFrame).
		// Realize now, with our context current, so that osgEarth's realize operation runs on it.
		if (!MakeCurrent())
		{
			SetStatus("Error: wglMakeCurrent failed");
			return false;
		}
		s.viewer->realize();
		return true;
	}

	void ApplyQueuedInput()
	{
		std::vector<InputEvent> events;
		{
			std::lock_guard<std::mutex> lock(g_Mutex);
			events.swap(g_InputEvents);
		}
		osgViewer::GraphicsWindow* gw = s.gw.get();
		if (!gw)
			return;
		osgGA::EventQueue* q = gw->getEventQueue();
		for (const InputEvent& e : events)
		{
			if (e.isKey)
			{
				if (e.type == kKeyPress) q->keyPress(e.value);
				else q->keyRelease(e.value);
				continue;
			}
			switch (e.type)
			{
			case kMouseMove: q->mouseMotion(e.x, e.y); break;
			case kMousePress: q->mouseButtonPress(e.x, e.y, e.value); break;
			case kMouseRelease: q->mouseButtonRelease(e.x, e.y, e.value); break;
			case kMouseDoubleClick: q->mouseDoubleButtonPress(e.x, e.y, e.value); break;
			case kMouseScroll:
				q->mouseMotion(e.x, e.y);
				q->mouseScroll(e.value > 0 ? osgGA::GUIEventAdapter::SCROLL_UP : osgGA::GUIEventAdapter::SCROLL_DOWN);
				break;
			}
		}
	}

	void CopyToUnityTexture()
	{
		ID3D11DeviceContext* ctx = NULL;
		s.device->GetImmediateContext(&ctx);
		if (s.usingInterop())
		{
			// GPU copy; CopySubresourceRegion also tolerates a mip chain on the Unity texture.
			ctx->CopySubresourceRegion(s.unityTex, 0, 0, 0, 0, s.sharedTex, 0, NULL);
		}
		else
		{
			ctx->UpdateSubresource(s.unityTex, 0, NULL, s.pixels.data(), s.width * 4, 0);
		}
		ctx->Release();
	}

	void ReadBackPixels()
	{
		GLFunctions& gl = s.gl;
		GLint packBuffer = 0;
		glGetIntegerv(kGL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
		if (packBuffer) gl.BindBuffer(kGL_PIXEL_PACK_BUFFER, 0);
		glPixelStorei(GL_PACK_ALIGNMENT, 4);
		glPixelStorei(GL_PACK_ROW_LENGTH, 0);
		gl.BindFramebuffer(kGL_READ_FRAMEBUFFER, s.targetFBO);
		glReadPixels(0, 0, s.width, s.height, s.readFormat, GL_UNSIGNED_BYTE, s.pixels.data());
		if (packBuffer) gl.BindBuffer(kGL_PIXEL_PACK_BUFFER, (GLuint)packBuffer);
	}

	void RenderFrame()
	{
		GLFunctions& gl = s.gl;

		if (s.usingInterop())
		{
			if (!gl.DXLockObjects(s.interopDevice, 1, &s.interopObject))
			{
				Log("wglDXLockObjectsNV failed, skipping frame");
				return;
			}
			s.interopLocked = true;
		}

		// OSG draws the main camera into whatever framebuffer is bound, and RTT cameras
		// restore the context's "default FBO" when they are done: point both at ours.
		const GLuint fbo = s.renderFBO();
		s.gw->setDefaultFboId(fbo);
		gl.BindFramebuffer(kGL_FRAMEBUFFER, fbo);

		if (s.lightSource.valid())
		{
			// from osgearth_viewer.cpp ("--light" loop)
			auto cam = s.viewer->getCamera()->getInverseViewMatrix().getTrans();
			cam.normalize();
			s.lightSource->getLight()->setPosition(osg::Vec4d(cam.x(), cam.y(), cam.z(), 0));
		}

		s.viewer->frame();

		if (s.msaaFBO)
		{
			// resolve MSAA (blits are affected by the scissor test)
			GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
			if (scissor) glDisable(GL_SCISSOR_TEST);
			gl.BindFramebuffer(kGL_READ_FRAMEBUFFER, s.msaaFBO);
			gl.BindFramebuffer(kGL_DRAW_FRAMEBUFFER, s.targetFBO);
			gl.BlitFramebuffer(0, 0, s.width, s.height, 0, 0, s.width, s.height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
			if (scissor) glEnable(GL_SCISSOR_TEST);
		}

		if (!s.usingInterop())
			ReadBackPixels();

		gl.BindFramebuffer(kGL_FRAMEBUFFER, 0);

		// Unlocking hands the texture back to D3D11 (the driver synchronizes the two APIs).
		if (s.usingInterop())
		{
			gl.DXUnlockObjects(s.interopDevice, 1, &s.interopObject);
			s.interopLocked = false;
		}

		CopyToUnityTexture();
	}

	void ReleaseEverything()
	{
		ScopedContextRestore restore;
		if (s.hglrc)
		{
			MakeCurrent();
			// Destroying the viewer closes its (embedded) graphics context, which deletes the
			// OSG GL objects: our context must be current while that happens.
			s.gw = nullptr;
			s.viewer = nullptr;
			s.lightSource = nullptr;
			DestroyTargets();
			if (s.interopDevice)
			{
				s.gl.DXCloseDevice(s.interopDevice);
				s.interopDevice = nullptr;
			}
			wglMakeCurrent(NULL, NULL);
			wglDeleteContext(s.hglrc);
		}
		else
		{
			s.gw = nullptr;
			s.viewer = nullptr;
			SAFE_RELEASE(s.sharedTex);
		}
		if (s.hdc) ReleaseDC(s.hwnd, s.hdc);
		if (s.hwnd) DestroyWindow(s.hwnd);
		SAFE_RELEASE(s.unityTex);
		s = State();
	}

} // namespace


// --------------------------------------------------------------------------
// Called by RenderingPlugin.cpp on Unity's render thread

void OsgEarth_Render(IUnityInterfaces* unityInterfaces)
{
	// Pick up a new target texture from C#, if any.
	{
		std::lock_guard<std::mutex> lock(g_Mutex);
		if (g_HasPendingTexture)
		{
			SAFE_RELEASE(s.unityTex);
			s.unityTex = g_PendingTexture; // reference handed over
			g_PendingTexture = nullptr;
			g_HasPendingTexture = false;
			s.width = s.height = 0; // force CreateTargets()
		}
	}
	if (!s.unityTex || s.failed)
		return;

	if (!s_NotifyInstalled)
	{
		osg::ref_ptr<osg::NotifyHandler> handler = new UnityNotifyHandler();
		osg::setNotifyHandler(handler.get());
		osgEarth::setNotifyHandler(handler.get());
		s_NotifyInstalled = true;
	}

	if (!s.device)
		s.device = unityInterfaces->Get<IUnityGraphicsD3D11>()->GetDevice();

	ScopedContextRestore restore;
	try
	{
		if (!s.hglrc)
		{
			SetStatus("Creating OpenGL context");
			if (!CreateGLContext())
			{
				SetStatus("Error: could not create the OpenGL context (see the log)");
				s.failed = true;
				return;
			}
		}

		if (!MakeCurrent())
		{
			SetStatus("Error: wglMakeCurrent failed");
			return;
		}
		PumpWindowMessages();

		if (!s.viewer.valid())
		{
			D3D11_TEXTURE2D_DESC desc;
			s.unityTex->GetDesc(&desc);
			s.width = (int)desc.Width;
			s.height = (int)desc.Height;
			SetStatus("Loading osgEarth...");
			if (!CreateViewer())
			{
				s.failed = true;
				return;
			}
			s.width = s.height = 0; // targets not created yet
		}

		if (s.width == 0)
		{
			if (!CreateTargets())
			{
				SetStatus("Error: could not create the render targets (see the log)");
				s.failed = true;
				return;
			}
			// keep OSG's viewport/projection and the mouse input range in sync with the texture
			s.gw->getEventQueue()->windowResize(0, 0, s.width, s.height);
			s.gw->resized(0, 0, s.width, s.height);
			SetStatus(s.usingInterop() ? "Running (D3D11/OpenGL interop)" : "Running (CPU readback fallback)");
		}

		ApplyQueuedInput();
		RenderFrame();
	}
	catch (const std::exception& e)
	{
		Log(std::string("Exception: ") + e.what());
		SetStatus(std::string("Error: ") + e.what());
		s.failed = true;
		if (s.interopLocked)
		{
			s.gl.DXUnlockObjects(s.interopDevice, 1, &s.interopObject);
			s.interopLocked = false;
		}
		if (s.gl.BindFramebuffer)
			s.gl.BindFramebuffer(kGL_FRAMEBUFFER, 0);
	}
}

void OsgEarth_Shutdown()
{
	// Only the render-thread state is released: a texture/command line set by C# after the
	// shutdown event was queued (OnDisable followed by OnEnable) must survive it.
	ReleaseEverything();
	SetStatus("Idle");
}


// --------------------------------------------------------------------------
// Functions called from C# (UseOsgEarthPlugin.cs), on Unity's main thread

// commandLine: the arguments of osgearth_viewer (e.g. "simple.earth --sky --samples 4"), UTF-8.
// dataPath: extra folder where relative file names are searched (e.g. StreamingAssets).
extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API OsgEarth_SetCommandLine(const char* commandLine, const char* dataPath)
{
	std::lock_guard<std::mutex> lock(g_Mutex);
	g_CommandLine = commandLine ? commandLine : "";
	g_DataPath = dataPath ? dataPath : "";
}

// Same idea as SetTextureFromUnity: the osgEarth image is copied into this texture every
// frame (it must be RGBA32 or BGRA32). Pass NULL to stop.
extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API OsgEarth_SetTexture(void* textureHandle, int /*w*/, int /*h*/)
{
	ID3D11Texture2D* tex = (ID3D11Texture2D*)textureHandle;
	if (tex)
		tex->AddRef(); // keeps it alive until the render thread is done with it
	std::lock_guard<std::mutex> lock(g_Mutex);
	SAFE_RELEASE(g_PendingTexture);
	g_PendingTexture = tex;
	g_HasPendingTexture = true;
}

// x, y in texture pixels, origin at the top-left corner.
extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API OsgEarth_MouseEvent(int type, float x, float y, int value)
{
	std::lock_guard<std::mutex> lock(g_Mutex);
	InputEvent e = { false, type, x, y, value };
	g_InputEvents.push_back(e);
}

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API OsgEarth_KeyEvent(int type, int key)
{
	std::lock_guard<std::mutex> lock(g_Mutex);
	InputEvent e = { true, type, 0.0f, 0.0f, key };
	g_InputEvents.push_back(e);
}

// Returned strings stay valid until the next call from the same thread.
extern "C" UNITY_INTERFACE_EXPORT const char* UNITY_INTERFACE_API OsgEarth_GetStatus()
{
	static thread_local std::string result;
	std::lock_guard<std::mutex> lock(g_Mutex);
	result = g_Status;
	return result.c_str();
}

// Pops the next OSG/osgEarth log line, or returns NULL when there are none.
extern "C" UNITY_INTERFACE_EXPORT const char* UNITY_INTERFACE_API OsgEarth_PopLogMessage()
{
	static thread_local std::string result;
	std::lock_guard<std::mutex> lock(g_Mutex);
	if (g_Log.empty())
		return NULL;
	result = g_Log.front();
	g_Log.pop_front();
	return result.c_str();
}

#endif // #if SUPPORT_OSGEARTH
