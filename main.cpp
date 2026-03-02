// ==========================================================================
// PartitionDemo - CustomDataPartitionId + NewWindowRequested Demo
//
// 7 Scenarios, each FULLY SELF-CONTAINED:
//
// --- New WebView scenarios (child is a NEW WebView): ---
//   1. No partition set on child
//   2. Same partition as parent ("demoPartition")
//   3. Different partition ("otherPartition")
//   4. Default/empty partition ("")
//
// --- Parent Reuse scenarios (parent WebView passed to put_NewWindow): ---
//   5. Inherits parent partition (no change before put_NewWindow)
//   6. Reset to default ("") before put_NewWindow
//   7. Own custom partition ("otherPartition") set before put_NewWindow
//
// ==========================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlwapi.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <wrl.h>
#include <wrl/event.h>

#include "WebView2.h"
#include "WebView2EnvironmentOptions.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")

using namespace Microsoft::WRL;

static void Log(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vprintf(fmt, args);
  va_end(args);
}

// ==========================================================================
// Experimental API - CustomDataPartitionId
// ==========================================================================
MIDL_INTERFACE("5a4d0ecf-3fe5-4456-ace5-d317cca0eff1")
IPartitionApi : public IUnknown {
 public:
  virtual HRESULT STDMETHODCALLTYPE
  get_CustomDataPartitionId(LPWSTR* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE
  put_CustomDataPartitionId(LPCWSTR value) = 0;
};

static HRESULT SetPartition(ICoreWebView2* wv, LPCWSTR id) {
  ComPtr<IPartitionApi> api;
  HRESULT hr = wv->QueryInterface(__uuidof(IPartitionApi), &api);
  return SUCCEEDED(hr) ? api->put_CustomDataPartitionId(id) : hr;
}

static std::wstring GetPartition(ICoreWebView2* wv) {
  ComPtr<IPartitionApi> api;
  if (FAILED(wv->QueryInterface(__uuidof(IPartitionApi), &api)))
    return L"(unsupported)";
  LPWSTR v = nullptr;
  api->get_CustomDataPartitionId(&v);
  std::wstring r = v ? v : L"";
  CoTaskMemFree(v);
  return r.empty() ? L"(default)" : r;
}

// ==========================================================================
// Per-scenario state
// ==========================================================================
struct ScenarioCtx {
  int num;
  HWND parentHwnd = nullptr;
  ComPtr<ICoreWebView2Controller> parentCtrl;
  ComPtr<ICoreWebView2> parentWv;
  HWND childHwnd = nullptr;
  ComPtr<ICoreWebView2Controller> childCtrl;
  ComPtr<ICoreWebView2> childWv;
  std::wstring pendingScript;
  bool isParentReuse = false;
  bool step2Done = false;  // Guard: NavigationCompleted runs only once
};
static std::vector<ScenarioCtx*> g_scenarios;

static ScenarioCtx* FindCtxByHwnd(HWND hwnd) {
  for (auto* s : g_scenarios)
    if (s->parentHwnd == hwnd || s->childHwnd == hwnd)
      return s;
  return nullptr;
}

// ==========================================================================
// Globals
// ==========================================================================
static const wchar_t kClassName[] = L"PartitionDemoWnd";
static HINSTANCE g_hInst;
static HWND g_launcherHwnd = nullptr;
static ComPtr<ICoreWebView2Environment> g_env;
static ComPtr<ICoreWebView2Controller> g_launcherCtrl;
static ComPtr<ICoreWebView2> g_launcherWv;
static const UINT_PTR kInjectTimer = 42;

static HWND CreateHostWindow(const wchar_t* title, int w, int h, bool show) {
  HWND hwnd = CreateWindowExW(0, kClassName, title, WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, w, h, nullptr,
                              nullptr, g_hInst, nullptr);
  if (show) ShowWindow(hwnd, SW_SHOW);
  return hwnd;
}

// ==========================================================================
// JS single-quote escaper — prevents ' in dynamic strings from breaking h+='...'
// ==========================================================================
static std::wstring EscJS(const std::wstring& s) {
  std::wstring out;
  out.reserve(s.size() + 16);
  for (wchar_t c : s) {
    if (c == L'\'') out += L"\\x27";
    else if (c == L'\\') out += L"\\\\";
    else out += c;
  }
  return out;
}

// ==========================================================================
// Result page builder
// ==========================================================================
static std::wstring BuildResultScript(int n, const wchar_t* title,
                                       const std::wstring& desc,
                                       const std::wstring& parentApiPart,
                                       const std::wstring& popupPart,
                                       const std::wstring& parentEffOverride = L"") {
  // parentEffOverride: if non-empty, use it as parent effective partition
  // (e.g. for "partition before nav" scenarios where partition IS applied).
  // Otherwise default to (default).
  std::wstring parentEff = parentEffOverride.empty() ? L"(default)" : parentEffOverride;
  std::wstring popupEff = (popupPart.empty() || popupPart == L"(default)")
      ? L"(default)" : popupPart;
  bool match = (popupEff == parentEff);
  std::wstring safeTitle = EscJS(title);
  std::wstring safeDesc = EscJS(desc);
  std::wstring safePopupEff = EscJS(popupEff);
  std::wstring safeParentEff = EscJS(parentEff);
  std::wstring js;
  js += L"(function(){\n";
  js += L"document.documentElement.style.cssText='margin:0;padding:0;width:100%;height:100%';\n";
  js += L"document.body.style.cssText='margin:0;padding:30px;background:#1e1e2e;color:#cdd6f4;font-family:Segoe UI,sans-serif;max-width:650px;min-height:100%';\n";
  js += L"document.title='Scenario " + std::to_wstring(n) + L"';\n";
  js += L"document.head.innerHTML='<style>*{box-sizing:border-box;margin:0;padding:0}body{font-family:Segoe UI,sans-serif;background:#1e1e2e;color:#cdd6f4;padding:30px;max-width:650px}h1{font-size:20px;margin-bottom:6px;color:#89b4fa}.desc{background:#313244;border-radius:8px;padding:14px 18px;margin:14px 0;font-size:13px;line-height:1.7}.desc b{color:#f9e2af}.pbox{background:#181825;border:1px solid #45475a;border-radius:8px;padding:14px 18px;margin:14px 0}.row{display:flex;justify-content:space-between;margin:4px 0;font-size:13px}.lbl{color:#a6adc8}.val{font-family:Cascadia Code,monospace;font-weight:600}.res{border-radius:8px;padding:14px;margin:10px 0;font-size:14px;font-weight:600;text-align:center}.ok{background:#1a3a2a;border:2px solid #a6e3a1;color:#a6e3a1}.fail{background:#3a1a1a;border:2px solid #f38ba8;color:#f38ba8}</style>';\n";
  js += L"var h='<h1>Scenario " + std::to_wstring(n) + L": " + safeTitle + L"</h1>';\n";
  js += L"h+='<div class=\"desc\">" + safeDesc + L"</div>';\n";
  js += L"h+='<div class=\"pbox\">';\n";
  js += L"h+='<div class=\"row\"><span class=\"lbl\">Parent effective partition:</span><span class=\"val\" style=\"color:#89b4fa\">" + safeParentEff + L"</span></div>';\n";
  js += L"h+='<div class=\"row\"><span class=\"lbl\">Popup effective partition:</span><span class=\"val\" style=\"color:" + std::wstring(match?L"#a6e3a1":L"#f38ba8") +L"\">" + safePopupEff + L"</span></div>';\n";
  js += L"h+='<div class=\"row\"><span class=\"lbl\">Match:</span><span class=\"val\" style=\"color:" + std::wstring(match?L"#a6e3a1":L"#f38ba8") +L"\">" + std::wstring(match?L"\\u2705 YES":L"\\u274C NO") + L"</span></div>';\n";
  js += L"h+='</div>';\n";
  if (match) {
    js += L"h+='<div class=\"res ok\">\\u2705 Same effective partition - blob URLs WORK.</div>';\n";
  } else {
    js += L"h+='<div class=\"res fail\">\\u274C Different effective partition - blob URLs FAIL.</div>';\n";
  }
  js += L"document.body.innerHTML=h;\n})();\n";
  return js;
}

// Forward declarations
static void RunScenario1();
static void RunScenario2();
static void RunScenario3();
static void RunScenario4();
static void RunScenario5();
static void RunScenario6();
static void RunScenario7();
static void RunScenario8();
static void RunScenario9();
static void RunScenario10();
static void RunScenario11();
static void RunScenario12();
static void RunScenario13();
static void RunScenario14();
static void RunScenario15();
static void RunScenario16();
static void RunScenario17();
static void RunScenario18();
static void RunScenario19();
static void RunScenario20();
static void RunScenario21();

// ==========================================================================
// Window Procedure
// ==========================================================================
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_TIMER: {
      if (wp == kInjectTimer) {
        KillTimer(hwnd, kInjectTimer);
        auto* ctx = FindCtxByHwnd(hwnd);
        if (ctx && !ctx->pendingScript.empty()) {
          ICoreWebView2* target = ctx->isParentReuse
              ? ctx->parentWv.Get() : ctx->childWv.Get();
          ICoreWebView2Controller* ctrl = ctx->isParentReuse
              ? ctx->parentCtrl.Get() : ctx->childCtrl.Get();
          if (target && ctrl) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            ctrl->put_Bounds(rc);
            ctrl->put_IsVisible(TRUE);
            Log("[STEP 5] Scenario %d: injecting result UI\n", ctx->num);
            target->ExecuteScript(ctx->pendingScript.c_str(), nullptr);
            ctx->pendingScript.clear();
          }
        }
      }
      return 0;
    }
    case WM_SIZE: {
      RECT rc;
      GetClientRect(hwnd, &rc);
      if (hwnd == g_launcherHwnd && g_launcherCtrl)
        g_launcherCtrl->put_Bounds(rc);
      auto* ctx = FindCtxByHwnd(hwnd);
      if (ctx) {
        if (hwnd == ctx->parentHwnd && ctx->parentCtrl)
          ctx->parentCtrl->put_Bounds(rc);
        if (hwnd == ctx->childHwnd && ctx->childCtrl)
          ctx->childCtrl->put_Bounds(rc);
      }
      return 0;
    }
    case WM_CLOSE:
      if (hwnd == g_launcherHwnd) {
        for (auto* s : g_scenarios) {
          if (s->parentCtrl) s->parentCtrl->Close();
          if (s->childCtrl) s->childCtrl->Close();
          if (s->parentHwnd) DestroyWindow(s->parentHwnd);
          if (s->childHwnd) DestroyWindow(s->childHwnd);
          delete s;
        }
        g_scenarios.clear();
        DestroyWindow(hwnd);
      } else {
        auto* ctx = FindCtxByHwnd(hwnd);
        if (ctx) {
          if (ctx->parentCtrl) ctx->parentCtrl->Close();
          if (ctx->childCtrl) ctx->childCtrl->Close();
          if (ctx->parentHwnd && ctx->parentHwnd != hwnd)
            DestroyWindow(ctx->parentHwnd);
          if (ctx->childHwnd && ctx->childHwnd != hwnd)
            DestroyWindow(ctx->childHwnd);
          for (auto it = g_scenarios.begin(); it != g_scenarios.end(); ++it) {
            if (*it == ctx) { delete ctx; g_scenarios.erase(it); break; }
          }
        }
        DestroyWindow(hwnd);
      }
      return 0;
    case WM_DESTROY:
      if (hwnd == g_launcherHwnd) PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

