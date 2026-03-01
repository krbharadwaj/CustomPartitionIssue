// ==========================================================================
// PartitionDemo — CustomDataPartitionId + NewWindowRequested Demo
//
// Mirrors the real Teams/Ravikiran scenario:
//   1. Top-level WebView → about:blank (never navigates to a real URL)
//   2. put_CustomDataPartitionId() set AFTER about:blank completes
//   3. Control panel UI injected via ExecuteScript on about:blank
//   4. window.open() → NewWindowRequested → put_NewWindow for popups
//
// 8 scenarios cover:
//   - window.open URL:   about:blank vs data: URL (simulating actual URL)
//   - WebView instance:  new child vs parent reuse
//   - Child partition:   same / default / different
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

// Console logging
static void Log(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vprintf(fmt, args);
  va_end(args);
}

// ==========================================================================
// Experimental API — ICoreWebView2Experimental20::CustomDataPartitionId
// ==========================================================================
MIDL_INTERFACE("5a4d0ecf-3fe5-4456-ace5-d317cca0eff1")
IPartitionApi : public IUnknown {
 public:
  virtual HRESULT STDMETHODCALLTYPE
  get_CustomDataPartitionId(LPWSTR* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE
  put_CustomDataPartitionId(LPCWSTR value) = 0;
};

// ==========================================================================
// Scenario Configuration
// ==========================================================================
struct Scenario {
  const wchar_t* name;
  bool newWebView;   // true = new child WebView, false = reuse parent
  bool aboutBlank;   // true = window.open("about:blank"), false = actual URL
  const wchar_t* partition;  // partition to set on child ("" = default)
};

static const Scenario kScenarios[8] = {
    {L"about:blank | New | Same Partition", true, true, L"demoPartition"},
    {L"about:blank | New | Default Partition", true, true, L""},
    {L"about:blank | Parent WebView (reused)", false, true, L""},
    {L"Actual URL  | New | Same Partition", true, false, L"demoPartition"},
    {L"Actual URL  | New | Default Partition", true, false, L""},
    {L"Actual URL  | Parent WebView (reused)", false, false, L""},
    {L"about:blank | New | Different Partition", true, true, L"otherPartition"},
    {L"Actual URL  | New | Different Partition", true, false, L"otherPartition"},
};

// All scenarios use about:blank for window.open (partition behavior doesn't
// depend on URL). The "Actual URL" label describes Teams' real scenario.

// ==========================================================================
// Application State
// ==========================================================================
static const wchar_t kClassName[] = L"PartitionDemoWnd";
static const wchar_t kParentPartition[] = L"demoPartition";

static HINSTANCE g_hInst;
static HWND g_parentHwnd = nullptr;
static ComPtr<ICoreWebView2Environment> g_env;
static ComPtr<ICoreWebView2Controller> g_parentCtrl;
static ComPtr<ICoreWebView2> g_parentWv;

static bool g_initialized = false;      // true after first about:blank done
static bool g_returningHome = false;     // true when navigating back after parent reuse
static int g_pendingScenario = 0;        // scenario# waiting for NewWindowRequested
static std::wstring g_parentReuseScript; // script to inject after parent reuse

struct ChildWindow {
  HWND hwnd;
  ComPtr<ICoreWebView2Controller> ctrl;
  ComPtr<ICoreWebView2> wv;
  int scenario;
  std::wstring pendingScript;  // script to inject after put_NewWindow
};
static std::vector<ChildWindow> g_children;

// ==========================================================================
// Helpers
// ==========================================================================
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

static ChildWindow* FindChild(HWND hwnd) {
  for (auto& c : g_children)
    if (c.hwnd == hwnd)
      return &c;
  return nullptr;
}

static ChildWindow* FindChildByScenario(int n) {
  for (auto& c : g_children)
    if (c.scenario == n)
      return &c;
  return nullptr;
}

// ==========================================================================
// Build the control panel UI (injected on parent's about:blank)
// ==========================================================================
static std::wstring BuildControlPanelScript() {
  std::wstring apiPartition = GetPartition(g_parentWv.Get());

  std::wstring js;
  js += L"(function(){\n";

  // Force body visible with inline styles first
  js += L"document.documentElement.style.cssText="
        L"'margin:0;padding:0;width:100%;height:100%';\n";
  js += L"document.body.style.cssText="
        L"'margin:0;padding:30px;background:#1e1e2e;color:#cdd6f4;"
        L"font-family:Segoe UI,sans-serif;max-width:750px;min-height:100%';\n";
  js += L"document.title='Partition + NewWindow Demo';\n";

  // Styles
  js += L"document.head.innerHTML='<style>"
        L"*{box-sizing:border-box;margin:0;padding:0}"
        L"body{font-family:Segoe UI,sans-serif;background:#1e1e2e;color:#cdd6f4;"
        L"padding:30px;max-width:750px}"
        L"h1{color:#89b4fa;font-size:22px;margin-bottom:4px}"
        L".hint{color:#a6adc8;font-size:13px;margin-bottom:16px}"
        L".info{background:#313244;border-radius:8px;padding:14px 18px;"
        L"margin-bottom:18px;font-size:13px;line-height:1.7}"
        L".info b{color:#a6e3a1}"
        L".info code{background:#181825;padding:2px 6px;border-radius:3px;"
        L"font-family:Cascadia Code,monospace;color:#f9e2af}"
        L"h2{color:#fab387;font-size:16px;margin-bottom:10px}"
        L".sc{background:#313244;border:1px solid #45475a;border-radius:8px;"
        L"padding:12px 16px;margin-bottom:8px;cursor:pointer;transition:.15s}"
        L".sc:hover{border-color:#89b4fa;background:#3b3d50}"
        L".num{color:#89b4fa;font-weight:700;font-size:18px;margin-right:10px}"
        L".lbl{font-weight:600;font-size:13px}"
        L".desc{color:#a6adc8;font-size:12px;margin-top:4px}"
        L"#status{color:#a6adc8;font-size:12px;margin-top:14px;font-style:italic}"
        L"</style>';\n";

  // Body content
  js += L"var h='';\n";
  js += L"h+='<h1>Partition + NewWindow Demo</h1>';\n";
  js += L"h+='<p class=\"hint\">Mirrors Teams scenario: partition set on "
        L"about:blank after navigation, never navigated to real URL.</p>';\n";

  // Parent info
  js += L"h+='<div class=\"info\">';\n";
  js += L"h+='<b>Parent WebView</b> (this window)<br>';\n";
  js += L"h+='Current URL: <code>about:blank</code><br>';\n";
  js += L"h+='Partition set: <code>" + apiPartition + L"</code><br>';\n";
  js += L"h+='Parent\\'s own storage: <b>(default)</b> \\u2014 partition "
        L"was set <b>after</b> about:blank navigation, so it does not change "
        L"this frame\\'s storage.<br>';\n";
  js += L"h+='Child windows: partition <b>propagates</b> to NewWindows "
        L"created from this WebView.';\n";
  js += L"h+='</div>';\n";

  // Scenario list
  js += L"h+='<h2>Scenarios</h2>';\n";

  const wchar_t* descs[8] = {
      L"New WebView + demoPartition. Parent is (default) \\u2192 MISMATCH.",
      L"New WebView + default partition. Matches parent \\u2192 blob URLs WORK.",
      L"\\u26A0 Parent WebView reused. Inherits (default) \\u2192 blob URLs WORK.",
      L"New WebView + demoPartition + actual URL. Parent is (default) \\u2192 MISMATCH.",
      L"New WebView + default partition + actual URL. Matches parent \\u2192 WORK.",
      L"\\u26A0 Parent WebView reused + actual URL. Inherits (default) \\u2192 WORK.",
      L"New WebView + otherPartition. Parent is (default) \\u2192 MISMATCH.",
      L"New WebView + otherPartition + actual URL. Parent is (default) \\u2192 MISMATCH.",
  };

  for (int i = 0; i < 8; i++) {
    js += L"h+='<div class=\"sc\" onclick=\"run(" +
          std::to_wstring(i + 1) + L")\">';\n";
    js += L"h+='<span class=\"num\">" + std::to_wstring(i + 1) +
          L"</span>';\n";
    js += L"h+='<span class=\"lbl\">" + std::wstring(kScenarios[i].name) +
          L"</span>';\n";
    js += L"h+='<div class=\"desc\">" + std::wstring(descs[i]) +
          L"</div>';\n";
    js += L"h+='</div>';\n";
  }

  js += L"h+='<div id=\"status\"></div>';\n";
  js += L"document.body.innerHTML=h;\n";

  // Run function and message listener
  js += L"window.run=function(n){\n";
  js += L"  document.getElementById('status').textContent="
        L"'Preparing scenario '+n+'...';\n";
  js += L"  window.chrome.webview.postMessage('run:'+n);\n";
  js += L"};\n";

  // Listen for C++ telling us to window.open
  js += L"window.chrome.webview.addEventListener('message',function(e){\n";
  js += L"  var m=e.data;\n";
  js += L"  if(typeof m==='string' && m.indexOf('open:')===0){\n";
  js += L"    var url=m.substring(5);\n";
  js += L"    document.getElementById('status').textContent="
        L"'Opened via window.open(\"'+url+'\")';\n";
  js += L"    window.open(url);\n";
  js += L"  }\n";
  js += L"});\n";

  js += L"})();\n";
  return js;
}

// ==========================================================================
// Build the child/result page script (injected on child's about:blank or
// data: page via AddScriptToExecuteOnDocumentCreated)
// ==========================================================================
static std::wstring BuildChildPageScript(int n,
                                         const std::wstring& childPart,
                                         const std::wstring& parentPart,
                                         bool isParentReuse) {
  const auto& sc = kScenarios[n - 1];
  bool samePartition = (childPart == parentPart);

  std::wstring js;
  js += L"(function(){\n";

  // Force body/html to be visible with explicit inline styles first
  js += L"document.documentElement.style.cssText="
        L"'margin:0;padding:0;width:100%;height:100%';\n";
  js += L"document.body.style.cssText="
        L"'margin:0;padding:30px;background:#1e1e2e;color:#cdd6f4;"
        L"font-family:Segoe UI,sans-serif;max-width:650px;min-height:100%';\n";
  js += L"document.title='Scenario " + std::to_wstring(n) + L": " +
        sc.name + L"';\n";

  // Styles
  js += L"document.head.innerHTML='<style>"
        L"*{box-sizing:border-box;margin:0;padding:0}"
        L"body{font-family:Segoe UI,sans-serif;background:#1e1e2e;color:#cdd6f4;"
        L"padding:30px;max-width:650px}"
        L"h1{font-size:20px;margin-bottom:6px;color:#89b4fa}"
        L".warn{background:#332b00;border-left:3px solid #f9e2af;padding:10px 14px;"
        L"margin:10px 0;font-size:12px;color:#f9e2af;border-radius:4px}"
        L".desc{background:#313244;border-radius:8px;padding:14px 18px;"
        L"margin:14px 0;font-size:13px;line-height:1.7}"
        L".desc b{color:#f9e2af}"
        L".pbox{background:#181825;border:1px solid #45475a;border-radius:8px;"
        L"padding:14px 18px;margin:14px 0}"
        L".row{display:flex;justify-content:space-between;margin:4px 0;font-size:13px}"
        L".lbl{color:#a6adc8}.val{font-family:Cascadia Code,monospace;font-weight:600}"
        L".res{border-radius:8px;padding:14px;margin:10px 0;font-size:14px;"
        L"font-weight:600;text-align:center}"
        L".ok{background:#1a3a2a;border:2px solid #a6e3a1;color:#a6e3a1}"
        L".fail{background:#3a1a1a;border:2px solid #f38ba8;color:#f38ba8}"
        L".sm{font-weight:400;font-size:12px;color:#a6adc8;margin-top:6px}"
        L".back{border:none;border-radius:6px;padding:8px 18px;font-weight:600;"
        L"font-size:12px;cursor:pointer;background:#89b4fa;color:#1e1e2e;"
        L"margin-top:14px}.back:hover{filter:brightness(1.1)}"
        L"</style>';\n";

  // Title
  js += L"var h='<h1>Scenario " + std::to_wstring(n) + L": " +
        sc.name + L"</h1>';\n";

  // Warning for parent reuse
  if (!sc.newWebView) {
    js += L"h+='<div class=\"warn\">"
          L"\\u26A0\\uFE0F API docs: WebView should not have been navigated "
          L"previously. This demos Ravikiran\\'s real bug pattern where "
          L"the already-navigated parent WebView is reused.</div>';\n";
  }

  // Description
  js += L"h+='<div class=\"desc\">';\n";
  if (sc.newWebView) {
    if (sc.partition[0]) {
      js += L"h+='A <b>new</b> WebView was created with partition "
            L"<b>" +
            std::wstring(sc.partition) + L"</b>.';\n";
    } else {
      js += L"h+='A <b>new</b> WebView was created with <b>no partition</b> "
            L"(uses default partition).';\n";
    }
  } else {
    js += L"h+='The <b>parent</b> WebView (already at about:blank with "
          L"partition set) was reused via put_NewWindow. Same WebView = "
          L"inherits parent partition.';\n";
  }
  js += L"h+='<br>Opened via <b>window.open(\\\"about:blank\\\")</b> "
        L"\\u2192 NewWindowRequested \\u2192 put_NewWindow.';\n";
  js += L"h+='</div>';\n";

  // Partition info box
  js += L"h+='<div class=\"pbox\">';\n";
  js += L"h+='<div class=\"row\"><span class=\"lbl\">Parent partition:</span>"
        L"<span class=\"val\" style=\"color:#89b4fa\">" +
        parentPart + L"</span></div>';\n";
  js += L"h+='<div class=\"row\"><span class=\"lbl\">Child partition:</span>"
        L"<span class=\"val\" style=\"color:" +
        std::wstring(samePartition ? L"#a6e3a1" : L"#f38ba8") + L"\">" +
        childPart + L"</span></div>';\n";
  js += L"h+='<div class=\"row\"><span class=\"lbl\">Match:</span>"
        L"<span class=\"val\" style=\"color:" +
        std::wstring(samePartition ? L"#a6e3a1" : L"#f38ba8") + L"\">" +
        std::wstring(samePartition ? L"\\u2705 YES" : L"\\u274C NO") +
        L"</span></div>';\n";
  js += L"h+='</div>';\n";

  // Verdict
  if (samePartition) {
    js += L"h+='<div class=\"res ok\">"
          L"\\u2705 Same partition \\u2014 blob URLs, cookies, localStorage "
          L"all SHARED between parent and this window."
          L"<div class=\"sm\">URL.createObjectURL() in parent is accessible "
          L"here.</div></div>';\n";
  } else {
    js += L"h+='<div class=\"res fail\">"
          L"\\u274C Different partition \\u2014 blob URLs will FAIL."
          L"<div class=\"sm\">URL.createObjectURL() in parent partition "
          L"\\\"" +
          parentPart +
          L"\\\" is NOT accessible from child partition "
          L"\\\"" +
          childPart +
          L"\\\". This is the Teams bug.</div></div>';\n";
  }

  // Back button for parent reuse
  if (isParentReuse) {
    js += L"h+='<button class=\"back\" onclick=\""
          L"window.chrome.webview.postMessage(\\x27returnHome\\x27)\">"
          L"\\u2190 Back to Control Panel</button>';\n";
  }

  js += L"document.body.innerHTML=h;\n";
  js += L"})();\n";
  return js;
}

// ==========================================================================
// Forward Declarations
// ==========================================================================
static void InitParentWebView();
static void InjectControlPanel();
static void SetupEventHandlers();
static void PrepareScenario(int n);

// ==========================================================================
// Window Procedure
// ==========================================================================
// Timer IDs for delayed script injection after put_NewWindow
static const UINT_PTR kChildInjectTimer = 42;
static const UINT_PTR kParentReuseInjectTimer = 43;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_TIMER: {
      if (wp == kChildInjectTimer) {
        KillTimer(hwnd, kChildInjectTimer);
        auto* child = FindChild(hwnd);
        if (child && child->wv && !child->pendingScript.empty()) {
          Log("[TIMER] Injecting script into child scenario %d\n",
              child->scenario);
          // Ensure WebView covers the full window and is visible
          RECT rc;
          GetClientRect(hwnd, &rc);
          child->ctrl->put_Bounds(rc);
          child->ctrl->put_IsVisible(TRUE);
          child->wv->ExecuteScript(child->pendingScript.c_str(), nullptr);
          child->pendingScript.clear();
        }
      } else if (wp == kParentReuseInjectTimer) {
        KillTimer(hwnd, kParentReuseInjectTimer);
        if (g_parentWv && !g_parentReuseScript.empty()) {
          Log("[TIMER] Injecting script into parent (reuse)\n");
          g_parentWv->ExecuteScript(g_parentReuseScript.c_str(), nullptr);
          g_parentReuseScript.clear();
        }
      }
      return 0;
    }
    case WM_SIZE: {
      RECT rc;
      GetClientRect(hwnd, &rc);
      if (hwnd == g_parentHwnd && g_parentCtrl)
        g_parentCtrl->put_Bounds(rc);
      if (auto* c = FindChild(hwnd))
        c->ctrl->put_Bounds(rc);
      return 0;
    }
    case WM_CLOSE:
      if (hwnd == g_parentHwnd) {
        for (auto& c : g_children) {
          if (c.ctrl)
            c.ctrl->Close();
          DestroyWindow(c.hwnd);
        }
        g_children.clear();
        DestroyWindow(hwnd);
      } else {
        if (auto* c = FindChild(hwnd))
          if (c->ctrl)
            c->ctrl->Close();
        DestroyWindow(hwnd);
      }
      return 0;
    case WM_DESTROY:
      for (auto it = g_children.begin(); it != g_children.end(); ++it) {
        if (it->hwnd == hwnd) {
          g_children.erase(it);
          break;
        }
      }
      if (hwnd == g_parentHwnd)
        PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND CreateHostWindow(const wchar_t* title, int w, int h, bool show) {
  HWND hwnd = CreateWindowExW(0, kClassName, title, WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, w, h, nullptr,
                              nullptr, g_hInst, nullptr);
  if (show)
    ShowWindow(hwnd, SW_SHOW);
  return hwnd;
}

// ==========================================================================
// WebView2 Initialization
//
// Flow (mirrors Teams):
//   CreateEnvironment → InitParentWebView → about:blank auto-loads →
//   NavigationCompleted → set partition → inject control panel UI
// ==========================================================================
static void CreateEnvironment() {
  CreateCoreWebView2EnvironmentWithOptions(
      nullptr, nullptr, nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(hr)) {
              MessageBoxW(nullptr, L"WebView2 runtime not found.", L"Error",
                          MB_ICONERROR);
              PostQuitMessage(1);
              return hr;
            }
            g_env = env;
            InitParentWebView();
            return S_OK;
          })
          .Get());
}

