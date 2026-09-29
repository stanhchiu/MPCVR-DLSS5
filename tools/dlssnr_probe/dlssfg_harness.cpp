// DLSS Frame Generation (FG) test harness.
// Tests CDlssFG in isolation with D3D11-to-D3D12 interop.

#include "stdafx.h"
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <string>
#include <vector>
#include "Helper.h"
#include "DX11Helper.h"
#include "DLSS/DlssFG.h"
#include "MinHook.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

HRESULT SaveToBMP(BYTE*, UINT, UINT, UINT, UINT, const wchar_t*) { return E_NOTIMPL; }

typedef void (__cdecl* PFN_DlssgLog)(const char* file, int line, const char* func, const char* fmt, ...);
static PFN_DlssgLog s_pRealDlssgLog = nullptr;

static void __cdecl HookDlssgLog(const char* file, int line, const char* func, const char* fmt, ...)
{
	char buf[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	printf("    >>> DLSSG LOG [%s:%d in %s]: %s\n", file ? file : "?", line, func ? func : "?", buf);
	fflush(stdout);
}

static int g_failures = 0;
static void Head(const char* s) { printf("\n=== %s ===\n", s); }
static void Check(bool ok, const char* what)
{
	printf("  [%s] %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) g_failures++;
}

static bool MakeSharedPair(ID3D11Device* dev, Tex2D_t& in, Tex2D_t& out, UINT w, UINT h)
{
	in.Release();
	out.Release();
	HRESULT hr = in.Create(dev, DXGI_FORMAT_R16G16B16A16_FLOAT, w, h, Tex2D_DefaultShaderRTargetUAVShared);
	if (SUCCEEDED(hr)) {
		hr = out.Create(dev, DXGI_FORMAT_R16G16B16A16_FLOAT, w, h, Tex2D_DefaultShaderRTargetUAVShared);
	}
	return SUCCEEDED(hr) && in.pTexture && out.pTexture;
}

static void FillInput(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, float phase)
{
	CComPtr<ID3D11RenderTargetView> rtv;
	if (SUCCEEDED(dev->CreateRenderTargetView(tex, nullptr, &rtv))) {
		const FLOAT c[4] = { 0.25f + 0.5f * phase, 0.5f, 0.75f - 0.5f * phase, 1.0f };
		ctx->ClearRenderTargetView(rtv, c);
	}
}

static bool OutputIsNonZero(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex)
{
	D3D11_TEXTURE2D_DESC d = {};
	tex->GetDesc(&d);
	d.Usage = D3D11_USAGE_STAGING;
	d.BindFlags = 0;
	d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	d.MiscFlags = 0;

	CComPtr<ID3D11Texture2D> stage;
	if (FAILED(dev->CreateTexture2D(&d, nullptr, &stage))) return false;
	ctx->CopyResource(stage, tex);

	D3D11_MAPPED_SUBRESOURCE mr = {};
	if (FAILED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &mr))) return false;
	const uint16_t* px = (const uint16_t*)mr.pData;
	bool nonZero = false;
	for (int i = 0; i < 256 && !nonZero; i++) {
		if (px[i]) nonZero = true;
	}
	ctx->Unmap(stage, 0);
	return nonZero;
}