// ==========================================================================
// SCENARIO 1: New WebView | No Partition
// ==========================================================================
static void RunScenario1() {
  Log("\n[SCENARIO 1] === New WebView | No Partition ===\n");
  auto* ctx = new ScenarioCtx();
  ctx->num = 1;
  g_scenarios.push_back(ctx);

  // --- STEP 1: Create parent WebView ---
  ctx->parentHwnd = CreateHostWindow(
      L"S1 Parent: demoPartition", 400, 100, true);

  g_env->CreateCoreWebView2Controller(
      ctx->parentHwnd,
      Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
          [ctx](HRESULT hr, ICoreWebView2Controller* ctrl) -> HRESULT {
            if (FAILED(hr)) return hr;
            ctx->parentCtrl = ctrl;
            ctrl->get_CoreWebView2(&ctx->parentWv);
            RECT rc; GetClientRect(ctx->parentHwnd, &rc);
            ctrl->put_Bounds(rc);
            Log("[STEP 1] S1: Parent WebView created\n");

            EventRegistrationToken navTok;
            ctx->parentWv->add_NavigationCompleted(
                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                    [ctx](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                      if (ctx->step2Done) return S_OK;  // Guard
                      ctx->step2Done = true;

                      // --- STEP 2: Set partition on parent ---
                      Log("[STEP 2] S1: Setting partition demoPartition on parent\n");
                      SetPartition(ctx->parentWv.Get(), L"demoPartition");

                      // --- Register NWR handler ---
                      EventRegistrationToken nwrTok;
                      ctx->parentWv->add_NewWindowRequested(
                          Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                              [ctx](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                Log("[STEP 4] S1: NewWindowRequested fired\n");

                                ctx->childHwnd = CreateHostWindow(
                                    L"Scenario 1 Result: New WebView | No Partition", 750, 600, false);

                                ComPtr<ICoreWebView2Deferral> deferral;
                                args->GetDeferral(&deferral);

                                g_env->CreateCoreWebView2Controller(
                                    ctx->childHwnd,
                                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                        [ctx, args, deferral](HRESULT hr, ICoreWebView2Controller* childCtrl) -> HRESULT {
                                          if (FAILED(hr)) return hr;
                                          ctx->childCtrl = childCtrl;
                                          childCtrl->get_CoreWebView2(&ctx->childWv);

                                          ComPtr<ICoreWebView2Settings> settings;
                                          ctx->childWv->get_Settings(&settings);
                                          if (settings) settings->put_AreDevToolsEnabled(TRUE);

                                          // *** NO partition set on child ***
                                          Log("[STEP 4] S1: Child created, NO partition set\n");

                                          std::wstring parentPart = GetPartition(ctx->parentWv.Get());
                                          std::wstring childPart = GetPartition(ctx->childWv.Get());
                                          Log("[STEP 4] S1: parent=%ls child=%ls\n", parentPart.c_str(), childPart.c_str());

                                          ctx->pendingScript = BuildResultScript(1,
                                              L"New WebView | No Partition",
                                              L"A <b>new</b> WebView created for popup. <b>No partition set</b>. Tests whether child inherits parent\'s partition via put_NewWindow.",
                                              parentPart, childPart);

                                          args->put_NewWindow(ctx->childWv.Get());
                                          args->put_Handled(TRUE);
                                          deferral->Complete();

                                          ShowWindow(ctx->childHwnd, SW_SHOW);
                                          SetForegroundWindow(ctx->childHwnd);
                                          SetTimer(ctx->childHwnd, kInjectTimer, 300, nullptr);
                                          return S_OK;
                                        }).Get());
                                return S_OK;
                              }).Get(), &nwrTok);

                      // --- STEP 3: Trigger window.open ---
                      Log("[STEP 3] S1: Calling window.open\n");
                      ctx->parentWv->ExecuteScript(L"window.open('about:blank')", nullptr);
                      return S_OK;
                    }).Get(), &navTok);

            ctx->parentWv->Navigate(L"about:blank");
            return S_OK;
          }).Get());
}

// ==========================================================================
// SCENARIO 2: New WebView | Same Partition
// ==========================================================================
static void RunScenario2() {
  Log("\n[SCENARIO 2] === New WebView | Same Partition ===\n");
  auto* ctx = new ScenarioCtx();
  ctx->num = 2;
  g_scenarios.push_back(ctx);

  // --- STEP 1: Create parent WebView ---
  ctx->parentHwnd = CreateHostWindow(
      L"S2 Parent: demoPartition", 400, 100, true);

  g_env->CreateCoreWebView2Controller(
      ctx->parentHwnd,
      Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
          [ctx](HRESULT hr, ICoreWebView2Controller* ctrl) -> HRESULT {
            if (FAILED(hr)) return hr;
            ctx->parentCtrl = ctrl;
            ctrl->get_CoreWebView2(&ctx->parentWv);
            RECT rc; GetClientRect(ctx->parentHwnd, &rc);
            ctrl->put_Bounds(rc);
            Log("[STEP 1] S2: Parent WebView created\n");

            EventRegistrationToken navTok;
            ctx->parentWv->add_NavigationCompleted(
                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                    [ctx](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                      if (ctx->step2Done) return S_OK;  // Guard
                      ctx->step2Done = true;

                      // --- STEP 2: Set partition on parent ---
                      Log("[STEP 2] S2: Setting partition demoPartition on parent\n");
                      SetPartition(ctx->parentWv.Get(), L"demoPartition");

                      // --- Register NWR handler ---
                      EventRegistrationToken nwrTok;
                      ctx->parentWv->add_NewWindowRequested(
                          Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                              [ctx](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                Log("[STEP 4] S2: NewWindowRequested fired\n");

                                ctx->childHwnd = CreateHostWindow(
                                    L"Scenario 2 Result: New WebView | Same Partition", 750, 600, false);

                                ComPtr<ICoreWebView2Deferral> deferral;
                                args->GetDeferral(&deferral);

                                g_env->CreateCoreWebView2Controller(
                                    ctx->childHwnd,
                                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                        [ctx, args, deferral](HRESULT hr, ICoreWebView2Controller* childCtrl) -> HRESULT {
                                          if (FAILED(hr)) return hr;
                                          ctx->childCtrl = childCtrl;
                                          childCtrl->get_CoreWebView2(&ctx->childWv);

                                          ComPtr<ICoreWebView2Settings> settings;
                                          ctx->childWv->get_Settings(&settings);
                                          if (settings) settings->put_AreDevToolsEnabled(TRUE);

                                          // *** Set partition: demoPartition ***
                                          SetPartition(ctx->childWv.Get(), L"demoPartition");
                                          Log("[STEP 4] S2: Child partition set to demoPartition\n");

                                          std::wstring parentPart = GetPartition(ctx->parentWv.Get());
                                          std::wstring childPart = GetPartition(ctx->childWv.Get());
                                          Log("[STEP 4] S2: parent=%ls child=%ls\n", parentPart.c_str(), childPart.c_str());

                                          ctx->pendingScript = BuildResultScript(2,
                                              L"New WebView | Same Partition",
                                              L"A <b>new</b> WebView created. <b>put_CustomDataPartitionId(demoPartition)</b> explicitly called - same as parent.",
                                              parentPart, childPart);

                                          args->put_NewWindow(ctx->childWv.Get());
                                          args->put_Handled(TRUE);
                                          deferral->Complete();

                                          ShowWindow(ctx->childHwnd, SW_SHOW);
                                          SetForegroundWindow(ctx->childHwnd);
                                          SetTimer(ctx->childHwnd, kInjectTimer, 300, nullptr);
                                          return S_OK;
                                        }).Get());
                                return S_OK;
                              }).Get(), &nwrTok);

                      // --- STEP 3: Trigger window.open ---
                      Log("[STEP 3] S2: Calling window.open\n");
                      ctx->parentWv->ExecuteScript(L"window.open('about:blank')", nullptr);
                      return S_OK;
                    }).Get(), &navTok);

            ctx->parentWv->Navigate(L"about:blank");
            return S_OK;
          }).Get());
}

// ==========================================================================
// SCENARIO 3: New WebView | Different Partition
// ==========================================================================
static void RunScenario3() {
  Log("\n[SCENARIO 3] === New WebView | Different Partition ===\n");
  auto* ctx = new ScenarioCtx();
  ctx->num = 3;
  g_scenarios.push_back(ctx);

  // --- STEP 1: Create parent WebView ---
  ctx->parentHwnd = CreateHostWindow(
      L"S3 Parent: demoPartition", 400, 100, true);

  g_env->CreateCoreWebView2Controller(
      ctx->parentHwnd,
      Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
          [ctx](HRESULT hr, ICoreWebView2Controller* ctrl) -> HRESULT {
            if (FAILED(hr)) return hr;
            ctx->parentCtrl = ctrl;
            ctrl->get_CoreWebView2(&ctx->parentWv);
            RECT rc; GetClientRect(ctx->parentHwnd, &rc);
            ctrl->put_Bounds(rc);
            Log("[STEP 1] S3: Parent WebView created\n");

            EventRegistrationToken navTok;
            ctx->parentWv->add_NavigationCompleted(
                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                    [ctx](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                      if (ctx->step2Done) return S_OK;  // Guard
                      ctx->step2Done = true;

                      // --- STEP 2: Set partition on parent ---
                      Log("[STEP 2] S3: Setting partition demoPartition on parent\n");
                      SetPartition(ctx->parentWv.Get(), L"demoPartition");

                      // --- Register NWR handler ---
                      EventRegistrationToken nwrTok;
                      ctx->parentWv->add_NewWindowRequested(
                          Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                              [ctx](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                Log("[STEP 4] S3: NewWindowRequested fired\n");

                                ctx->childHwnd = CreateHostWindow(
                                    L"Scenario 3 Result: New WebView | Different Partition", 750, 600, false);

                                ComPtr<ICoreWebView2Deferral> deferral;
                                args->GetDeferral(&deferral);

                                g_env->CreateCoreWebView2Controller(
                                    ctx->childHwnd,
                                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                        [ctx, args, deferral](HRESULT hr, ICoreWebView2Controller* childCtrl) -> HRESULT {
                                          if (FAILED(hr)) return hr;
                                          ctx->childCtrl = childCtrl;
                                          childCtrl->get_CoreWebView2(&ctx->childWv);

                                          ComPtr<ICoreWebView2Settings> settings;
                                          ctx->childWv->get_Settings(&settings);
                                          if (settings) settings->put_AreDevToolsEnabled(TRUE);

                                          // *** Set partition: otherPartition ***
                                          SetPartition(ctx->childWv.Get(), L"otherPartition");
                                          Log("[STEP 4] S3: Child partition set to otherPartition\n");

                                          std::wstring parentPart = GetPartition(ctx->parentWv.Get());
                                          std::wstring childPart = GetPartition(ctx->childWv.Get());
                                          Log("[STEP 4] S3: parent=%ls child=%ls\n", parentPart.c_str(), childPart.c_str());

                                          ctx->pendingScript = BuildResultScript(3,
                                              L"New WebView | Different Partition",
                                              L"A <b>new</b> WebView created. <b>put_CustomDataPartitionId(otherPartition)</b> - a different partition from parent.",
                                              parentPart, childPart);

                                          args->put_NewWindow(ctx->childWv.Get());
                                          args->put_Handled(TRUE);
                                          deferral->Complete();

                                          ShowWindow(ctx->childHwnd, SW_SHOW);
                                          SetForegroundWindow(ctx->childHwnd);
                                          SetTimer(ctx->childHwnd, kInjectTimer, 300, nullptr);
                                          return S_OK;
                                        }).Get());
                                return S_OK;
                              }).Get(), &nwrTok);

                      // --- STEP 3: Trigger window.open ---
                      Log("[STEP 3] S3: Calling window.open\n");
                      ctx->parentWv->ExecuteScript(L"window.open('about:blank')", nullptr);
                      return S_OK;
                    }).Get(), &navTok);

            ctx->parentWv->Navigate(L"about:blank");
            return S_OK;
          }).Get());
}