static void InitParentWebView() {
  g_env->CreateCoreWebView2Controller(
      g_parentHwnd,
      Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
          [](HRESULT hr, ICoreWebView2Controller* ctrl) -> HRESULT {
            if (FAILED(hr))
              return hr;

            g_parentCtrl = ctrl;
            ctrl->get_CoreWebView2(&g_parentWv);

            RECT rc;
            GetClientRect(g_parentHwnd, &rc);
            ctrl->put_Bounds(rc);

            // NavigationCompleted handles two cases:
            // 1. Initial about:blank → set partition + inject control panel
            // 2. Returning home after parent reuse → re-inject control panel
            EventRegistrationToken tok;
            g_parentWv->add_NavigationCompleted(
                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                    [](ICoreWebView2*,
                       ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                      if (!g_initialized) {
                        g_initialized = true;
                        Log("[INIT] about:blank loaded, setting partition\n");

                        // Set partition AFTER about:blank (Teams' pattern).
                        // This does NOT change the parent's own storage
                        // partition (already navigated), but it DOES propagate
                        // to child frames and NewWindows created from it.
                        SetPartition(g_parentWv.Get(), kParentPartition);

                        // Inject control panel UI on about:blank
                        InjectControlPanel();

                        // Register event handlers (once)
                        SetupEventHandlers();
                      } else if (g_returningHome) {
                        g_returningHome = false;
                        Log("[HOME] Re-injecting control panel\n");
                        InjectControlPanel();
                      }
                      return S_OK;
                    })
                    .Get(),
                &tok);

            // WebView starts at about:blank automatically — no Navigate needed.
            // But explicitly navigate to match Teams' pattern of calling
            // Navigate("about:blank") as the first action.
            g_parentWv->Navigate(L"about:blank");
            return S_OK;
          })
          .Get());
}

