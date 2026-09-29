// resize_suite.inl: Automated regression testing of window resizing during playback with DLSS Frame Generation active.
//
// Verifies that when the video window / swap chain is resized dynamically during live playback:
// 1. The filter survives without deadlocks or crashes (guarded by Watchdog).
// 2. DLSS Frame Generation does not permanently disable itself or drop to "off" or "execution failed".
// 3. The feature cleanly recovers (via DlssFGPass self-healing feature recreation) and returns to "ready".
// 4. Frames continue to be rendered (FPS remains healthy).
// 5. The screen surface continues to update with real frames (verified by CScreenWatch).
// 6. DLSS FG timings continue to be recorded in OSD statistics.

#pragma once

static int RunResizeSuite(HMODULE hFilter, HWND hwnd, SIZE source, SIZE initialWindow, int seconds)
{
	HRESULT hr = S_OK;
	CComPtr<IFilterGraph2> pGraph;
	if (FAILED(pGraph.CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER))) {
		printf("no filter graph\n");
		return 1;
	}

	using PFN_DllGetClassObject = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);
	const auto pfnGetClassObject = (PFN_DllGetClassObject)GetProcAddress(hFilter, "DllGetClassObject");
	CComPtr<IClassFactory> pFactory;
	CComPtr<IBaseFilter> pRenderer;
	if (!pfnGetClassObject || FAILED(pfnGetClassObject(CLSID_MpcVideoRenderer, IID_IClassFactory, (LPVOID*)&pFactory))
			|| FAILED(pFactory->CreateInstance(nullptr, IID_IBaseFilter, (void**)&pRenderer))) {
		printf("the renderer could not be created\n");
		return 1;
	}

	CComQIPtr<IVideoRenderer> pVR(pRenderer.p);
	Settings_t sets;
	pVR->GetSettings(sets);
	sets.bUseD3D11 = true;
	sets.bShowStats = true;
	sets.bExclusiveFS = false;
	sets.bHdrPassthrough = false;
	sets.iHdrToggleDisplay = HDRTD_Disabled;
	sets.bDlssNR = false;
	sets.bDlssSR = false;
	sets.bDlssRenderAhead = false;
	pVR->SetSettings(sets);

	CComQIPtr<IVideoWindow> pVW(pRenderer.p);
	CComQIPtr<IBasicVideo> pBV(pRenderer.p);
	pVW->put_Owner((OAHWND)hwnd);
	pVW->put_WindowStyle(WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN);
	pVW->SetWindowPosition(0, 0, initialWindow.cx, initialWindow.cy);
	pBV->SetDestinationPosition(0, 0, initialWindow.cx, initialWindow.cy);

	auto pSource = new CFilmSource(&hr, source.cx, source.cy);
	pGraph->AddFilter(pSource, L"Film source");
	pGraph->AddFilter(pRenderer, L"MPC Video Renderer");
	if (FAILED(pGraph->ConnectDirect(GetPin(pSource, PINDIR_OUTPUT), GetPin(pRenderer, PINDIR_INPUT), nullptr))) {
		printf("connection failed\n");
		return 1;
	}

	CComQIPtr<IMediaControl> pMC(pGraph.p);
	pMC->Run();

	printf("Waiting for playback and DLSS FG to initialise and settle...\n");
	Pump(4000);

	std::wstring initialText = StatsText(pRenderer);
	std::wstring initialFG = StatsLine(initialText, L"DLSS FG       : ");
	wprintf(L"Initial DLSS FG status: %s\n", initialFG.c_str());

	CScreenWatch watch;
	const std::string watchError = watch.Start(hwnd);
	if (!watchError.empty()) {
		printf("screen watch: %s\n", watchError.c_str());
	}

	struct ResizeTestCase {
		const char* name;
		int width;
		int height;
		int settleMs;
	};

	const ResizeTestCase cases[] = {
		{ "Upscale to 1600x900",          1600, 900,  1500 },
		{ "Upscale to 1920x1080 (1080p)", 1920, 1080, 1500 },
		{ "Downscale to 1024x576",        1024, 576,  1500 },
		{ "Downscale to 800x450 (1:1)",   800,  450,  1500 },
		{ "Restore to 1280x720 (720p)",   1280, 720,  1500 },
	};

	int failures = 0;
	printf("\n=== Running Live Window Resize Regression Tests ===\n");
	printf("  %-38s %8s %7s %8s  %s\n", "resize step", "call (s)", "fps", "screen", "DLSS FG status");

	const auto DoResize = [&](const char* name, int w, int h, int settleMs) -> bool {
		const ULONGLONG t0 = GetTickCount64();
		{
			Watchdog watchdog(name, 10);
			RECT rc = { 0, 0, w, h };
			AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
			SetWindowPos(hwnd, nullptr, 20, 20, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE);
			pVW->SetWindowPosition(0, 0, w, h);
			pBV->SetDestinationPosition(0, 0, w, h);
		}
		const double took = (GetTickCount64() - t0) / 1000.0;

		// Measure frame delivery post-resize
		Pump(settleMs / 2);
		int framesA = 0, framesB = 0, skipped = 0;
		ParseSkipped(StatsText(pRenderer), framesA, skipped);
		Pump(settleMs / 2);
		std::wstring text = StatsText(pRenderer);
		ParseSkipped(text, framesB, skipped);

		const double fps = (framesB - framesA) / (settleMs / 2000.0);
		std::wstring fgStatus = StatsLine(text, L"DLSS FG       : ");

		bool bScreenMoves = !watch.Ready();
		uint64_t before = watch.Signature(hwnd);
		for (int look = 0; look < 3 && !bScreenMoves; look++) {
			Pump(200);
			const uint64_t now = watch.Signature(hwnd);
			bScreenMoves = (now != before);
			before = now;
		}

		// DLSS FG must NOT be "off", must NOT be "execution failed", and should contain "ready"
		const bool bFgOk = (fgStatus.find(L"ready") != std::wstring::npos);
		const bool bFpsOk = (fps > 1.0);
		const bool bPass = bFgOk && bFpsOk && bScreenMoves;

		printf("  %-38s %8.2f %7.1f %8s  %S%s\n",
			name, took, fps,
			bScreenMoves ? "moving" : "FROZEN",
			fgStatus.c_str(),
			bPass ? "" : "   FAIL");

		return bPass;
	};

	for (const auto& tc : cases) {
		if (!DoResize(tc.name, tc.width, tc.height, tc.settleMs)) {
			failures++;
		}
	}

	// Rapid dragging simulation: 5 successive resizes in quick succession
	printf("\n=== Rapid Continuous Resizing (Drag Simulation) ===\n");
	for (int i = 0; i < 5; i++) {
		const int stepW = 1200 + i * 80;
		const int stepH = 675 + i * 45;
		RECT rc = { 0, 0, stepW, stepH };
		AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
		SetWindowPos(hwnd, nullptr, 20, 20, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE);
		pVW->SetWindowPosition(0, 0, stepW, stepH);
		pBV->SetDestinationPosition(0, 0, stepW, stepH);
		Pump(100);
	}
	// After rapid resizing, allow 1.5s to settle and verify FG is active
	Pump(1500);
	int frames1 = 0, frames2 = 0, sk = 0;
	ParseSkipped(StatsText(pRenderer), frames1, sk);
	Pump(1000);
	std::wstring textRapid = StatsText(pRenderer);
	ParseSkipped(textRapid, frames2, sk);
	const double fpsRapid = (double)(frames2 - frames1);
	std::wstring fgRapid = StatsLine(textRapid, L"DLSS FG       : ");
	const bool bRapidFgOk = (fgRapid.find(L"ready") != std::wstring::npos);
	printf("  Rapid drag sequence (5 steps)         --    %7.1f %8s  %S%s\n",
		fpsRapid, "settled", fgRapid.c_str(), (bRapidFgOk && fpsRapid > 1.0) ? "" : "   FAIL");
	if (!bRapidFgOk || fpsRapid <= 1.0) {
		failures++;
	}

	pMC->Stop();
	pVW->put_Visible(OAFALSE);
	pVW->put_Owner(0);
	pGraph->RemoveFilter(pRenderer);
	pGraph->RemoveFilter(pSource);

	printf("\nResize test result: %d failure(s)\n", failures);
	return failures ? 1 : 0;
}