// ==========================================================================
// SCENARIO 4: New WebView | Default Partition
// ==========================================================================
static void RunScenario4() {
  Log("\n[SCENARIO 4] === New WebView | Default Partition ===\n");
  auto* ctx = new ScenarioCtx();
  ctx->num = 4;
  g_scenarios.push_back(ctx);

  // --- STEP 1: Create parent WebView ---
  ctx->parentHwnd = CreateHostWindow(
      L"S4 Parent: demoPartition", 400, 100, true);

  g_env->CreateCoreWebView2Controller(
      ctx->parentHwnd,
      Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
          [ctx](HRESULT hr, ICoreWebView2Controller* ctrl) -> HRESULT {
            if (FAILED(hr)) return hr;
            ctx->parentCtrl = ctrl;
            ctrl->get_CoreWebView2(&ctx->parentWv);
            RECT rc; GetClientRect(ctx->parentHwnd, &rc);
            ctrl->put_Bounds(rc);
            Log("[STEP 1] S4: Parent WebView created\n");

            EventRegistrationToken navTok;
            ctx->parentWv->add_NavigationCompleted(
                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                    [ctx](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                      if (ctx->step2Done) return S_OK;  // Guard
                      ctx->step2Done = true;

                      // --- STEP 2: Set partition on parent ---
                      Log("[STEP 2] S4: Setting partition demoPartition on parent\n");
                      SetPartition(ctx->parentWv.Get(), L"demoPartition");

                      // --- Register NWR handler ---
                      EventRegistrationToken nwrTok;
                      ctx->parentWv->add_NewWindowRequested(
                          Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                              [ctx](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                Log("[STEP 4] S4: NewWindowRequested fired\n");

                                ctx->childHwnd = CreateHostWindow(
                                    L"Scenario 4 Result: New WebView | Default Partition", 750, 600, false);

                                ComPtr<ICoreWebView2Deferral> deferral;
                                args->GetDeferral(&deferral);

                                g_env->CreateCoreWebView2Controller(
                                    ctx->childHwnd,
                                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                        [ctx, args, deferral](HRESULT hr, ICoreWebView2Controller* childCtrl) -> HRESULT {
                                          if (FAILED(hr)) return hr;
                                          ctx->childCtrl = childCtrl;
                                          childCtrl->get_CoreWebView2(&ctx->childWv);

                                          ComPtr<ICoreWebView2Settings> settings;
                                          ctx->childWv->get_Settings(&settings);
                                          if (settings) settings->put_AreDevToolsEnabled(TRUE);

                                          // *** Set partition: (default) ***
                                          SetPartition(ctx->childWv.Get(), L"");
                                          Log("[STEP 4] S4: Child partition set to (default)\n");

                                          std::wstring parentPart = GetPartition(ctx->parentWv.Get());
                                          std::wstring childPart = GetPartition(ctx->childWv.Get());
                                          Log("[STEP 4] S4: parent=%ls child=%ls\n", parentPart.c_str(), childPart.c_str());

                                          ctx->pendingScript = BuildResultScript(4,
                                              L"New WebView | Default Partition",
                                              L"A <b>new</b> WebView created. <b>put_CustomDataPartitionId(\'\')</b> - explicitly set to default/empty.",
                                              parentPart, childPart);

                                          args->put_NewWindow(ctx->childWv.Get());
                                          args->put_Handled(TRUE);
                                          deferral->Complete();

                                          ShowWindow(ctx->childHwnd, SW_SHOW);
                                          SetForegroundWindow(ctx->childHwnd);
                                          SetTimer(ctx->childHwnd, kInjectTimer, 300, nullptr);
                                          return S_OK;
                                        }).Get());
                                return S_OK;
                              }).Get(), &nwrTok);

                      // --- STEP 3: Trigger window.open ---
                      Log("[STEP 3] S4: Calling window.open\n");
                      ctx->parentWv->ExecuteScript(L"window.open('about:blank')", nullptr);
                      return S_OK;
                    }).Get(), &navTok);

            ctx->parentWv->Navigate(L"about:blank");
            return S_OK;
          }).Get());
}

// ==========================================================================
// SCENARIO 5: Parent Reused | Inherits Partition
// ==========================================================================
static void RunScenario5() {
  Log("\n[SCENARIO 5] === Parent Reused | Inherits Partition ===\n");
  auto* ctx = new ScenarioCtx();
  ctx->num = 5;
  ctx->isParentReuse = true;
  g_scenarios.push_back(ctx);

  // --- STEP 1: Create parent WebView ---
  ctx->parentHwnd = CreateHostWindow(
      L"Scenario 5: Parent Reused | Inherits Partition", 750, 600, true);

  g_env->CreateCoreWebView2Controller(
      ctx->parentHwnd,
      Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
          [ctx](HRESULT hr, ICoreWebView2Controller* ctrl) -> HRESULT {
            if (FAILED(hr)) return hr;
            ctx->parentCtrl = ctrl;
            ctrl->get_CoreWebView2(&ctx->parentWv);
            RECT rc; GetClientRect(ctx->parentHwnd, &rc);
            ctrl->put_Bounds(rc);
            Log("[STEP 1] S5: Parent WebView created\n");

            EventRegistrationToken navTok;
            ctx->parentWv->add_NavigationCompleted(
                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                    [ctx](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                      if (ctx->step2Done) return S_OK;  // Guard: put_NewWindow can re-trigger nav
                      ctx->step2Done = true;

                      // --- STEP 2: Set partition on parent ---
                      Log("[STEP 2] S5: Setting partition demoPartition on parent\n");
                      SetPartition(ctx->parentWv.Get(), L"demoPartition");

                      // --- Register NWR handler ---
                      EventRegistrationToken nwrTok;
                      ctx->parentWv->add_NewWindowRequested(
                          Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                              [ctx](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                Log("[STEP 4] S5: NewWindowRequested fired\n");

                                // *** No partition change - inherits parent partition ***
                                Log("[STEP 4] S5: No partition change, inheriting parent\n");

                                std::wstring parentPart = GetPartition(ctx->parentWv.Get());
                                Log("[STEP 4] S5: Parent/popup partition = %ls\n", parentPart.c_str());

                                ctx->pendingScript = BuildResultScript(5,
                                    L"Parent Reused | Inherits Partition",
                                    L"The <b>parent</b> WebView was passed to put_NewWindow. No partition change - popup inherits parent\'s <b>demoPartition</b>.",
                                    parentPart, parentPart);

                                args->put_NewWindow(ctx->parentWv.Get());
                                args->put_Handled(TRUE);

                                SetTimer(ctx->parentHwnd, kInjectTimer, 300, nullptr);
                                Log("[STEP 4] S5: put_NewWindow(parent) done\n");
                                return S_OK;
                              }).Get(), &nwrTok);

                      // --- STEP 3: Trigger window.open ---
                      Log("[STEP 3] S5: Calling window.open\n");
                      ctx->parentWv->ExecuteScript(L"window.open('about:blank')", nullptr);
                      return S_OK;
                    }).Get(), &navTok);

            ctx->parentWv->Navigate(L"about:blank");
            return S_OK;
          }).Get());
}

// ==========================================================================
// SCENARIO 6: Parent Reused | Reset to Default
// ==========================================================================
static void RunScenario6() {
  Log("\n[SCENARIO 6] === Parent Reused | Reset to Default ===\n");
  auto* ctx = new ScenarioCtx();
  ctx->num = 6;
  ctx->isParentReuse = true;
  g_scenarios.push_back(ctx);

  // --- STEP 1: Create parent WebView ---
  ctx->parentHwnd = CreateHostWindow(
      L"Scenario 6: Parent Reused | Reset to Default", 750, 600, true);

  g_env->CreateCoreWebView2Controller(
      ctx->parentHwnd,
      Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
          [ctx](HRESULT hr, ICoreWebView2Controller* ctrl) -> HRESULT {
            if (FAILED(hr)) return hr;
            ctx->parentCtrl = ctrl;
            ctrl->get_CoreWebView2(&ctx->parentWv);
            RECT rc; GetClientRect(ctx->parentHwnd, &rc);
            ctrl->put_Bounds(rc);
            Log("[STEP 1] S6: Parent WebView created\n");

            EventRegistrationToken navTok;
            ctx->parentWv->add_NavigationCompleted(
                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                    [ctx](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                      if (ctx->step2Done) return S_OK;  // Guard: put_NewWindow can re-trigger nav
                      ctx->step2Done = true;

                      // --- STEP 2: Set partition on parent ---
                      Log("[STEP 2] S6: Setting partition demoPartition on parent\n");
                      SetPartition(ctx->parentWv.Get(), L"demoPartition");

                      // --- Register NWR handler ---
                      EventRegistrationToken nwrTok;
                      ctx->parentWv->add_NewWindowRequested(
                          Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                              [ctx](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                Log("[STEP 4] S6: NewWindowRequested fired\n");

                                // Parent was at "demoPartition" — capture BEFORE changing
                                std::wstring originalParentPart = L"demoPartition";

                                // *** Change partition before put_NewWindow: (default) ***
                                SetPartition(ctx->parentWv.Get(), L"");
                                Log("[STEP 4] S6: Partition changed to (default)\n");

                                std::wstring popupPart = GetPartition(ctx->parentWv.Get());
                                Log("[STEP 4] S6: Parent was=%ls, popup now=%ls\n", originalParentPart.c_str(), popupPart.c_str());

                                ctx->pendingScript = BuildResultScript(6,
                                    L"Parent Reused | Reset to Default",
                                    L"The <b>parent</b> WebView was passed to put_NewWindow. Partition was <b>reset to default (\'\')</b> before put_NewWindow.",
                                    originalParentPart, popupPart);

                                args->put_NewWindow(ctx->parentWv.Get());
                                args->put_Handled(TRUE);

                                SetTimer(ctx->parentHwnd, kInjectTimer, 300, nullptr);
                                Log("[STEP 4] S6: put_NewWindow(parent) done\n");
                                return S_OK;
                              }).Get(), &nwrTok);

                      // --- STEP 3: Trigger window.open ---
                      Log("[STEP 3] S6: Calling window.open\n");
                      ctx->parentWv->ExecuteScript(L"window.open('about:blank')", nullptr);
                      return S_OK;
                    }).Get(), &navTok);

            ctx->parentWv->Navigate(L"about:blank");
            return S_OK;
          }).Get());
}

// ==========================================================================
// SCENARIO 7: Parent Reused | Own Custom Partition
// ==========================================================================
static void RunScenario7() {
  Log("\n[SCENARIO 7] === Parent Reused | Own Custom Partition ===\n");
  auto* ctx = new ScenarioCtx();
  ctx->num = 7;
  ctx->isParentReuse = true;
  g_scenarios.push_back(ctx);

  // --- STEP 1: Create parent WebView ---
  ctx->parentHwnd = CreateHostWindow(
      L"Scenario 7: Parent Reused | Own Custom Partition", 750, 600, true);

  g_env->CreateCoreWebView2Controller(
      ctx->parentHwnd,
      Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
          [ctx](HRESULT hr, ICoreWebView2Controller* ctrl) -> HRESULT {
            if (FAILED(hr)) return hr;
            ctx->parentCtrl = ctrl;
            ctrl->get_CoreWebView2(&ctx->parentWv);
            RECT rc; GetClientRect(ctx->parentHwnd, &rc);
            ctrl->put_Bounds(rc);
            Log("[STEP 1] S7: Parent WebView created\n");

            EventRegistrationToken navTok;
            ctx->parentWv->add_NavigationCompleted(
                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                    [ctx](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                      if (ctx->step2Done) return S_OK;  // Guard: put_NewWindow can re-trigger nav
                      ctx->step2Done = true;

                      // --- STEP 2: Set partition on parent ---
                      Log("[STEP 2] S7: Setting partition demoPartition on parent\n");
                      SetPartition(ctx->parentWv.Get(), L"demoPartition");

                      // --- Register NWR handler ---
                      EventRegistrationToken nwrTok;
                      ctx->parentWv->add_NewWindowRequested(
                          Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                              [ctx](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                Log("[STEP 4] S7: NewWindowRequested fired\n");

                                // Parent was at "demoPartition" — capture BEFORE changing
                                std::wstring originalParentPart = L"demoPartition";

                                // *** Change partition before put_NewWindow: otherPartition ***
                                SetPartition(ctx->parentWv.Get(), L"otherPartition");
                                Log("[STEP 4] S7: Partition changed to otherPartition\n");

                                std::wstring popupPart = GetPartition(ctx->parentWv.Get());
                                Log("[STEP 4] S7: Parent was=%ls, popup now=%ls\n", originalParentPart.c_str(), popupPart.c_str());

                                ctx->pendingScript = BuildResultScript(7,
                                    L"Parent Reused | Own Custom Partition",
                                    L"The <b>parent</b> WebView was passed to put_NewWindow. Partition was <b>changed to otherPartition</b> before put_NewWindow.",
                                    originalParentPart, popupPart);

                                args->put_NewWindow(ctx->parentWv.Get());
                                args->put_Handled(TRUE);

                                SetTimer(ctx->parentHwnd, kInjectTimer, 300, nullptr);
                                Log("[STEP 4] S7: put_NewWindow(parent) done\n");
                                return S_OK;
                              }).Get(), &nwrTok);

                      // --- STEP 3: Trigger window.open ---
                      Log("[STEP 3] S7: Calling window.open\n");
                      ctx->parentWv->ExecuteScript(L"window.open('about:blank')", nullptr);
                      return S_OK;
                    }).Get(), &navTok);

            ctx->parentWv->Navigate(L"about:blank");
            return S_OK;
          }).Get());
}