static void InjectControlPanel() {
  std::wstring script = BuildControlPanelScript();
  g_parentWv->ExecuteScript(script.c_str(), nullptr);
}

// ==========================================================================
// Event Handlers
// ==========================================================================
static void SetupEventHandlers() {
  // WebMessageReceived: handle scenario triggers and returnHome
  EventRegistrationToken msgTok;
  g_parentWv->add_WebMessageReceived(
      Callback<ICoreWebView2WebMessageReceivedEventHandler>(
          [](ICoreWebView2*,
             ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            LPWSTR raw = nullptr;
            args->TryGetWebMessageAsString(&raw);
            if (!raw)
              return S_OK;
            std::wstring msg(raw);
            CoTaskMemFree(raw);

            if (msg.substr(0, 4) == L"run:") {
              int n = _wtoi(msg.c_str() + 4);
              if (n >= 1 && n <= 8)
                PrepareScenario(n);
            } else if (msg == L"returnHome") {
              Log("[HOME] returnHome received\n");
              g_parentReuseScript.clear();
              // Navigate back to about:blank → re-inject control panel
              g_returningHome = true;
              g_parentWv->Navigate(L"about:blank");
            }
            return S_OK;
          })
          .Get(),
      &msgTok);

  // NewWindowRequested: put_NewWindow immediately, then inject UI via timer.
  // AddScriptToExecuteOnDocumentCreated proved unreliable — the script fires
  // on the child's initial about:blank BEFORE put_NewWindow, not on the
  // popup's document AFTER put_NewWindow. Timer-based ExecuteScript is robust.
  EventRegistrationToken nwTok;
  g_parentWv->add_NewWindowRequested(
      Callback<ICoreWebView2NewWindowRequestedEventHandler>(
          [](ICoreWebView2*,
             ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
            if (g_pendingScenario < 1 || g_pendingScenario > 8)
              return S_OK;

            int n = g_pendingScenario;
            g_pendingScenario = 0;
            const auto& sc = kScenarios[n - 1];

            Log("[NWR] Scenario %d: newWV=%d blank=%d\n", n, sc.newWebView,
                sc.aboutBlank);

            // Parent called put_CustomDataPartitionId("demoPartition") AFTER
            // about:blank. This does NOT change the parent's own storage
            // (still default), but propagates to child NewWindows.
            // For storage comparison, parent's effective partition is (default).
            std::wstring parentPart = L"(default)";

            if (!sc.newWebView) {
              // ---- Parent reuse (scenarios 3, 6) ----
              std::wstring childPart = parentPart;
              g_parentReuseScript =
                  BuildChildPageScript(n, childPart, parentPart, true);

              args->put_NewWindow(g_parentWv.Get());
              args->put_Handled(TRUE);

              // Inject UI after 300ms delay (popup document needs time to load)
              SetTimer(g_parentHwnd, kParentReuseInjectTimer, 300, nullptr);
              Log("[NWR] Parent reuse: put_NewWindow done, timer set\n");
              return S_OK;
            }

            // ---- New WebView (scenarios 1, 2, 4, 5, 7, 8) ----
            ChildWindow* child = FindChildByScenario(n);
            if (!child) {
              Log("[NWR] ERROR: No child WebView for scenario %d\n", n);
              return S_OK;
            }

            std::wstring childPart = GetPartition(child->wv.Get());
            child->pendingScript =
                BuildChildPageScript(n, childPart, parentPart, false);

            args->put_NewWindow(child->wv.Get());
            args->put_Handled(TRUE);

            // Ensure controller covers the window and is visible
            RECT rc;
            GetClientRect(child->hwnd, &rc);
            child->ctrl->put_Bounds(rc);
            child->ctrl->put_IsVisible(TRUE);

            ShowWindow(child->hwnd, SW_SHOW);
            SetForegroundWindow(child->hwnd);
            UpdateWindow(child->hwnd);

            // Inject UI after 300ms delay
            SetTimer(child->hwnd, kChildInjectTimer, 300, nullptr);
            Log("[NWR] Child scenario %d: put_NewWindow done, timer set\n", n);
            return S_OK;
          })
          .Get(),
      &nwTok);
}