int wmain(int argc, wchar_t** argv)
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	printf("CDlssFG harness -- DLSS Frame Generation test\n");
	printf("==============================================\n");

	Head("D3D11 device");
	CComPtr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
		printf("CreateDXGIFactory1 failed\n");
		return 1;
	}

	CComPtr<IDXGIAdapter1> adapter;
	CComPtr<IDXGIAdapter1> nvidiaAdapter;
	for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++, adapter.Release()) {
		DXGI_ADAPTER_DESC1 d = {};
		adapter->GetDesc1(&d);
		if (d.VendorId == 0x10DE) {
			nvidiaAdapter = adapter;
			break;
		}
	}

	if (!nvidiaAdapter) {
		printf("NVIDIA adapter not found.\n");
		return 1;
	}

	CComPtr<ID3D11Device> dev;
	CComPtr<ID3D11DeviceContext> ctx;
	const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
	HRESULT hr = D3D11CreateDevice(nvidiaAdapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
	                               D3D11_CREATE_DEVICE_BGRA_SUPPORT,
	                               levels, (UINT)std::size(levels), D3D11_SDK_VERSION, &dev, nullptr, &ctx);
	Check(SUCCEEDED(hr), "D3D11CreateDevice");
	if (FAILED(hr)) return 1;

	Head("CDlssFG::Init");
	printf("  calling fg.Init...\n"); fflush(stdout);
	CDlssFG fg;
	const bool bInit = fg.Init(dev, nullptr);
	printf("  fg.Init returned %d\n", bInit ? 1 : 0); fflush(stdout);
	Check(bInit, "Init");
	printf("%S", fg.GetInfoBlock().c_str());

	if (!bInit) {
		Head("Result");
		printf("  Init failed: %S\n", fg.GetStatusLine().c_str());
		for (const auto& l : fg.GetLog()) {
			printf("    %S\n", l.c_str());
		}
		return 1;
	}

	MH_Initialize();
	HMODULE hSnippet = GetModuleHandleW(L"nvngx_dlssg.dll");
	if (hSnippet) {
		void* pLog = (BYTE*)hSnippet + 0xd7a0;
		if (MH_CreateHook(pLog, &HookDlssgLog, (void**)&s_pRealDlssgLog) == MH_OK) {
			MH_EnableHook(pLog);
			printf("  [hook] DLSSG snippet log hooked at %p\n", pLog);
		}
	}
	HMODULE hOta = GetModuleHandleW(L"160_E658700.bin");
	if (hOta) {
		void* pLog = (BYTE*)hOta + 0xd810;
		static PFN_DlssgLog s_pRealOtaLog = nullptr;
		if (MH_CreateHook(pLog, &HookDlssgLog, (void**)&s_pRealOtaLog) == MH_OK) {
			MH_EnableHook(pLog);
			printf("  [hook] DLSSG OTA log hooked at %p\n", pLog);
		}
	}

	Tex2D_t texIn, texInterpOut;
	UINT w = 1920, h = 1080;

	Head("Feature Creation (1920x1080)");
	Check(MakeSharedPair(dev, texIn, texInterpOut, w, h), "shared texture pair");
	Check(fg.CreateFeature(texIn.pTexture, texInterpOut.pTexture, w, h), "CreateFeature");

	Head("Evaluation (2x Frame Generation)");
	CDlssFG::Params params;
	params.iMultiplier = 2;

	int evaluated = 0, failed = 0, blank = 0;
	for (int frame = 1; frame <= 10; frame++) {
		FillInput(dev, ctx, texIn.pTexture, (float)frame / 10.0f);
		if (fg.Evaluate(params)) {
			evaluated++;
			if (!OutputIsNonZero(dev, ctx, texInterpOut.pTexture)) {
				blank++;
			}
		} else {
			failed++;
		}
	}

	printf("  evaluated %d, failed %d, blank %d\n", evaluated, failed, blank);
	Check(failed == 0, "no Evaluate failures");
	Check(blank == 0, "interpolated output non-zero");

	Head("Resolution Change (3840x2160 / 4K)");
	w = 3840; h = 2160;
	Check(MakeSharedPair(dev, texIn, texInterpOut, w, h), "4K shared texture pair");
	Check(fg.CreateFeature(texIn.pTexture, texInterpOut.pTexture, w, h), "CreateFeature 4K");
	FillInput(dev, ctx, texIn.pTexture, 0.5f);
	Check(fg.Evaluate(params), "Evaluate 4K");

	Head("Seek / Reset");
	params.bReset = true;
	FillInput(dev, ctx, texIn.pTexture, 0.6f);
	Check(fg.Evaluate(params), "Evaluate with Reset = true");
	params.bReset = false;
	FillInput(dev, ctx, texIn.pTexture, 0.7f);
	Check(fg.Evaluate(params), "Evaluate after Reset");

	Head("Teardown & Re-init");
	fg.Shutdown();
	Check(SUCCEEDED(dev->GetDeviceRemovedReason()), "device survived Shutdown");

	Check(fg.Init(dev, nullptr), "Init after Shutdown");
	w = 1280; h = 720;
	Check(MakeSharedPair(dev, texIn, texInterpOut, w, h), "720p shared pair");
	Check(fg.CreateFeature(texIn.pTexture, texInterpOut.pTexture, w, h), "CreateFeature 720p");
	FillInput(dev, ctx, texIn.pTexture, 0.5f);
	Check(fg.Evaluate(params), "Evaluate 720p without MVec");

	Head("MVec Swap Test");
	Check(fg.SetGuides(texInterpOut.pTexture), "SetGuides with valid MVec");
	FillInput(dev, ctx, texIn.pTexture, 0.6f);
	Check(fg.Evaluate(params), "Evaluate with new MVec");

	Check(fg.SetGuides(nullptr), "SetGuides to nullptr");
	FillInput(dev, ctx, texIn.pTexture, 0.7f);
	Check(fg.Evaluate(params), "Evaluate without MVec again");

	Head("Player Seek Simulation");
	params.bReset = true;
	Check(fg.SetGuides(nullptr), "Seek Frame 1: SetGuides(nullptr)");
	FillInput(dev, ctx, texIn.pTexture, 0.8f);
	Check(fg.Evaluate(params), "Seek Frame 1: Evaluate (Reset=1, MVec=nullptr)");

	params.bReset = false;
	Check(fg.SetGuides(texInterpOut.pTexture), "Seek Frame 2: SetGuides(valid)");
	FillInput(dev, ctx, texIn.pTexture, 0.9f);
	Check(fg.Evaluate(params), "Seek Frame 2: Evaluate (Reset=0, MVec=valid)");

	Head("Multiplier Scaling (3x and 4x)");
	// 3x simulation
	params.bReset = true;
	FillInput(dev, ctx, texIn.pTexture, 0.1f);
	params.iMultiplier = 3;
	params.iIndex = 1;
	Check(fg.Evaluate(params), "Evaluate 3x Frame 1 (Reset=1)");
	params.bReset = false;
	FillInput(dev, ctx, texIn.pTexture, 0.2f);
	params.iIndex = 1;
	Check(fg.Evaluate(params), "Evaluate 3x Frame 2 (Index=1)");
	params.iIndex = 2;
	Check(fg.Evaluate(params), "Evaluate 3x Frame 2 (Index=2)");

	// 4x simulation
	params.bReset = true;
	FillInput(dev, ctx, texIn.pTexture, 0.3f);
	params.iMultiplier = 4;
	params.iIndex = 1;
	Check(fg.Evaluate(params), "Evaluate 4x Frame 1 (Reset=1)");
	params.bReset = false;
	FillInput(dev, ctx, texIn.pTexture, 0.4f);
	params.iIndex = 1;
	Check(fg.Evaluate(params), "Evaluate 4x Frame 2 (Index=1)");
	params.iIndex = 2;
	Check(fg.Evaluate(params), "Evaluate 4x Frame 2 (Index=2)");
	params.iIndex = 3;
	Check(fg.Evaluate(params), "Evaluate 4x Frame 2 (Index=3)");

	Head("Resolution Change Simulation");
	texIn.Release();
	texInterpOut.Release();
	Check(MakeSharedPair(dev, texIn, texInterpOut, w, h), "Recreate shared pair");
	Check(fg.CreateFeature(texIn.pTexture, texInterpOut.pTexture, w, h), "CreateFeature again");
	FillInput(dev, ctx, texIn.pTexture, 0.4f);
	params.bReset = true;
	Check(fg.Evaluate(params), "Evaluate after recreate (Reset=1)");
	params.bReset = false;
	FillInput(dev, ctx, texIn.pTexture, 0.5f);
	Check(fg.Evaluate(params), "Evaluate after recreate (Reset=0)");

	Head("SwapChain ResizeBuffers & Auto-Recovery");
	WNDCLASSW wcDummy = {};
	wcDummy.lpfnWndProc = DefWindowProcW;
	wcDummy.hInstance = GetModuleHandleW(nullptr);
	wcDummy.lpszClassName = L"DlssFgHarnessDummy";
	RegisterClassW(&wcDummy);
	HWND hwndDummy = CreateWindowExW(0, wcDummy.lpszClassName, L"Dummy", WS_POPUP, 0, 0, 1280, 720, nullptr, nullptr, nullptr, nullptr);

	DXGI_SWAP_CHAIN_DESC scd = {};
	scd.BufferCount = 2;
	scd.BufferDesc.Width = 1280;
	scd.BufferDesc.Height = 720;
	scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	scd.OutputWindow = hwndDummy;
	scd.SampleDesc.Count = 1;
	scd.Windowed = TRUE;
	scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

	CComPtr<IDXGISwapChain> sc;
	HRESULT hrSc = factory->CreateSwapChain(dev, &scd, &sc);
	Check(SUCCEEDED(hrSc), "CreateSwapChain for dummy window");
	if (sc) {
		FillInput(dev, ctx, texIn.pTexture, 0.5f);
		Check(fg.Evaluate(params), "Evaluate before ResizeBuffers");

		// Resize swapchain buffers (e.g. 1280x720 -> 1920x1080)
		hrSc = sc->ResizeBuffers(0, 1920, 1080, DXGI_FORMAT_UNKNOWN, 0);
		Check(SUCCEEDED(hrSc), "ResizeBuffers 1280x720 -> 1920x1080");

		// Immediate evaluate after ResizeBuffers -- if NGX experiences transient boundary error,
		// verify the auto-recovery path (ReleaseFeature + CreateFeature) heals the session immediately.
		FillInput(dev, ctx, texIn.pTexture, 0.55f);
		bool bEvalAfterResize = fg.Evaluate(params);
		if (!bEvalAfterResize) {
			fg.ReleaseFeature();
			Check(fg.CreateFeature(texIn.pTexture, texInterpOut.pTexture, w, h), "Recreate feature on transient boundary failure");
			params.bReset = true;
			bEvalAfterResize = fg.Evaluate(params);
			params.bReset = false;
		}
		Check(bEvalAfterResize, "Evaluate succeeded on / recovered from swapchain resize");

		// Continuous evaluation post-resize
		int scEvalOk = 0;
		for (int f = 0; f < 5; f++) {
			FillInput(dev, ctx, texIn.pTexture, 0.6f + f * 0.05f);
			if (fg.Evaluate(params) && OutputIsNonZero(dev, ctx, texInterpOut.pTexture)) {
				scEvalOk++;
			}
		}
		Check(scEvalOk == 5, "5 consecutive frames evaluated post-resize with non-zero output");

		// Downscale resize
		hrSc = sc->ResizeBuffers(0, 960, 540, DXGI_FORMAT_UNKNOWN, 0);
		Check(SUCCEEDED(hrSc), "ResizeBuffers 1920x1080 -> 960x540");
		FillInput(dev, ctx, texIn.pTexture, 0.85f);
		bool bEvalDownscale = fg.Evaluate(params);
		if (!bEvalDownscale) {
			fg.ReleaseFeature();
			Check(fg.CreateFeature(texIn.pTexture, texInterpOut.pTexture, w, h), "Recreate feature on downscale");
			params.bReset = true;
			bEvalDownscale = fg.Evaluate(params);
			params.bReset = false;
		}
		Check(bEvalDownscale && OutputIsNonZero(dev, ctx, texInterpOut.pTexture), "Post-downscale evaluation valid and non-zero");
		sc.Release();
	}
	if (hwndDummy) {
		DestroyWindow(hwndDummy);
	}

	Head("Transient Failure Self-Healing Recovery Test");
	// Explicitly test the auto-recovery logic: ReleaseFeature + CreateFeature resets state and restores Ready
	fg.ReleaseFeature();
	Check(!fg.IsFeatureReady(), "Feature released (not ready)");
	Check(fg.CreateFeature(texIn.pTexture, texInterpOut.pTexture, w, h), "CreateFeature self-heal");
	Check(fg.IsFeatureReady(), "Feature restored to Ready");
	params.bReset = true;
	FillInput(dev, ctx, texIn.pTexture, 0.1f);
	Check(fg.Evaluate(params), "First frame after heal evaluated (Reset=1)");
	params.bReset = false;
	FillInput(dev, ctx, texIn.pTexture, 0.2f);
	Check(fg.Evaluate(params), "Second frame evaluated (Reset=0)");
	Check(OutputIsNonZero(dev, ctx, texInterpOut.pTexture), "Output is non-zero after self-heal");

	fg.Shutdown();

	Head("Result");
	printf("  %d check(s) failed\n", g_failures);
	return g_failures ? 1 : 0;
}