// ==========================================================================
// SCENARIO 15: Before Nav | New WV | No Partition
// Like S1 but SetPartition is called BEFORE Navigate(about:blank).
// ==========================================================================
static void RunScenario15() {
  Log("\n[SCENARIO 15] === Before Nav | New WV | No Partition ===\n");
  auto* ctx = new ScenarioCtx();
  ctx->num = 15;
  g_scenarios.push_back(ctx);

  ctx->parentHwnd = CreateHostWindow(
      L"S15 Parent (partition before nav)", 400, 100, true);

  g_env->CreateCoreWebView2Controller(
      ctx->parentHwnd,
      Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
          [ctx](HRESULT hr, ICoreWebView2Controller* ctrl) -> HRESULT {
            if (FAILED(hr)) return hr;
            ctx->parentCtrl = ctrl;
            ctrl->get_CoreWebView2(&ctx->parentWv);
            RECT rc; GetClientRect(ctx->parentHwnd, &rc);
            ctrl->put_Bounds(rc);
            Log("[STEP 1] S15: Parent created\n");

            // *** STEP 2: Set partition BEFORE Navigate ***
            Log("[STEP 2] S15: Setting partition BEFORE Navigate(about:blank)\n");
            SetPartition(ctx->parentWv.Get(), L"demoPartition");

            // Wait for Navigate to complete before window.open
            EventRegistrationToken navTok;
            ctx->parentWv->add_NavigationCompleted(
                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                    [ctx](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                      if (ctx->step2Done) return S_OK;
                      ctx->step2Done = true;

                      Log("[STEP 2b] S15: Navigate(about:blank) completed\n");

                      // --- Register NWR handler ---
                      EventRegistrationToken nwrTok;
                      ctx->parentWv->add_NewWindowRequested(
                          Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                              [ctx](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                Log("[STEP 4] S15: NewWindowRequested fired\n");

                                ctx->childHwnd = CreateHostWindow(
                                    L"Scenario 15: Before Nav | New WV | No Partition", 750, 600, false);

                                ComPtr<ICoreWebView2Deferral> deferral;
                                args->GetDeferral(&deferral);

                                g_env->CreateCoreWebView2Controller(
                                    ctx->childHwnd,
                                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                        [ctx, args, deferral](HRESULT hr, ICoreWebView2Controller* childCtrl) -> HRESULT {
                                          if (FAILED(hr)) return hr;
                                          ctx->childCtrl = childCtrl;
                                          childCtrl->get_CoreWebView2(&ctx->childWv);

                                          ComPtr<ICoreWebView2Settings> settings;
                                          ctx->childWv->get_Settings(&settings);
                                          if (settings) settings->put_AreDevToolsEnabled(TRUE);

                                          Log("[STEP 4] S15: Child created, NO partition set\n");

                                          std::wstring parentPart = GetPartition(ctx->parentWv.Get());
                                          std::wstring childPart = GetPartition(ctx->childWv.Get());
                                          Log("[STEP 4] S15: parent=%ls child=%ls\n", parentPart.c_str(), childPart.c_str());

                                          ctx->pendingScript = BuildResultScript(15,
                                              L"Before Nav | New WV | No Partition",
                                              L"Parent sets <b>partition BEFORE Navigate(about:blank)</b>. New child WebView, no partition set.",
                                              parentPart, childPart, L"demoPartition");

                                          args->put_NewWindow(ctx->childWv.Get());
                                          args->put_Handled(TRUE);
                                          deferral->Complete();

                                          ShowWindow(ctx->childHwnd, SW_SHOW);
                                          SetForegroundWindow(ctx->childHwnd);
                                          SetTimer(ctx->childHwnd, kInjectTimer, 300, nullptr);
                                          return S_OK;
                                        }).Get());
                                return S_OK;
                              }).Get(), &nwrTok);

                      Log("[STEP 3] S15: Calling window.open\n");
                      ctx->parentWv->ExecuteScript(L"window.open('about:blank')", nullptr);
                      return S_OK;
                    }).Get(), &navTok);

            // *** Navigate AFTER SetPartition ***
            Log("[STEP 2c] S15: Now calling Navigate(about:blank)\n");
            ctx->parentWv->Navigate(L"about:blank");
            return S_OK;
          }).Get());
}

// ==========================================================================
// SCENARIO 16: Before Nav | New WV | Same Partition
// Like S2 but SetPartition is called BEFORE Navigate(about:blank).
// ==========================================================================
static void RunScenario16() {
  Log("\n[SCENARIO 16] === Before Nav | New WV | Same Partition ===\n");
  auto* ctx = new ScenarioCtx();
  ctx->num = 16;
  g_scenarios.push_back(ctx);

  ctx->parentHwnd = CreateHostWindow(
      L"S16 Parent (partition before nav)", 400, 100, true);

  g_env->CreateCoreWebView2Controller(
      ctx->parentHwnd,
      Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
          [ctx](HRESULT hr, ICoreWebView2Controller* ctrl) -> HRESULT {
            if (FAILED(hr)) return hr;
            ctx->parentCtrl = ctrl;
            ctrl->get_CoreWebView2(&ctx->parentWv);
            RECT rc; GetClientRect(ctx->parentHwnd, &rc);
            ctrl->put_Bounds(rc);
            Log("[STEP 1] S16: Parent created\n");

            // *** STEP 2: Set partition BEFORE Navigate ***
            Log("[STEP 2] S16: Setting partition BEFORE Navigate(about:blank)\n");
            SetPartition(ctx->parentWv.Get(), L"demoPartition");

            // Wait for Navigate to complete before window.open
            EventRegistrationToken navTok;
            ctx->parentWv->add_NavigationCompleted(
                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                    [ctx](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                      if (ctx->step2Done) return S_OK;
                      ctx->step2Done = true;

                      Log("[STEP 2b] S16: Navigate(about:blank) completed\n");

                      // --- Register NWR handler ---
                      EventRegistrationToken nwrTok;
                      ctx->parentWv->add_NewWindowRequested(
                          Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                              [ctx](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                Log("[STEP 4] S16: NewWindowRequested fired\n");

                                ctx->childHwnd = CreateHostWindow(
                                    L"Scenario 16: Before Nav | New WV | Same Partition", 750, 600, false);

                                ComPtr<ICoreWebView2Deferral> deferral;
                                args->GetDeferral(&deferral);

                                g_env->CreateCoreWebView2Controller(
                                    ctx->childHwnd,
                                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                        [ctx, args, deferral](HRESULT hr, ICoreWebView2Controller* childCtrl) -> HRESULT {
                                          if (FAILED(hr)) return hr;
                                          ctx->childCtrl = childCtrl;
                                          childCtrl->get_CoreWebView2(&ctx->childWv);

                                          ComPtr<ICoreWebView2Settings> settings;
                                          ctx->childWv->get_Settings(&settings);
                                          if (settings) settings->put_AreDevToolsEnabled(TRUE);

                                          SetPartition(ctx->childWv.Get(), L"demoPartition");
                                          Log("[STEP 4] S16: Child partition set to demoPartition\n");

                                          std::wstring parentPart = GetPartition(ctx->parentWv.Get());
                                          std::wstring childPart = GetPartition(ctx->childWv.Get());
                                          Log("[STEP 4] S16: parent=%ls child=%ls\n", parentPart.c_str(), childPart.c_str());

                                          ctx->pendingScript = BuildResultScript(16,
                                              L"Before Nav | New WV | Same Partition",
                                              L"Parent sets <b>partition BEFORE Navigate(about:blank)</b>. New child with same partition.",
                                              parentPart, childPart, L"demoPartition");

                                          args->put_NewWindow(ctx->childWv.Get());
                                          args->put_Handled(TRUE);
                                          deferral->Complete();

                                          ShowWindow(ctx->childHwnd, SW_SHOW);
                                          SetForegroundWindow(ctx->childHwnd);
                                          SetTimer(ctx->childHwnd, kInjectTimer, 300, nullptr);
                                          return S_OK;
                                        }).Get());
                                return S_OK;
                              }).Get(), &nwrTok);

                      Log("[STEP 3] S16: Calling window.open\n");
                      ctx->parentWv->ExecuteScript(L"window.open('about:blank')", nullptr);
                      return S_OK;
                    }).Get(), &navTok);

            // *** Navigate AFTER SetPartition ***
            Log("[STEP 2c] S16: Now calling Navigate(about:blank)\n");
            ctx->parentWv->Navigate(L"about:blank");
            return S_OK;
          }).Get());
}

// ==========================================================================
// SCENARIO 17: Before Nav | New WV | Different Partition
// Like S3 but SetPartition is called BEFORE Navigate(about:blank).
// ==========================================================================
static void RunScenario17() {
  Log("\n[SCENARIO 17] === Before Nav | New WV | Different Partition ===\n");
  auto* ctx = new ScenarioCtx();
  ctx->num = 17;
  g_scenarios.push_back(ctx);

  ctx->parentHwnd = CreateHostWindow(
      L"S17 Parent (partition before nav)", 400, 100, true);

  g_env->CreateCoreWebView2Controller(
      ctx->parentHwnd,
      Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
          [ctx](HRESULT hr, ICoreWebView2Controller* ctrl) -> HRESULT {
            if (FAILED(hr)) return hr;
            ctx->parentCtrl = ctrl;
            ctrl->get_CoreWebView2(&ctx->parentWv);
            RECT rc; GetClientRect(ctx->parentHwnd, &rc);
            ctrl->put_Bounds(rc);
            Log("[STEP 1] S17: Parent created\n");

            // *** STEP 2: Set partition BEFORE Navigate ***
            Log("[STEP 2] S17: Setting partition BEFORE Navigate(about:blank)\n");
            SetPartition(ctx->parentWv.Get(), L"demoPartition");

            // Wait for Navigate to complete before window.open
            EventRegistrationToken navTok;
            ctx->parentWv->add_NavigationCompleted(
                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                    [ctx](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                      if (ctx->step2Done) return S_OK;
                      ctx->step2Done = true;

                      Log("[STEP 2b] S17: Navigate(about:blank) completed\n");

                      // --- Register NWR handler ---
                      EventRegistrationToken nwrTok;
                      ctx->parentWv->add_NewWindowRequested(
                          Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                              [ctx](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                Log("[STEP 4] S17: NewWindowRequested fired\n");

                                ctx->childHwnd = CreateHostWindow(
                                    L"Scenario 17: Before Nav | New WV | Different Partition", 750, 600, false);

                                ComPtr<ICoreWebView2Deferral> deferral;
                                args->GetDeferral(&deferral);

                                g_env->CreateCoreWebView2Controller(
                                    ctx->childHwnd,
                                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                        [ctx, args, deferral](HRESULT hr, ICoreWebView2Controller* childCtrl) -> HRESULT {
                                          if (FAILED(hr)) return hr;
                                          ctx->childCtrl = childCtrl;
                                          childCtrl->get_CoreWebView2(&ctx->childWv);

                                          ComPtr<ICoreWebView2Settings> settings;
                                          ctx->childWv->get_Settings(&settings);
                                          if (settings) settings->put_AreDevToolsEnabled(TRUE);

                                          SetPartition(ctx->childWv.Get(), L"otherPartition");
                                          Log("[STEP 4] S17: Child partition set to otherPartition\n");

                                          std::wstring parentPart = GetPartition(ctx->parentWv.Get());
                                          std::wstring childPart = GetPartition(ctx->childWv.Get());
                                          Log("[STEP 4] S17: parent=%ls child=%ls\n", parentPart.c_str(), childPart.c_str());

                                          ctx->pendingScript = BuildResultScript(17,
                                              L"Before Nav | New WV | Different Partition",
                                              L"Parent sets <b>partition BEFORE Navigate(about:blank)</b>. New child with different partition.",
                                              parentPart, childPart, L"demoPartition");

                                          args->put_NewWindow(ctx->childWv.Get());
                                          args->put_Handled(TRUE);
                                          deferral->Complete();

                                          ShowWindow(ctx->childHwnd, SW_SHOW);
                                          SetForegroundWindow(ctx->childHwnd);
                                          SetTimer(ctx->childHwnd, kInjectTimer, 300, nullptr);
                                          return S_OK;
                                        }).Get());
                                return S_OK;
                              }).Get(), &nwrTok);

                      Log("[STEP 3] S17: Calling window.open\n");
                      ctx->parentWv->ExecuteScript(L"window.open('about:blank')", nullptr);
                      return S_OK;
                    }).Get(), &navTok);

            // *** Navigate AFTER SetPartition ***
            Log("[STEP 2c] S17: Now calling Navigate(about:blank)\n");
            ctx->parentWv->Navigate(L"about:blank");
            return S_OK;
          }).Get());
}