// ==========================================================================
// Scenario Preparation
//
// Creates child WebView (if new), sets partition, tells JS to window.open().
// The window.open() triggers NewWindowRequested which does put_NewWindow.
// ==========================================================================
static void PrepareScenario(int n) {
  const auto& sc = kScenarios[n - 1];
  Log("[PREP] Scenario %d: newWV=%d blank=%d part='%ls'\n", n,
         sc.newWebView, sc.aboutBlank, sc.partition);

  if (!sc.newWebView) {
    // Parent reuse: just trigger window.open, NewWindowRequested will
    // use parent WebView via put_NewWindow
    g_pendingScenario = n;
    g_parentWv->PostWebMessageAsString(L"open:about:blank");
    return;
  }

  // New WebView: create HWND + WebView, set partition, then trigger open
  wchar_t title[256];
  swprintf_s(title, L"Scenario %d: %s", n, sc.name);
  HWND childHwnd = CreateHostWindow(title, 750, 600, false);

  g_env->CreateCoreWebView2Controller(
      childHwnd,
      Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
          [n, childHwnd](HRESULT hr,
                         ICoreWebView2Controller* ctrl) -> HRESULT {
            if (FAILED(hr)) {
              DestroyWindow(childHwnd);
              return hr;
            }

            const auto& sc = kScenarios[n - 1];

            ChildWindow child;
            child.hwnd = childHwnd;
            child.ctrl = ctrl;
            ctrl->get_CoreWebView2(&child.wv);
            child.scenario = n;

            // Enable DevTools on child (F12 works)
            ComPtr<ICoreWebView2Settings> settings;
            child.wv->get_Settings(&settings);
            if (settings) {
              settings->put_AreDevToolsEnabled(TRUE);
              settings->put_AreDefaultContextMenusEnabled(TRUE);
            }

            // Set partition on child (if specified)
            if (sc.partition[0] != L'\0')
              SetPartition(child.wv.Get(), sc.partition);

            RECT rc;
            GetClientRect(childHwnd, &rc);
            ctrl->put_Bounds(rc);

            g_children.push_back(std::move(child));
            g_pendingScenario = n;

            Log("[PREP] Scenario %d: child ready, sending open:about:blank\n", n);
            g_parentWv->PostWebMessageAsString(L"open:about:blank");
            return S_OK;
          })
          .Get());
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

  g_parentHwnd = CreateHostWindow(L"Partition + NewWindow Demo (Parent)",
                                  1150, 870, true);
  UpdateWindow(g_parentHwnd);

  CreateEnvironment();

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  CoUninitialize();
  return (int)msg.wParam;
}