// ==========================================================================
// SCENARIO 18: Before Nav | New WV | Default Partition
// Like S4 but SetPartition is called BEFORE Navigate(about:blank).
// ==========================================================================
static void RunScenario18() {
  Log("\n[SCENARIO 18] === Before Nav | New WV | Default Partition ===\n");
  auto* ctx = new ScenarioCtx();
  ctx->num = 18;
  g_scenarios.push_back(ctx);

  ctx->parentHwnd = CreateHostWindow(
      L"S18 Parent (partition before nav)", 400, 100, true);

  g_env->CreateCoreWebView2Controller(
      ctx->parentHwnd,
      Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
          [ctx](HRESULT hr, ICoreWebView2Controller* ctrl) -> HRESULT {
            if (FAILED(hr)) return hr;
            ctx->parentCtrl = ctrl;
            ctrl->get_CoreWebView2(&ctx->parentWv);
            RECT rc; GetClientRect(ctx->parentHwnd, &rc);
            ctrl->put_Bounds(rc);
            Log("[STEP 1] S18: Parent created\n");

            // *** STEP 2: Set partition BEFORE Navigate ***
            Log("[STEP 2] S18: Setting partition BEFORE Navigate(about:blank)\n");
            SetPartition(ctx->parentWv.Get(), L"demoPartition");

            // Wait for Navigate to complete before window.open
            EventRegistrationToken navTok;
            ctx->parentWv->add_NavigationCompleted(
                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                    [ctx](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                      if (ctx->step2Done) return S_OK;
                      ctx->step2Done = true;

                      Log("[STEP 2b] S18: Navigate(about:blank) completed\n");

                      // --- Register NWR handler ---
                      EventRegistrationToken nwrTok;
                      ctx->parentWv->add_NewWindowRequested(
                          Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                              [ctx](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                Log("[STEP 4] S18: NewWindowRequested fired\n");

                                ctx->childHwnd = CreateHostWindow(
                                    L"Scenario 18: Before Nav | New WV | Default Partition", 750, 600, false);

                                ComPtr<ICoreWebView2Deferral> deferral;
                                args->GetDeferral(&deferral);

                                g_env->CreateCoreWebView2Controller(
                                    ctx->childHwnd,
                                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                        [ctx, args, deferral](HRESULT hr, ICoreWebView2Controller* childCtrl) -> HRESULT {
                                          if (FAILED(hr)) return hr;
                                          ctx->childCtrl = childCtrl;
                                          childCtrl->get_CoreWebView2(&ctx->childWv);

                                          ComPtr<ICoreWebView2Settings> settings;
                                          ctx->childWv->get_Settings(&settings);
                                          if (settings) settings->put_AreDevToolsEnabled(TRUE);

                                          SetPartition(ctx->childWv.Get(), L"");
                                          Log("[STEP 4] S18: Child partition set to (default)\n");

                                          std::wstring parentPart = GetPartition(ctx->parentWv.Get());
                                          std::wstring childPart = GetPartition(ctx->childWv.Get());
                                          Log("[STEP 4] S18: parent=%ls child=%ls\n", parentPart.c_str(), childPart.c_str());

                                          ctx->pendingScript = BuildResultScript(18,
                                              L"Before Nav | New WV | Default Partition",
                                              L"Parent sets <b>partition BEFORE Navigate(about:blank)</b>. New child with default partition.",
                                              parentPart, childPart, L"demoPartition");

                                          args->put_NewWindow(ctx->childWv.Get());
                                          args->put_Handled(TRUE);
                                          deferral->Complete();

                                          ShowWindow(ctx->childHwnd, SW_SHOW);
                                          SetForegroundWindow(ctx->childHwnd);
                                          SetTimer(ctx->childHwnd, kInjectTimer, 300, nullptr);
                                          return S_OK;
                                        }).Get());
                                return S_OK;
                              }).Get(), &nwrTok);

                      Log("[STEP 3] S18: Calling window.open\n");
                      ctx->parentWv->ExecuteScript(L"window.open('about:blank')", nullptr);
                      return S_OK;
                    }).Get(), &navTok);

            // *** Navigate AFTER SetPartition ***
            Log("[STEP 2c] S18: Now calling Navigate(about:blank)\n");
            ctx->parentWv->Navigate(L"about:blank");
            return S_OK;
          }).Get());
}

// ==========================================================================
// SCENARIO 19: Before Nav | Reused | Inherits
// Like S5 but SetPartition is called BEFORE Navigate(about:blank).
// ==========================================================================
static void RunScenario19() {
  Log("\n[SCENARIO 19] === Before Nav | Reused | Inherits ===\n");
  auto* ctx = new ScenarioCtx();
  ctx->num = 19;
  ctx->isParentReuse = true;
  g_scenarios.push_back(ctx);

  ctx->parentHwnd = CreateHostWindow(
      L"Scenario 19: Before Nav | Reused | Inherits", 750, 600, true);

  g_env->CreateCoreWebView2Controller(
      ctx->parentHwnd,
      Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
          [ctx](HRESULT hr, ICoreWebView2Controller* ctrl) -> HRESULT {
            if (FAILED(hr)) return hr;
            ctx->parentCtrl = ctrl;
            ctrl->get_CoreWebView2(&ctx->parentWv);
            RECT rc; GetClientRect(ctx->parentHwnd, &rc);
            ctrl->put_Bounds(rc);
            Log("[STEP 1] S19: Parent created\n");

            // *** STEP 2: Set partition BEFORE Navigate ***
            Log("[STEP 2] S19: Setting partition BEFORE Navigate(about:blank)\n");
            SetPartition(ctx->parentWv.Get(), L"demoPartition");

            EventRegistrationToken navTok;
            ctx->parentWv->add_NavigationCompleted(
                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                    [ctx](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                      if (ctx->step2Done) return S_OK;
                      ctx->step2Done = true;

                      Log("[STEP 2b] S19: Navigate(about:blank) completed\n");

                      EventRegistrationToken nwrTok;
                      ctx->parentWv->add_NewWindowRequested(
                          Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                              [ctx](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                Log("[STEP 4] S19: NewWindowRequested fired\n");

                                Log("[STEP 4] S19: No partition change, inheriting parent\n");
                                std::wstring parentPart = GetPartition(ctx->parentWv.Get());
                                ctx->pendingScript = BuildResultScript(19,
                                    L"Before Nav | Reused | Inherits",
                                    L"Parent sets <b>partition BEFORE Navigate(about:blank)</b>. Parent reused, inherits partition.",
                                    parentPart, parentPart, L"demoPartition");

                                args->put_NewWindow(ctx->parentWv.Get());
                                args->put_Handled(TRUE);
                                SetTimer(ctx->parentHwnd, kInjectTimer, 300, nullptr);
                                return S_OK;
                              }).Get(), &nwrTok);

                      Log("[STEP 3] S19: Calling window.open\n");
                      ctx->parentWv->ExecuteScript(L"window.open('about:blank')", nullptr);
                      return S_OK;
                    }).Get(), &navTok);

            // *** Navigate AFTER SetPartition ***
            Log("[STEP 2c] S19: Now calling Navigate(about:blank)\n");
            ctx->parentWv->Navigate(L"about:blank");
            return S_OK;
          }).Get());
}

// ==========================================================================
// SCENARIO 20: Before Nav | Reused | Default
// Like S6 but SetPartition is called BEFORE Navigate(about:blank).
// ==========================================================================
static void RunScenario20() {
  Log("\n[SCENARIO 20] === Before Nav | Reused | Default ===\n");
  auto* ctx = new ScenarioCtx();
  ctx->num = 20;
  ctx->isParentReuse = true;
  g_scenarios.push_back(ctx);

  ctx->parentHwnd = CreateHostWindow(
      L"Scenario 20: Before Nav | Reused | Default", 750, 600, true);

  g_env->CreateCoreWebView2Controller(
      ctx->parentHwnd,
      Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
          [ctx](HRESULT hr, ICoreWebView2Controller* ctrl) -> HRESULT {
            if (FAILED(hr)) return hr;
            ctx->parentCtrl = ctrl;
            ctrl->get_CoreWebView2(&ctx->parentWv);
            RECT rc; GetClientRect(ctx->parentHwnd, &rc);
            ctrl->put_Bounds(rc);
            Log("[STEP 1] S20: Parent created\n");

            // *** STEP 2: Set partition BEFORE Navigate ***
            Log("[STEP 2] S20: Setting partition BEFORE Navigate(about:blank)\n");
            SetPartition(ctx->parentWv.Get(), L"demoPartition");

            EventRegistrationToken navTok;
            ctx->parentWv->add_NavigationCompleted(
                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                    [ctx](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                      if (ctx->step2Done) return S_OK;
                      ctx->step2Done = true;

                      Log("[STEP 2b] S20: Navigate(about:blank) completed\n");

                      EventRegistrationToken nwrTok;
                      ctx->parentWv->add_NewWindowRequested(
                          Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                              [ctx](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                Log("[STEP 4] S20: NewWindowRequested fired\n");

                                std::wstring originalParentPart = L"demoPartition";
                                SetPartition(ctx->parentWv.Get(), L"");
                                Log("[STEP 4] S20: Partition changed to (default)\n");
                                std::wstring popupPart = GetPartition(ctx->parentWv.Get());
                                ctx->pendingScript = BuildResultScript(20,
                                    L"Before Nav | Reused | Default",
                                    L"Parent sets <b>partition BEFORE Navigate(about:blank)</b>. Parent reused, reset to default before put_NewWindow.",
                                    originalParentPart, popupPart, L"demoPartition");

                                args->put_NewWindow(ctx->parentWv.Get());
                                args->put_Handled(TRUE);
                                SetTimer(ctx->parentHwnd, kInjectTimer, 300, nullptr);
                                return S_OK;
                              }).Get(), &nwrTok);

                      Log("[STEP 3] S20: Calling window.open\n");
                      ctx->parentWv->ExecuteScript(L"window.open('about:blank')", nullptr);
                      return S_OK;
                    }).Get(), &navTok);

            // *** Navigate AFTER SetPartition ***
            Log("[STEP 2c] S20: Now calling Navigate(about:blank)\n");
            ctx->parentWv->Navigate(L"about:blank");
            return S_OK;
          }).Get());
}

// ==========================================================================
// SCENARIO 21: Before Nav | Reused | Own Partition
// Like S7 but SetPartition is called BEFORE Navigate(about:blank).
// ==========================================================================
static void RunScenario21() {
  Log("\n[SCENARIO 21] === Before Nav | Reused | Own Partition ===\n");
  auto* ctx = new ScenarioCtx();
  ctx->num = 21;
  ctx->isParentReuse = true;
  g_scenarios.push_back(ctx);

  ctx->parentHwnd = CreateHostWindow(
      L"Scenario 21: Before Nav | Reused | Own Partition", 750, 600, true);

  g_env->CreateCoreWebView2Controller(
      ctx->parentHwnd,
      Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
          [ctx](HRESULT hr, ICoreWebView2Controller* ctrl) -> HRESULT {
            if (FAILED(hr)) return hr;
            ctx->parentCtrl = ctrl;
            ctrl->get_CoreWebView2(&ctx->parentWv);
            RECT rc; GetClientRect(ctx->parentHwnd, &rc);
            ctrl->put_Bounds(rc);
            Log("[STEP 1] S21: Parent created\n");

            // *** STEP 2: Set partition BEFORE Navigate ***
            Log("[STEP 2] S21: Setting partition BEFORE Navigate(about:blank)\n");
            SetPartition(ctx->parentWv.Get(), L"demoPartition");

            EventRegistrationToken navTok;
            ctx->parentWv->add_NavigationCompleted(
                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                    [ctx](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                      if (ctx->step2Done) return S_OK;
                      ctx->step2Done = true;

                      Log("[STEP 2b] S21: Navigate(about:blank) completed\n");

                      EventRegistrationToken nwrTok;
                      ctx->parentWv->add_NewWindowRequested(
                          Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                              [ctx](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                Log("[STEP 4] S21: NewWindowRequested fired\n");

                                std::wstring originalParentPart = L"demoPartition";
                                SetPartition(ctx->parentWv.Get(), L"otherPartition");
                                Log("[STEP 4] S21: Partition changed to otherPartition\n");
                                std::wstring popupPart = GetPartition(ctx->parentWv.Get());
                                ctx->pendingScript = BuildResultScript(21,
                                    L"Before Nav | Reused | Own Partition",
                                    L"Parent sets <b>partition BEFORE Navigate(about:blank)</b>. Parent reused, changed to otherPartition before put_NewWindow.",
                                    originalParentPart, popupPart, L"demoPartition");

                                args->put_NewWindow(ctx->parentWv.Get());
                                args->put_Handled(TRUE);
                                SetTimer(ctx->parentHwnd, kInjectTimer, 300, nullptr);
                                return S_OK;
                              }).Get(), &nwrTok);

                      Log("[STEP 3] S21: Calling window.open\n");
                      ctx->parentWv->ExecuteScript(L"window.open('about:blank')", nullptr);
                      return S_OK;
                    }).Get(), &navTok);

            // *** Navigate AFTER SetPartition ***
            Log("[STEP 2c] S21: Now calling Navigate(about:blank)\n");
            ctx->parentWv->Navigate(L"about:blank");
            return S_OK;
          }).Get());
}


// ==========================================================================
// SCENARIO 8: Implicit Nav | New WV | No Partition
// Same as S1 but parent does NOT call Navigate(about:blank).
// Auto about:blank completes during creation, so we proceed directly.
// ==========================================================================
static void RunScenario8() {
  Log("\n[SCENARIO 8] === Implicit Nav | New WV | No Partition ===\n");
  auto* ctx = new ScenarioCtx();
  ctx->num = 8;
  g_scenarios.push_back(ctx);

  ctx->parentHwnd = CreateHostWindow(
      L"S8 Parent (implicit nav)", 400, 100, true);

  g_env->CreateCoreWebView2Controller(
      ctx->parentHwnd,
      Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
          [ctx](HRESULT hr, ICoreWebView2Controller* ctrl) -> HRESULT {
            if (FAILED(hr)) return hr;
            ctx->parentCtrl = ctrl;
            ctrl->get_CoreWebView2(&ctx->parentWv);
            RECT rc; GetClientRect(ctx->parentHwnd, &rc);
            ctrl->put_Bounds(rc);
            Log("[STEP 1] S8: Parent created (NO explicit Navigate)\n");

            // Auto about:blank already loaded � proceed directly
            Log("[STEP 2] S8: Setting partition on parent (auto about:blank already done)\n");
            SetPartition(ctx->parentWv.Get(), L"demoPartition");

            // --- Register NWR handler ---
            EventRegistrationToken nwrTok;
            ctx->parentWv->add_NewWindowRequested(
                Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                    [ctx](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                      Log("[STEP 4] S8: NewWindowRequested fired\n");

                      ctx->childHwnd = CreateHostWindow(
                          L"Scenario 8: Implicit Nav | New WV | No Partition", 750, 600, false);

                      ComPtr<ICoreWebView2Deferral> deferral;
                      args->GetDeferral(&deferral);

                      g_env->CreateCoreWebView2Controller(
                          ctx->childHwnd,
                          Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                              [ctx, args, deferral](HRESULT hr, ICoreWebView2Controller* childCtrl) -> HRESULT {
                                if (FAILED(hr)) return hr;
                                ctx->childCtrl = childCtrl;
                                childCtrl->get_CoreWebView2(&ctx->childWv);

                                ComPtr<ICoreWebView2Settings> settings;
                                ctx->childWv->get_Settings(&settings);
                                if (settings) settings->put_AreDevToolsEnabled(TRUE);

                                Log("[STEP 4] S8: Child created, NO partition set\n");

                                std::wstring parentPart = GetPartition(ctx->parentWv.Get());
                                std::wstring childPart = GetPartition(ctx->childWv.Get());
                                Log("[STEP 4] S8: parent=%ls child=%ls\n", parentPart.c_str(), childPart.c_str());

                                ctx->pendingScript = BuildResultScript(8,
                                    L"Implicit Nav | New WV | No Partition",
                                    L"Like S1 but parent does <b>NOT</b> call Navigate(about:blank). Auto-nav already complete at creation callback.",
                                    parentPart, childPart);

                                args->put_NewWindow(ctx->childWv.Get());
                                args->put_Handled(TRUE);
                                deferral->Complete();

                                ShowWindow(ctx->childHwnd, SW_SHOW);
                                SetForegroundWindow(ctx->childHwnd);
                                SetTimer(ctx->childHwnd, kInjectTimer, 300, nullptr);
                                return S_OK;
                              }).Get());
                      return S_OK;
                    }).Get(), &nwrTok);

            // --- STEP 3: Trigger window.open ---
            Log("[STEP 3] S8: Calling window.open\n");
            ctx->parentWv->ExecuteScript(L"window.open('about:blank')", nullptr);
            return S_OK;
          }).Get());
}

// ==========================================================================
// SCENARIO 9: Implicit Nav | New WV | Same Partition
// Same as S2 but parent does NOT call Navigate(about:blank).
// Auto about:blank completes during creation, so we proceed directly.
// ==========================================================================
static void RunScenario9() {
  Log("\n[SCENARIO 9] === Implicit Nav | New WV | Same Partition ===\n");
  auto* ctx = new ScenarioCtx();
  ctx->num = 9;
  g_scenarios.push_back(ctx);

  ctx->parentHwnd = CreateHostWindow(
      L"S9 Parent (implicit nav)", 400, 100, true);

  g_env->CreateCoreWebView2Controller(
      ctx->parentHwnd,
      Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
          [ctx](HRESULT hr, ICoreWebView2Controller* ctrl) -> HRESULT {
            if (FAILED(hr)) return hr;
            ctx->parentCtrl = ctrl;
            ctrl->get_CoreWebView2(&ctx->parentWv);
            RECT rc; GetClientRect(ctx->parentHwnd, &rc);
            ctrl->put_Bounds(rc);
            Log("[STEP 1] S9: Parent created (NO explicit Navigate)\n");

            // Auto about:blank already loaded � proceed directly
            Log("[STEP 2] S9: Setting partition on parent (auto about:blank already done)\n");
            SetPartition(ctx->parentWv.Get(), L"demoPartition");

            // --- Register NWR handler ---
            EventRegistrationToken nwrTok;
            ctx->parentWv->add_NewWindowRequested(
                Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                    [ctx](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                      Log("[STEP 4] S9: NewWindowRequested fired\n");

                      ctx->childHwnd = CreateHostWindow(
                          L"Scenario 9: Implicit Nav | New WV | Same Partition", 750, 600, false);

                      ComPtr<ICoreWebView2Deferral> deferral;
                      args->GetDeferral(&deferral);

                      g_env->CreateCoreWebView2Controller(
                          ctx->childHwnd,
                          Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                              [ctx, args, deferral](HRESULT hr, ICoreWebView2Controller* childCtrl) -> HRESULT {
                                if (FAILED(hr)) return hr;
                                ctx->childCtrl = childCtrl;
                                childCtrl->get_CoreWebView2(&ctx->childWv);

                                ComPtr<ICoreWebView2Settings> settings;
                                ctx->childWv->get_Settings(&settings);
                                if (settings) settings->put_AreDevToolsEnabled(TRUE);

                                SetPartition(ctx->childWv.Get(), L"demoPartition");
                                Log("[STEP 4] S9: Child partition set to demoPartition\n");

                                std::wstring parentPart = GetPartition(ctx->parentWv.Get());
                                std::wstring childPart = GetPartition(ctx->childWv.Get());
                                Log("[STEP 4] S9: parent=%ls child=%ls\n", parentPart.c_str(), childPart.c_str());

                                ctx->pendingScript = BuildResultScript(9,
                                    L"Implicit Nav | New WV | Same Partition",
                                    L"Like S2 but parent does <b>NOT</b> call Navigate(about:blank). Auto-nav already complete at creation callback.",
                                    parentPart, childPart);

                                args->put_NewWindow(ctx->childWv.Get());
                                args->put_Handled(TRUE);
                                deferral->Complete();

                                ShowWindow(ctx->childHwnd, SW_SHOW);
                                SetForegroundWindow(ctx->childHwnd);
                                SetTimer(ctx->childHwnd, kInjectTimer, 300, nullptr);
                                return S_OK;
                              }).Get());
                      return S_OK;
                    }).Get(), &nwrTok);

            // --- STEP 3: Trigger window.open ---
            Log("[STEP 3] S9: Calling window.open\n");
            ctx->parentWv->ExecuteScript(L"window.open('about:blank')", nullptr);
            return S_OK;
          }).Get());
}

// ==========================================================================
// SCENARIO 10: Implicit Nav | New WV | Different Partition
// Same as S3 but parent does NOT call Navigate(about:blank).
// Auto about:blank completes during creation, so we proceed directly.
// ==========================================================================
static void RunScenario10() {
  Log("\n[SCENARIO 10] === Implicit Nav | New WV | Different Partition ===\n");
  auto* ctx = new ScenarioCtx();
  ctx->num = 10;
  g_scenarios.push_back(ctx);

  ctx->parentHwnd = CreateHostWindow(
      L"S10 Parent (implicit nav)", 400, 100, true);

  g_env->CreateCoreWebView2Controller(
      ctx->parentHwnd,
      Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
          [ctx](HRESULT hr, ICoreWebView2Controller* ctrl) -> HRESULT {
            if (FAILED(hr)) return hr;
            ctx->parentCtrl = ctrl;
            ctrl->get_CoreWebView2(&ctx->parentWv);
            RECT rc; GetClientRect(ctx->parentHwnd, &rc);
            ctrl->put_Bounds(rc);
            Log("[STEP 1] S10: Parent created (NO explicit Navigate)\n");

            // Auto about:blank already loaded � proceed directly
            Log("[STEP 2] S10: Setting partition on parent (auto about:blank already done)\n");
            SetPartition(ctx->parentWv.Get(), L"demoPartition");

            // --- Register NWR handler ---
            EventRegistrationToken nwrTok;
            ctx->parentWv->add_NewWindowRequested(
                Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                    [ctx](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                      Log("[STEP 4] S10: NewWindowRequested fired\n");

                      ctx->childHwnd = CreateHostWindow(
                          L"Scenario 10: Implicit Nav | New WV | Different Partition", 750, 600, false);

                      ComPtr<ICoreWebView2Deferral> deferral;
                      args->GetDeferral(&deferral);

                      g_env->CreateCoreWebView2Controller(
                          ctx->childHwnd,
                          Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                              [ctx, args, deferral](HRESULT hr, ICoreWebView2Controller* childCtrl) -> HRESULT {
                                if (FAILED(hr)) return hr;
                                ctx->childCtrl = childCtrl;
                                childCtrl->get_CoreWebView2(&ctx->childWv);

                                ComPtr<ICoreWebView2Settings> settings;
                                ctx->childWv->get_Settings(&settings);
                                if (settings) settings->put_AreDevToolsEnabled(TRUE);

                                SetPartition(ctx->childWv.Get(), L"otherPartition");
                                Log("[STEP 4] S10: Child partition set to otherPartition\n");

                                std::wstring parentPart = GetPartition(ctx->parentWv.Get());
                                std::wstring childPart = GetPartition(ctx->childWv.Get());
                                Log("[STEP 4] S10: parent=%ls child=%ls\n", parentPart.c_str(), childPart.c_str());

                                ctx->pendingScript = BuildResultScript(10,
                                    L"Implicit Nav | New WV | Different Partition",
                                    L"Like S3 but parent does <b>NOT</b> call Navigate(about:blank). Auto-nav already complete at creation callback.",
                                    parentPart, childPart);

                                args->put_NewWindow(ctx->childWv.Get());
                                args->put_Handled(TRUE);
                                deferral->Complete();

                                ShowWindow(ctx->childHwnd, SW_SHOW);
                                SetForegroundWindow(ctx->childHwnd);
                                SetTimer(ctx->childHwnd, kInjectTimer, 300, nullptr);
                                return S_OK;
                              }).Get());
                      return S_OK;
                    }).Get(), &nwrTok);

            // --- STEP 3: Trigger window.open ---
            Log("[STEP 3] S10: Calling window.open\n");
            ctx->parentWv->ExecuteScript(L"window.open('about:blank')", nullptr);
            return S_OK;
          }).Get());
}

// ==========================================================================
// SCENARIO 11: Implicit Nav | New WV | Default Partition
// Same as S4 but parent does NOT call Navigate(about:blank).
// Auto about:blank completes during creation, so we proceed directly.
// ==========================================================================
static void RunScenario11() {
  Log("\n[SCENARIO 11] === Implicit Nav | New WV | Default Partition ===\n");
  auto* ctx = new ScenarioCtx();
  ctx->num = 11;
  g_scenarios.push_back(ctx);

  ctx->parentHwnd = CreateHostWindow(
      L"S11 Parent (implicit nav)", 400, 100, true);

  g_env->CreateCoreWebView2Controller(
      ctx->parentHwnd,
      Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
          [ctx](HRESULT hr, ICoreWebView2Controller* ctrl) -> HRESULT {
            if (FAILED(hr)) return hr;
            ctx->parentCtrl = ctrl;
            ctrl->get_CoreWebView2(&ctx->parentWv);
            RECT rc; GetClientRect(ctx->parentHwnd, &rc);
            ctrl->put_Bounds(rc);
            Log("[STEP 1] S11: Parent created (NO explicit Navigate)\n");

            // Auto about:blank already loaded � proceed directly
            Log("[STEP 2] S11: Setting partition on parent (auto about:blank already done)\n");
            SetPartition(ctx->parentWv.Get(), L"demoPartition");

            // --- Register NWR handler ---
            EventRegistrationToken nwrTok;
            ctx->parentWv->add_NewWindowRequested(
                Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                    [ctx](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                      Log("[STEP 4] S11: NewWindowRequested fired\n");

                      ctx->childHwnd = CreateHostWindow(
                          L"Scenario 11: Implicit Nav | New WV | Default Partition", 750, 600, false);

                      ComPtr<ICoreWebView2Deferral> deferral;
                      args->GetDeferral(&deferral);

                      g_env->CreateCoreWebView2Controller(
                          ctx->childHwnd,
                          Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                              [ctx, args, deferral](HRESULT hr, ICoreWebView2Controller* childCtrl) -> HRESULT {
                                if (FAILED(hr)) return hr;
                                ctx->childCtrl = childCtrl;
                                childCtrl->get_CoreWebView2(&ctx->childWv);

                                ComPtr<ICoreWebView2Settings> settings;
                                ctx->childWv->get_Settings(&settings);
                                if (settings) settings->put_AreDevToolsEnabled(TRUE);

                                SetPartition(ctx->childWv.Get(), L"");
                                Log("[STEP 4] S11: Child partition set to (default)\n");

                                std::wstring parentPart = GetPartition(ctx->parentWv.Get());
                                std::wstring childPart = GetPartition(ctx->childWv.Get());
                                Log("[STEP 4] S11: parent=%ls child=%ls\n", parentPart.c_str(), childPart.c_str());

                                ctx->pendingScript = BuildResultScript(11,
                                    L"Implicit Nav | New WV | Default Partition",
                                    L"Like S4 but parent does <b>NOT</b> call Navigate(about:blank). Auto-nav already complete at creation callback.",
                                    parentPart, childPart);

                                args->put_NewWindow(ctx->childWv.Get());
                                args->put_Handled(TRUE);
                                deferral->Complete();

                                ShowWindow(ctx->childHwnd, SW_SHOW);
                                SetForegroundWindow(ctx->childHwnd);
                                SetTimer(ctx->childHwnd, kInjectTimer, 300, nullptr);
                                return S_OK;
                              }).Get());
                      return S_OK;
                    }).Get(), &nwrTok);

            // --- STEP 3: Trigger window.open ---
            Log("[STEP 3] S11: Calling window.open\n");
            ctx->parentWv->ExecuteScript(L"window.open('about:blank')", nullptr);
            return S_OK;
          }).Get());
}

// ==========================================================================
// SCENARIO 12: Implicit Nav | Reused | Inherits
// Same as S5 but parent does NOT call Navigate(about:blank).
// ==========================================================================
static void RunScenario12() {
  Log("\n[SCENARIO 12] === Implicit Nav | Reused | Inherits ===\n");
  auto* ctx = new ScenarioCtx();
  ctx->num = 12;
  ctx->isParentReuse = true;
  g_scenarios.push_back(ctx);

  ctx->parentHwnd = CreateHostWindow(
      L"Scenario 12: Implicit Nav | Reused | Inherits", 750, 600, true);

  g_env->CreateCoreWebView2Controller(
      ctx->parentHwnd,
      Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
          [ctx](HRESULT hr, ICoreWebView2Controller* ctrl) -> HRESULT {
            if (FAILED(hr)) return hr;
            ctx->parentCtrl = ctrl;
            ctrl->get_CoreWebView2(&ctx->parentWv);
            RECT rc; GetClientRect(ctx->parentHwnd, &rc);
            ctrl->put_Bounds(rc);
            Log("[STEP 1] S12: Parent created (NO explicit Navigate)\n");

            // Auto about:blank already loaded � proceed directly
            Log("[STEP 2] S12: Setting partition on parent (auto about:blank already done)\n");
            SetPartition(ctx->parentWv.Get(), L"demoPartition");

            // --- Register NWR handler ---
            EventRegistrationToken nwrTok;
            ctx->parentWv->add_NewWindowRequested(
                Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                    [ctx](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                      Log("[STEP 4] S12: NewWindowRequested fired\n");

                      Log("[STEP 4] S12: No partition change, inheriting parent\n");
                      std::wstring parentPart = GetPartition(ctx->parentWv.Get());
                      ctx->pendingScript = BuildResultScript(12,
                          L"Implicit Nav | Reused | Inherits",
                          L"Like S5 but parent does <b>NOT</b> call Navigate(about:blank). Auto-nav at creation.",
                          parentPart, parentPart);

                      args->put_NewWindow(ctx->parentWv.Get());
                      args->put_Handled(TRUE);
                      SetTimer(ctx->parentHwnd, kInjectTimer, 300, nullptr);
                      return S_OK;
                    }).Get(), &nwrTok);

            // --- STEP 3: Trigger window.open ---
            Log("[STEP 3] S12: Calling window.open\n");
            ctx->parentWv->ExecuteScript(L"window.open('about:blank')", nullptr);
            return S_OK;
          }).Get());
}

// ==========================================================================
// SCENARIO 13: Implicit Nav | Reused | Default
// Same as S6 but parent does NOT call Navigate(about:blank).
// ==========================================================================
static void RunScenario13() {
  Log("\n[SCENARIO 13] === Implicit Nav | Reused | Default ===\n");
  auto* ctx = new ScenarioCtx();
  ctx->num = 13;
  ctx->isParentReuse = true;
  g_scenarios.push_back(ctx);

  ctx->parentHwnd = CreateHostWindow(
      L"Scenario 13: Implicit Nav | Reused | Default", 750, 600, true);

  g_env->CreateCoreWebView2Controller(
      ctx->parentHwnd,
      Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
          [ctx](HRESULT hr, ICoreWebView2Controller* ctrl) -> HRESULT {
            if (FAILED(hr)) return hr;
            ctx->parentCtrl = ctrl;
            ctrl->get_CoreWebView2(&ctx->parentWv);
            RECT rc; GetClientRect(ctx->parentHwnd, &rc);
            ctrl->put_Bounds(rc);
            Log("[STEP 1] S13: Parent created (NO explicit Navigate)\n");

            // Auto about:blank already loaded � proceed directly
            Log("[STEP 2] S13: Setting partition on parent (auto about:blank already done)\n");
            SetPartition(ctx->parentWv.Get(), L"demoPartition");

            // --- Register NWR handler ---
            EventRegistrationToken nwrTok;
            ctx->parentWv->add_NewWindowRequested(
                Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                    [ctx](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                      Log("[STEP 4] S13: NewWindowRequested fired\n");

                      std::wstring originalParentPart = L"demoPartition";
                      SetPartition(ctx->parentWv.Get(), L"");
                      Log("[STEP 4] S13: Partition changed to (default)\n");
                      std::wstring popupPart = GetPartition(ctx->parentWv.Get());
                      ctx->pendingScript = BuildResultScript(13,
                          L"Implicit Nav | Reused | Default",
                          L"Like S6 but parent does <b>NOT</b> call Navigate(about:blank). Partition reset to default before put_NewWindow.",
                          originalParentPart, popupPart);

                      args->put_NewWindow(ctx->parentWv.Get());
                      args->put_Handled(TRUE);
                      SetTimer(ctx->parentHwnd, kInjectTimer, 300, nullptr);
                      return S_OK;
                    }).Get(), &nwrTok);

            // --- STEP 3: Trigger window.open ---
            Log("[STEP 3] S13: Calling window.open\n");
            ctx->parentWv->ExecuteScript(L"window.open('about:blank')", nullptr);
            return S_OK;
          }).Get());
}

// ==========================================================================
// SCENARIO 14: Implicit Nav | Reused | Own Partition
// Same as S7 but parent does NOT call Navigate(about:blank).
// ==========================================================================
static void RunScenario14() {
  Log("\n[SCENARIO 14] === Implicit Nav | Reused | Own Partition ===\n");
  auto* ctx = new ScenarioCtx();
  ctx->num = 14;
  ctx->isParentReuse = true;
  g_scenarios.push_back(ctx);

  ctx->parentHwnd = CreateHostWindow(
      L"Scenario 14: Implicit Nav | Reused | Own Partition", 750, 600, true);

  g_env->CreateCoreWebView2Controller(
      ctx->parentHwnd,
      Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
          [ctx](HRESULT hr, ICoreWebView2Controller* ctrl) -> HRESULT {
            if (FAILED(hr)) return hr;
            ctx->parentCtrl = ctrl;
            ctrl->get_CoreWebView2(&ctx->parentWv);
            RECT rc; GetClientRect(ctx->parentHwnd, &rc);
            ctrl->put_Bounds(rc);
            Log("[STEP 1] S14: Parent created (NO explicit Navigate)\n");

            // Auto about:blank already loaded � proceed directly
            Log("[STEP 2] S14: Setting partition on parent (auto about:blank already done)\n");
            SetPartition(ctx->parentWv.Get(), L"demoPartition");

            // --- Register NWR handler ---
            EventRegistrationToken nwrTok;
            ctx->parentWv->add_NewWindowRequested(
                Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                    [ctx](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                      Log("[STEP 4] S14: NewWindowRequested fired\n");

                      std::wstring originalParentPart = L"demoPartition";
                      SetPartition(ctx->parentWv.Get(), L"otherPartition");
                      Log("[STEP 4] S14: Partition changed to otherPartition\n");
                      std::wstring popupPart = GetPartition(ctx->parentWv.Get());
                      ctx->pendingScript = BuildResultScript(14,
                          L"Implicit Nav | Reused | Own Partition",
                          L"Like S7 but parent does <b>NOT</b> call Navigate(about:blank). Partition changed to otherPartition before put_NewWindow.",
                          originalParentPart, popupPart);

                      args->put_NewWindow(ctx->parentWv.Get());
                      args->put_Handled(TRUE);
                      SetTimer(ctx->parentHwnd, kInjectTimer, 300, nullptr);
                      return S_OK;
                    }).Get(), &nwrTok);

            // --- STEP 3: Trigger window.open ---
            Log("[STEP 3] S14: Calling window.open\n");
            ctx->parentWv->ExecuteScript(L"window.open('about:blank')", nullptr);
            return S_OK;
          }).Get());
}


// ==========================================================================
// Launcher (control panel)
// ==========================================================================
static void InitLauncher() {
  g_env->CreateCoreWebView2Controller(
      g_launcherHwnd,
      Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
          [](HRESULT hr, ICoreWebView2Controller* ctrl) -> HRESULT {
            if (FAILED(hr)) return hr;
            g_launcherCtrl = ctrl;
            ctrl->get_CoreWebView2(&g_launcherWv);
            RECT rc; GetClientRect(g_launcherHwnd, &rc);
            ctrl->put_Bounds(rc);

            EventRegistrationToken msgTok;
            g_launcherWv->add_WebMessageReceived(
                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                    [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                      LPWSTR raw = nullptr;
                      args->TryGetWebMessageAsString(&raw);
                      if (!raw) return S_OK;
                      int n = _wtoi(raw);
                      CoTaskMemFree(raw);
                      switch (n) {
                        case 1: RunScenario1(); break;
                        case 2: RunScenario2(); break;
                        case 3: RunScenario3(); break;
                        case 4: RunScenario4(); break;
                        case 5: RunScenario5(); break;
                        case 6: RunScenario6(); break;
                        case 7: RunScenario7(); break;
                        case 8: RunScenario8(); break;
                        case 9: RunScenario9(); break;
                        case 10: RunScenario10(); break;
                        case 11: RunScenario11(); break;
                        case 12: RunScenario12(); break;
                        case 13: RunScenario13(); break;
                        case 14: RunScenario14(); break;
                        case 15: RunScenario15(); break;
                        case 16: RunScenario16(); break;
                        case 17: RunScenario17(); break;
                        case 18: RunScenario18(); break;
                        case 19: RunScenario19(); break;
                        case 20: RunScenario20(); break;
                        case 21: RunScenario21(); break;
                      }
                      return S_OK;
                    }).Get(), &msgTok);

            // Inject launcher UI
            std::wstring js;
            js += L"(function(){\n";
            js += L"document.documentElement.style.cssText='margin:0;padding:0;width:100%;height:100%';\n";
            js += L"document.body.style.cssText='margin:0;padding:30px;background:#1e1e2e;color:#cdd6f4;font-family:Segoe UI,sans-serif;max-width:800px;min-height:100%';\n";
            js += L"document.title='Partition + NewWindow Demo';\n";
            js += L"document.head.innerHTML='<style>*{box-sizing:border-box;margin:0;padding:0}body{font-family:Segoe UI,sans-serif;background:#1e1e2e;color:#cdd6f4;padding:30px;max-width:800px}h1{color:#89b4fa;font-size:22px;margin-bottom:4px}.hint{color:#a6adc8;font-size:13px;margin-bottom:16px}h2{color:#fab387;font-size:16px;margin:14px 0 8px}.sc{background:#313244;border:1px solid #45475a;border-radius:8px;padding:12px 16px;margin-bottom:8px;cursor:pointer;transition:.15s}.sc:hover{border-color:#89b4fa;background:#3b3d50}.num{color:#89b4fa;font-weight:700;font-size:18px;margin-right:10px}.lbl{font-weight:600;font-size:13px}.desc{color:#a6adc8;font-size:12px;margin-top:4px}</style>';\n";
            js += L"var h='<h1>Partition + NewWindow Demo</h1>';\n";
            js += L"h+='<p class=\"hint\">Each scenario creates its own WebView from scratch. Click to run.</p>';\n";

            js += L"h+='<h2>\\u2460 Partition AFTER Navigate (S1-S7)</h2>';\n";
            js += L"h+='<p class=\"hint\" style=\"margin-top:-4px\">Parent calls Navigate(about:blank), waits for completion, THEN sets partition.</p>';\n";
            js += L"h+='<div class=\"sc\" onclick=\"window.chrome.webview.postMessage(\\x271\\x27)\">';\n";
            js += L"h+='<span class=\"num\">1</span>';\n";
            js += L"h+='<span class=\"lbl\">New WebView | No Partition</span>';\n";
            js += L"h+='<div class=\"desc\">New child WebView, NO partition set. Does it inherit?</div>';\n";
            js += L"h+='</div>';\n";
            js += L"h+='<div class=\"sc\" onclick=\"window.chrome.webview.postMessage(\\x272\\x27)\">';\n";
            js += L"h+='<span class=\"num\">2</span>';\n";
            js += L"h+='<span class=\"lbl\">New WebView | Same Partition</span>';\n";
            js += L"h+='<div class=\"desc\">New child WebView, SAME partition (demoPartition) set.</div>';\n";
            js += L"h+='</div>';\n";
            js += L"h+='<div class=\"sc\" onclick=\"window.chrome.webview.postMessage(\\x273\\x27)\">';\n";
            js += L"h+='<span class=\"num\">3</span>';\n";
            js += L"h+='<span class=\"lbl\">New WebView | Different Partition</span>';\n";
            js += L"h+='<div class=\"desc\">New child WebView, DIFFERENT partition (otherPartition) set.</div>';\n";
            js += L"h+='</div>';\n";
            js += L"h+='<div class=\"sc\" onclick=\"window.chrome.webview.postMessage(\\x274\\x27)\">';\n";
            js += L"h+='<span class=\"num\">4</span>';\n";
            js += L"h+='<span class=\"lbl\">New WebView | Default Partition</span>';\n";
            js += L"h+='<div class=\"desc\">New child WebView, DEFAULT/empty partition set.</div>';\n";
            js += L"h+='</div>';\n";

            js += L"h+='<div class=\"sc\" onclick=\"window.chrome.webview.postMessage(\\x275\\x27)\">';\n";
            js += L"h+='<span class=\"num\">5</span>';\n";
            js += L"h+='<span class=\"lbl\">Parent Reused | Inherits Partition</span>';\n";
            js += L"h+='<div class=\"desc\">Parent WebView reused. No partition change - inherits demoPartition.</div>';\n";
            js += L"h+='</div>';\n";
            js += L"h+='<div class=\"sc\" onclick=\"window.chrome.webview.postMessage(\\x276\\x27)\">';\n";
            js += L"h+='<span class=\"num\">6</span>';\n";
            js += L"h+='<span class=\"lbl\">Parent Reused | Reset to Default</span>';\n";
            js += L"h+='<div class=\"desc\">Parent WebView reused. Partition reset to default before put_NewWindow.</div>';\n";
            js += L"h+='</div>';\n";
            js += L"h+='<div class=\"sc\" onclick=\"window.chrome.webview.postMessage(\\x277\\x27)\">';\n";
            js += L"h+='<span class=\"num\">7</span>';\n";
            js += L"h+='<span class=\"lbl\">Parent Reused | Own Custom Partition</span>';\n";
            js += L"h+='<div class=\"desc\">Parent WebView reused. Partition changed to otherPartition before put_NewWindow.</div>';\n";
            js += L"h+='</div>';\n";

            js += L"h+='<h2>\\u2461 Partition BEFORE Navigate (S15-S21)</h2>';\n";
            js += L"h+='<p class=\"hint\" style=\"margin-top:-4px\">Parent sets partition FIRST, then calls Navigate(about:blank).</p>';\n";
            js += L"h+='<div class=\"sc\" onclick=\"window.chrome.webview.postMessage(\\x2715\\x27)\">';\n";
            js += L"h+='<span class=\"num\">15</span>';\n";
            js += L"h+='<span class=\"lbl\">Before Nav | New WV | No Partition</span>';\n";
            js += L"h+='<div class=\"desc\">Like S1: new child, no partition.</div>';\n";
            js += L"h+='</div>';\n";
            js += L"h+='<div class=\"sc\" onclick=\"window.chrome.webview.postMessage(\\x2716\\x27)\">';\n";
            js += L"h+='<span class=\"num\">16</span>';\n";
            js += L"h+='<span class=\"lbl\">Before Nav | New WV | Same Partition</span>';\n";
            js += L"h+='<div class=\"desc\">Like S2: new child, same partition.</div>';\n";
            js += L"h+='</div>';\n";
            js += L"h+='<div class=\"sc\" onclick=\"window.chrome.webview.postMessage(\\x2717\\x27)\">';\n";
            js += L"h+='<span class=\"num\">17</span>';\n";
            js += L"h+='<span class=\"lbl\">Before Nav | New WV | Different Partition</span>';\n";
            js += L"h+='<div class=\"desc\">Like S3: new child, different partition.</div>';\n";
            js += L"h+='</div>';\n";
            js += L"h+='<div class=\"sc\" onclick=\"window.chrome.webview.postMessage(\\x2718\\x27)\">';\n";
            js += L"h+='<span class=\"num\">18</span>';\n";
            js += L"h+='<span class=\"lbl\">Before Nav | New WV | Default Partition</span>';\n";
            js += L"h+='<div class=\"desc\">Like S4: new child, default partition.</div>';\n";
            js += L"h+='</div>';\n";
            js += L"h+='<div class=\"sc\" onclick=\"window.chrome.webview.postMessage(\\x2719\\x27)\">';\n";
            js += L"h+='<span class=\"num\">19</span>';\n";
            js += L"h+='<span class=\"lbl\">Before Nav | Reused | Inherits</span>';\n";
            js += L"h+='<div class=\"desc\">Like S5: parent reused, inherits partition.</div>';\n";
            js += L"h+='</div>';\n";
            js += L"h+='<div class=\"sc\" onclick=\"window.chrome.webview.postMessage(\\x2720\\x27)\">';\n";
            js += L"h+='<span class=\"num\">20</span>';\n";
            js += L"h+='<span class=\"lbl\">Before Nav | Reused | Default</span>';\n";
            js += L"h+='<div class=\"desc\">Like S6: parent reused, reset to default.</div>';\n";
            js += L"h+='</div>';\n";
            js += L"h+='<div class=\"sc\" onclick=\"window.chrome.webview.postMessage(\\x2721\\x27)\">';\n";
            js += L"h+='<span class=\"num\">21</span>';\n";
            js += L"h+='<span class=\"lbl\">Before Nav | Reused | Own Partition</span>';\n";
            js += L"h+='<div class=\"desc\">Like S7: parent reused, own partition.</div>';\n";
            js += L"h+='</div>';\n";

            js += L"h+='<h2>\\u2462 Implicit Navigation (S8-S14)</h2>';\n";
            js += L"h+='<p class=\"hint\" style=\"margin-top:-4px\">Same as above but parent does NOT call Navigate(about:blank). Relies on WebView2 auto-navigation at creation.</p>';\n";
            js += L"h+='<div class=\"sc\" onclick=\"window.chrome.webview.postMessage(\\x278\\x27)\">';\n";
            js += L"h+='<span class=\"num\">8</span>';\n";
            js += L"h+='<span class=\"lbl\">Implicit Nav | New WV | No Partition</span>';\n";
            js += L"h+='<div class=\"desc\">Like S1: new child, no partition. Parent auto-navigates.</div>';\n";
            js += L"h+='</div>';\n";
            js += L"h+='<div class=\"sc\" onclick=\"window.chrome.webview.postMessage(\\x279\\x27)\">';\n";
            js += L"h+='<span class=\"num\">9</span>';\n";
            js += L"h+='<span class=\"lbl\">Implicit Nav | New WV | Same Partition</span>';\n";
            js += L"h+='<div class=\"desc\">Like S2: new child, same partition. Parent auto-navigates.</div>';\n";
            js += L"h+='</div>';\n";
            js += L"h+='<div class=\"sc\" onclick=\"window.chrome.webview.postMessage(\\x2710\\x27)\">';\n";
            js += L"h+='<span class=\"num\">10</span>';\n";
            js += L"h+='<span class=\"lbl\">Implicit Nav | New WV | Different Partition</span>';\n";
            js += L"h+='<div class=\"desc\">Like S3: new child, different partition. Parent auto-navigates.</div>';\n";
            js += L"h+='</div>';\n";
            js += L"h+='<div class=\"sc\" onclick=\"window.chrome.webview.postMessage(\\x2711\\x27)\">';\n";
            js += L"h+='<span class=\"num\">11</span>';\n";
            js += L"h+='<span class=\"lbl\">Implicit Nav | New WV | Default Partition</span>';\n";
            js += L"h+='<div class=\"desc\">Like S4: new child, default partition. Parent auto-navigates.</div>';\n";
            js += L"h+='</div>';\n";
            js += L"h+='<div class=\"sc\" onclick=\"window.chrome.webview.postMessage(\\x2712\\x27)\">';\n";
            js += L"h+='<span class=\"num\">12</span>';\n";
            js += L"h+='<span class=\"lbl\">Implicit Nav | Reused | Inherits</span>';\n";
            js += L"h+='<div class=\"desc\">Like S5: parent reused, inherits partition. Parent auto-navigates.</div>';\n";
            js += L"h+='</div>';\n";
            js += L"h+='<div class=\"sc\" onclick=\"window.chrome.webview.postMessage(\\x2713\\x27)\">';\n";
            js += L"h+='<span class=\"num\">13</span>';\n";
            js += L"h+='<span class=\"lbl\">Implicit Nav | Reused | Default</span>';\n";
            js += L"h+='<div class=\"desc\">Like S6: parent reused, reset to default. Parent auto-navigates.</div>';\n";
            js += L"h+='</div>';\n";
            js += L"h+='<div class=\"sc\" onclick=\"window.chrome.webview.postMessage(\\x2714\\x27)\">';\n";
            js += L"h+='<span class=\"num\">14</span>';\n";
            js += L"h+='<span class=\"lbl\">Implicit Nav | Reused | Own Partition</span>';\n";
            js += L"h+='<div class=\"desc\">Like S7: parent reused, own partition. Parent auto-navigates.</div>';\n";
            js += L"h+='</div>';\n";

            js += L"document.body.innerHTML=h;\n";
            js += L"})();\n";

            g_launcherWv->ExecuteScript(js.c_str(), nullptr);
            return S_OK;
          }).Get());
}

// ==========================================================================
// Entry Point
// ==========================================================================
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
  g_hInst = hInst;

  AllocConsole();
  FILE* dummy;
  freopen_s(&dummy, "CONOUT$", "w", stdout);
  Log("[INIT] PartitionDemo starting\n");

  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInst;
  wc.lpszClassName = kClassName;
  wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  RegisterClassExW(&wc);

  g_launcherHwnd = CreateHostWindow(
      L"Partition + NewWindow Demo", 900, 750, true);
  UpdateWindow(g_launcherHwnd);

  CreateCoreWebView2EnvironmentWithOptions(
      nullptr, nullptr, nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(hr)) {
              MessageBoxW(nullptr, L"WebView2 runtime not found.",
                          L"Error", MB_ICONERROR);
              PostQuitMessage(1);
              return hr;
            }
            g_env = env;
            InitLauncher();
            return S_OK;
          }).Get());

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  CoUninitialize();
  return (int)msg.wParam;
}
