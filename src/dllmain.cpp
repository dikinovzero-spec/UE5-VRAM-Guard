#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <intrin.h>
#include <atomic>
#include <mutex>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <filesystem>

#pragma comment(lib, "dxguid.lib")

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace Config {
    UINT64 VRAM_BUDGET_CAP_BYTES = 2850ULL * 1024ULL * 1024ULL;
    double TARGET_FPS = 60.0;
    UINT64 LARGE_TEXTURE_DIM = 1024;
    bool BOOST_CPU_PRIORITY = true;
    bool AUTO_PATCH_UE5_INI = true;
    constexpr uint32_t MAX_DETAILED_LOGS = 60;

    void LoadIni() {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::filesystem::path iniPath = std::filesystem::path(exe).parent_path() / L"dxgi_optimizer.ini";
        if (!std::filesystem::exists(iniPath)) {
            std::wofstream out(iniPath, std::ios::out | std::ios::trunc);
            if (out.is_open()) {
                out << L"; UE5 VRAM Guard v2.2 configuration / конфигурация\n"
                    << L"; 4 GB GPU: VramBudgetMB=2850. 6 GB GPU: VramBudgetMB=4600.\n"
                    << L"; Set VramBudgetMB=0 to disable budget spoofing.\n"
                    << L"; Установите VramBudgetMB=0 для отключения спуфинга бюджета.\n\n"
                    << L"[Settings]\n"
                    << L"VramBudgetMB=2850\nTargetFPS=60\nLargeTextureDim=1024\n"
                    << L"BoostCpuPriority=1\nAutoPatchUE5Ini=1\n";
            }
        }
        int budgetMb = GetPrivateProfileIntW(L"Settings", L"VramBudgetMB", 2850, iniPath.c_str());
        int targetFps = GetPrivateProfileIntW(L"Settings", L"TargetFPS", 60, iniPath.c_str());
        int textureDim = GetPrivateProfileIntW(L"Settings", L"LargeTextureDim", 1024, iniPath.c_str());
        int boost = GetPrivateProfileIntW(L"Settings", L"BoostCpuPriority", 1, iniPath.c_str());
        int patch = GetPrivateProfileIntW(L"Settings", L"AutoPatchUE5Ini", 1, iniPath.c_str());
        VRAM_BUDGET_CAP_BYTES = budgetMb > 0 ? static_cast<UINT64>(budgetMb) * 1024ULL * 1024ULL : 0;
        TARGET_FPS = targetFps > 0 ? static_cast<double>(targetFps) : 0.0;
        LARGE_TEXTURE_DIM = textureDim > 0 ? static_cast<UINT64>(textureDim) : 1024;
        BOOST_CPU_PRIORITY = boost != 0;
        AUTO_PATCH_UE5_INI = patch != 0;
    }
}

class Logger {
public:
    static Logger& Get() { static Logger inst; return inst; }
    void Init() {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (m_init) return;
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        m_file.open(std::filesystem::path(exe).parent_path() / L"dxgi_optimizer.log", std::ios::out | std::ios::trunc);
        m_init = true;
        Write("[INFO] UE5 VRAM Guard v2.2 Universal Public Release initialized");
    }
    void Info(const std::string& s) { std::lock_guard<std::mutex> l(m_mtx); if (m_init) Write("[INFO] " + s); }
    void Warn(const std::string& s) { std::lock_guard<std::mutex> l(m_mtx); if (m_init) Write("[WARN] " + s); }
    void Error(const std::string& s) { std::lock_guard<std::mutex> l(m_mtx); if (m_init) Write("[ERR ] " + s); }
private:
    void Write(const std::string& s) {
        if (!m_file.is_open()) return;
        SYSTEMTIME st; GetLocalTime(&st);
        m_file << std::setfill('0') << "[" << std::setw(2) << st.wHour << ":" << std::setw(2) << st.wMinute << ":"
               << std::setw(2) << st.wSecond << "." << std::setw(3) << st.wMilliseconds << "] " << s << std::endl;
    }
    std::mutex m_mtx;
    std::ofstream m_file;
    bool m_init = false;
};

template <typename FuncPtr>
bool HookVTableSlot(void* pInterface, size_t slot, FuncPtr hook, FuncPtr* orig, const char* name) {
    if (!pInterface) return false;
    void** vtbl = *reinterpret_cast<void***>(pInterface);
    if (!vtbl || vtbl[slot] == reinterpret_cast<void*>(hook)) return false;
    DWORD oldProt = 0;
    if (!VirtualProtect(&vtbl[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt)) return false;
    if (*orig == nullptr) *orig = reinterpret_cast<FuncPtr>(vtbl[slot]);
    vtbl[slot] = reinterpret_cast<void*>(hook);
    DWORD dummy = 0;
    VirtualProtect(&vtbl[slot], sizeof(void*), oldProt, &dummy);
    std::ostringstream oss;
    oss << "Hooked VTable [" << name << "] slot " << slot;
    Logger::Get().Info(oss.str());
    return true;
}

namespace UE5ConfigOptimizer {
    void PatchIni(const std::filesystem::path& p) {
        try {
            std::filesystem::create_directories(p.parent_path());
            std::string existingContent;
            if (std::filesystem::exists(p)) {
                std::ifstream in(p);
                existingContent.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                size_t oldPos = existingContent.find("; [DXGI_PROXY_GTX1650_OPTIMIZER");
                if (oldPos != std::string::npos) {
                    while (oldPos > 0 && (existingContent[oldPos - 1] == '\n' || existingContent[oldPos - 1] == '\r'))
                        --oldPos;
                    existingContent.erase(oldPos);
                }
            }
            DWORD attr = GetFileAttributesW(p.c_str());
            bool ro = (attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_READONLY);
            if (ro) SetFileAttributesW(p.c_str(), attr & ~FILE_ATTRIBUTE_READONLY);
            std::ofstream out(p, std::ios::out | std::ios::trunc);
            if (out.is_open()) {
                out << existingContent
                    << "\n\n; [DXGI_PROXY_GTX1650_OPTIMIZER_V2_1]\n[SystemSettings]\n"
                    << "r.Shadow.Virtual.Enable=0\n"
                    << "r.Lumen.ScreenProbeGather.TracingOctahedronResolution=4\n"
                    << "r.LumenScene.SurfaceCache.CardCapturesPerFrame=3\n"
                    << "r.VolumetricFog.GridPixelSize=16\n"
                    << "r.VolumetricFog.GridSizeZ=64\n"
                    << "r.D3D12.AllowAsyncCompute=0\n"
                    << "r.Lumen.DiffuseIndirect.AsyncCompute=0\n"
                    << "r.Streaming.LimitPoolSizeToVRAM=1\n"
                    << "r.Streaming.PoolSize=1500\n"
                    << "r.Streaming.MaxTempMemoryAllowed=128\n"
                    << "r.Streaming.FramesForFullUpdate=15\n"
                    << "r.Streaming.MaxNumTexturesToStreamPerFrame=3\n"
                    << "s.AsyncLoadingTimeLimit=2.0\n"
                    << "s.PriorityAsyncLoadingExtraTime=0.0\n"
                    << "a.ParallelAnimEvaluation=1\n"
                    << "r.OneFrameThreadLag=1\n"
                    << "r.D3D12.UseAllowTearing=1\n"
                    << "D3D12.ResidencyManagement=1\n";
                out.close();
                Logger::Get().Info("Injected UE5 pipeline CVars into: " + p.string());
            }
            if (ro) SetFileAttributesW(p.c_str(), attr);
        } catch (...) {}
    }
    void Run() {
        if (!Config::AUTO_PATCH_UE5_INI) return;
        wchar_t buf[MAX_PATH] = {};
        if (!GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH)) return;
        std::filesystem::path base(buf);
        PatchIni(base / L"Stalker2/Saved/Config/Windows/Engine.ini");
        if (std::filesystem::exists(base / L"Stalker2/Saved/Config/WinGDK"))
            PatchIni(base / L"Stalker2/Saved/Config/WinGDK/Engine.ini");

        wchar_t exeBuf[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
        std::filesystem::path exePath(exeBuf);
        if (exePath.parent_path().filename() == L"Win64" &&
            exePath.parent_path().parent_path().filename() == L"Binaries") {
            std::filesystem::path projectDir = exePath.parent_path().parent_path().parent_path();
            std::wstring projectName = projectDir.filename().wstring();
            if (!projectName.empty() && projectName != L"Stalker2") {
                PatchIni(base / projectName / L"Saved/Config/Windows/Engine.ini");
                PatchIni(base / projectName / L"Saved/Config/WinGDK/Engine.ini");
                PatchIni(base / projectName / L"Saved/Config/WindowsNoEditor/Engine.ini");
            }
        }
    }
}

class HighPrecisionFramePacer {
public:
    static HighPrecisionFramePacer& Get() { static HighPrecisionFramePacer inst; return inst; }
    void Initialize() {
        std::call_once(m_flag, [this]() {
            QueryPerformanceFrequency(&m_freq);
            m_targetTicks = static_cast<LONGLONG>(static_cast<double>(m_freq.QuadPart) / Config::TARGET_FPS + 0.5);
            m_spinTicks = m_freq.QuadPart / 2000;
            m_yieldTicks = m_freq.QuadPart / 10000;
            m_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_MANUAL_RESET | CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (!m_timer) m_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_MANUAL_RESET, TIMER_ALL_ACCESS);
            Logger::Get().Info("HighPrecisionFramePacer initialized (16.666 ms / 60.0 FPS).");
        });
    }
    void PaceFrame(UINT flags) {
        if (Config::TARGET_FPS <= 0.0 || (flags & DXGI_PRESENT_TEST)) return;
        Initialize();
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        if (m_nextTick == 0) { m_nextTick = now.QuadPart + m_targetTicks; return; }
        LONGLONG rem = m_nextTick - now.QuadPart;
        if (rem > 0) {
            if (m_timer && rem > m_spinTicks) {
                LONGLONG sleepTicks = rem - m_spinTicks;
                LARGE_INTEGER due;
                due.QuadPart = -static_cast<LONGLONG>((static_cast<double>(sleepTicks) * 10000000.0) / static_cast<double>(m_freq.QuadPart));
                if (due.QuadPart < 0 && SetWaitableTimerEx(m_timer, &due, 0, nullptr, nullptr, nullptr, 0))
                    WaitForSingleObject(m_timer, 20);
            }
            for (;;) {
                QueryPerformanceCounter(&now);
                LONGLONG left = m_nextTick - now.QuadPart;
                if (left <= 0) break;
                if (left > m_yieldTicks) SwitchToThread(); else _mm_pause();
            }
        }
        m_nextTick += m_targetTicks;
        QueryPerformanceCounter(&now);
        if (now.QuadPart > m_nextTick) m_nextTick = now.QuadPart + m_targetTicks;
    }
private:
    std::once_flag m_flag;
    LARGE_INTEGER m_freq = {};
    LONGLONG m_targetTicks = 0, m_spinTicks = 0, m_yieldTicks = 0, m_nextTick = 0;
    HANDLE m_timer = nullptr;
};

namespace VramController {
    std::mutex g_mtx;
    std::unordered_map<ID3D12Heap*, int> g_heapRankMap;
    std::atomic<uint32_t> g_maxCnt{0}, g_highCnt{0}, g_lowCnt{0}, g_logCnt{0};

    bool IsBC(DXGI_FORMAT f) {
        return (f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC5_SNORM) ||
               (f >= DXGI_FORMAT_BC6H_TYPELESS && f <= DXGI_FORMAT_BC7_UNORM_SRGB);
    }
    int Classify(const D3D12_RESOURCE_DESC* d) {
        if (!d) return 0;
        if (d->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
            return 3;
        if (d->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
            return 2;
        if (d->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d->MipLevels >= 4 && (d->Width >= Config::LARGE_TEXTURE_DIM || d->Height >= Config::LARGE_TEXTURE_DIM) && IsBC(d->Format))
            return 1;
        return 0;
    }
    D3D12_RESIDENCY_PRIORITY ToD3DPrio(int c) {
        if (c == 3) return D3D12_RESIDENCY_PRIORITY_MAXIMUM;
        if (c == 2) return D3D12_RESIDENCY_PRIORITY_HIGH;
        return D3D12_RESIDENCY_PRIORITY_LOW;
    }
    const char* PrioName(int c) {
        if (c == 3) return "MAXIMUM(RT/UAV)";
        if (c == 2) return "HIGH(GeomBuffer)";
        return "LOW(MipTex)";
    }
    void SetPrio(ID3D12Device* dev, ID3D12Pageable* pg, const D3D12_RESOURCE_DESC* d, int c, const char* tag) {
        ID3D12Device1* dev1 = nullptr;
        if (!dev || !pg || FAILED(dev->QueryInterface(IID_PPV_ARGS(&dev1))) || !dev1) return;
        ID3D12Pageable* arr[1] = { pg };
        D3D12_RESIDENCY_PRIORITY prios[1] = { ToD3DPrio(c) };
        HRESULT hr = dev1->SetResidencyPriority(1, arr, prios);
        dev1->Release();
        if (SUCCEEDED(hr)) {
            if (c == 3) ++g_maxCnt; else if (c == 2) ++g_highCnt; else ++g_lowCnt;
            uint32_t idx = ++g_logCnt;
            if (idx <= Config::MAX_DETAILED_LOGS || idx % 500 == 0) {
                std::ostringstream oss;
                oss << "[" << tag << "] " << PrioName(c)
                    << " (" << (d ? d->Width : 0) << "x" << (d ? d->Height : 0) << ") Totals[MAX=" << g_maxCnt.load() << ", LOW=" << g_lowCnt.load() << "]";
                Logger::Get().Info(oss.str());
            }
        }
    }
}

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateCommittedResource)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreatePlacedResource)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_QueryVideoMemoryInfo)(IDXGIAdapter3*, UINT, DXGI_MEMORY_SEGMENT_GROUP, DXGI_QUERY_VIDEO_MEMORY_INFO*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_EnumAdapters)(IDXGIFactory*, UINT, IDXGIAdapter**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChain)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_EnumAdapters1)(IDXGIFactory1*, UINT, IDXGIAdapter1**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChainForHwnd)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_EnumAdapterByLuid)(IDXGIFactory4*, LUID, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_EnumAdapterByGpuPreference)(IDXGIFactory6*, UINT, DXGI_GPU_PREFERENCE, REFIID, void**);

static PFN_CreateCommittedResource oCreateCommittedResource = nullptr;
static PFN_CreatePlacedResource oCreatePlacedResource = nullptr;
static PFN_QueryVideoMemoryInfo oQueryVideoMemoryInfo = nullptr;
static PFN_Present oPresent = nullptr;
static PFN_Present1 oPresent1 = nullptr;
static PFN_EnumAdapters oEnumAdapters = nullptr;
static PFN_CreateSwapChain oCreateSwapChain = nullptr;
static PFN_EnumAdapters1 oEnumAdapters1 = nullptr;
static PFN_CreateSwapChainForHwnd oCreateSwapChainForHwnd = nullptr;
static PFN_EnumAdapterByLuid oEnumAdapterByLuid = nullptr;
static PFN_EnumAdapterByGpuPreference oEnumAdapterByGpuPreference = nullptr;

static std::mutex g_hookMtx;
static std::unordered_set<IDXGISwapChain*> g_latSet;

HRESULT STDMETHODCALLTYPE hkCreateCommittedResource(ID3D12Device* This, const D3D12_HEAP_PROPERTIES* pHeapProp, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitState, const D3D12_CLEAR_VALUE* pClear, REFIID riid, void** ppv) {
    HRESULT hr = oCreateCommittedResource(This, pHeapProp, HeapFlags, pDesc, InitState, pClear, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv && pHeapProp && pHeapProp->Type == D3D12_HEAP_TYPE_DEFAULT) {
        int c = VramController::Classify(pDesc);
        if (c != 0) {
            ID3D12Pageable* pg = nullptr;
            if (SUCCEEDED(static_cast<IUnknown*>(*ppv)->QueryInterface(IID_PPV_ARGS(&pg))) && pg) {
                VramController::SetPrio(This, pg, pDesc, c, "Committed");
                pg->Release();
            }
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hkCreatePlacedResource(ID3D12Device* This, ID3D12Heap* pHeap, UINT64 Offset, const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitState, const D3D12_CLEAR_VALUE* pClear, REFIID riid, void** ppv) {
    HRESULT hr = oCreatePlacedResource(This, pHeap, Offset, pDesc, InitState, pClear, riid, ppv);
    if (SUCCEEDED(hr) && pHeap && pDesc && pHeap->GetDesc().Properties.Type == D3D12_HEAP_TYPE_DEFAULT) {
        int c = VramController::Classify(pDesc);
        if (c > 0) {
            bool shouldElevate = false;
            {
                std::lock_guard<std::mutex> l(VramController::g_mtx);
                int& currentRank = VramController::g_heapRankMap[pHeap];
                if (c > currentRank) {
                    currentRank = c;
                    shouldElevate = true;
                }
            }
            if (shouldElevate) VramController::SetPrio(This, pHeap, pDesc, c, "PlacedHeap");
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hkQueryVideoMemoryInfo(IDXGIAdapter3* This, UINT Node, DXGI_MEMORY_SEGMENT_GROUP Group, DXGI_QUERY_VIDEO_MEMORY_INFO* pInfo) {
    HRESULT hr = oQueryVideoMemoryInfo(This, Node, Group, pInfo);
    if (SUCCEEDED(hr) && pInfo && Group == DXGI_MEMORY_SEGMENT_GROUP_LOCAL && Config::VRAM_BUDGET_CAP_BYTES > 0 && pInfo->Budget > Config::VRAM_BUDGET_CAP_BYTES) {
        UINT64 orig = pInfo->Budget;
        pInfo->Budget = Config::VRAM_BUDGET_CAP_BYTES;
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) {
            std::ostringstream oss;
            oss << "VRAM Budget Spoofed v2.1: " << (orig / (1024 * 1024)) << " MB -> 2850 MB";
            Logger::Get().Info(oss.str());
        }
    }
    return hr;
}

void ApplySwapChainLatency(IDXGISwapChain* sc) {
    if (!sc) return;
    { std::lock_guard<std::mutex> l(g_hookMtx); if (!g_latSet.insert(sc).second) return; }
    IDXGISwapChain2* sc2 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc2))) && sc2) {
        if (SUCCEEDED(sc2->SetMaximumFrameLatency(1)))
            Logger::Get().Info("IDXGISwapChain2::SetMaximumFrameLatency(1) set.");
        sc2->Release();
    }
}

HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* This, UINT Sync, UINT Flags) {
    ApplySwapChainLatency(This);
    HRESULT hr = oPresent(This, Sync, Flags);
    HighPrecisionFramePacer::Get().PaceFrame(Flags);
    return hr;
}

HRESULT STDMETHODCALLTYPE hkPresent1(IDXGISwapChain1* This, UINT Sync, UINT Flags, const DXGI_PRESENT_PARAMETERS* pParams) {
    ApplySwapChainLatency(This);
    HRESULT hr = oPresent1(This, Sync, Flags, pParams);
    HighPrecisionFramePacer::Get().PaceFrame(Flags);
    return hr;
}

void HookD3D12Device(ID3D12Device* dev) {
    if (!dev) return;
    std::lock_guard<std::mutex> l(g_hookMtx);
    HookVTableSlot(dev, 27, &hkCreateCommittedResource, &oCreateCommittedResource, "ID3D12Device::CreateCommittedResource");
    HookVTableSlot(dev, 29, &hkCreatePlacedResource, &oCreatePlacedResource, "ID3D12Device::CreatePlacedResource");
}

void HookAdapter(IUnknown* unk) {
    if (!unk) return;
    IDXGIAdapter3* ad3 = nullptr;
    if (SUCCEEDED(unk->QueryInterface(IID_PPV_ARGS(&ad3))) && ad3) {
        { std::lock_guard<std::mutex> l(g_hookMtx); HookVTableSlot(ad3, 14, &hkQueryVideoMemoryInfo, &oQueryVideoMemoryInfo, "IDXGIAdapter3::QueryVideoMemoryInfo"); }
        static std::atomic<bool> preHooked{false};
        DXGI_ADAPTER_DESC desc = {};
        if (!preHooked.load() && SUCCEEDED(ad3->GetDesc(&desc)) && desc.VendorId == 0x10DE) {
            HMODULE h12 = GetModuleHandleW(L"d3d12.dll");
            if (!h12) h12 = LoadLibraryW(L"d3d12.dll");
            if (h12) {
                auto fn = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(h12, "D3D12CreateDevice"));
                ID3D12Device* tmp = nullptr;
                if (fn && SUCCEEDED(fn(ad3, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&tmp))) && tmp) {
                    preHooked.store(true);
                    HookD3D12Device(tmp);
                    tmp->Release();
                }
            }
        }
        ad3->Release();
    }
}

void HookSwapChain(IDXGISwapChain* sc, IUnknown* devUnk) {
    if (!sc) return;
    if (devUnk) {
        ID3D12CommandQueue* q = nullptr;
        if (SUCCEEDED(devUnk->QueryInterface(IID_PPV_ARGS(&q))) && q) {
            ID3D12Device* d = nullptr;
            if (SUCCEEDED(q->GetDevice(IID_PPV_ARGS(&d))) && d) { HookD3D12Device(d); d->Release(); }
            q->Release();
        }
    }
    {
        std::lock_guard<std::mutex> l(g_hookMtx);
        HookVTableSlot(sc, 8, &hkPresent, &oPresent, "IDXGISwapChain::Present");
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1) {
            HookVTableSlot(sc1, 22, &hkPresent1, &oPresent1, "IDXGISwapChain1::Present1");
            sc1->Release();
        }
    }
    ApplySwapChainLatency(sc);
}

HRESULT STDMETHODCALLTYPE hkEnumAdapters(IDXGIFactory* This, UINT i, IDXGIAdapter** pp) {
    HRESULT hr = oEnumAdapters(This, i, pp); if (SUCCEEDED(hr) && pp && *pp) HookAdapter(*pp); return hr;
}
HRESULT STDMETHODCALLTYPE hkEnumAdapters1(IDXGIFactory1* This, UINT i, IDXGIAdapter1** pp) {
    HRESULT hr = oEnumAdapters1(This, i, pp); if (SUCCEEDED(hr) && pp && *pp) HookAdapter(*pp); return hr;
}
HRESULT STDMETHODCALLTYPE hkEnumAdapterByLuid(IDXGIFactory4* This, LUID luid, REFIID riid, void** ppv) {
    HRESULT hr = oEnumAdapterByLuid(This, luid, riid, ppv); if (SUCCEEDED(hr) && ppv && *ppv) HookAdapter(static_cast<IUnknown*>(*ppv)); return hr;
}
HRESULT STDMETHODCALLTYPE hkEnumAdapterByGpuPreference(IDXGIFactory6* This, UINT i, DXGI_GPU_PREFERENCE pref, REFIID riid, void** ppv) {
    HRESULT hr = oEnumAdapterByGpuPreference(This, i, pref, riid, ppv); if (SUCCEEDED(hr) && ppv && *ppv) HookAdapter(static_cast<IUnknown*>(*ppv)); return hr;
}
HRESULT STDMETHODCALLTYPE hkCreateSwapChain(IDXGIFactory* This, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** pp) {
    HRESULT hr = oCreateSwapChain(This, dev, desc, pp); if (SUCCEEDED(hr) && pp && *pp) HookSwapChain(*pp, dev); return hr;
}
HRESULT STDMETHODCALLTYPE hkCreateSwapChainForHwnd(IDXGIFactory2* This, IUnknown* dev, HWND h, const DXGI_SWAP_CHAIN_DESC1* d, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fd, IDXGIOutput* out, IDXGISwapChain1** pp) {
    HRESULT hr = oCreateSwapChainForHwnd(This, dev, h, d, fd, out, pp); if (SUCCEEDED(hr) && pp && *pp) HookSwapChain(*pp, dev); return hr;
}

void HookFactory(IUnknown* unk) {
    if (!unk) return;
    std::lock_guard<std::mutex> l(g_hookMtx);
    IDXGIFactory* f0 = nullptr; if (SUCCEEDED(unk->QueryInterface(IID_PPV_ARGS(&f0))) && f0) { HookVTableSlot(f0, 7, &hkEnumAdapters, &oEnumAdapters, "EnumAdapters"); HookVTableSlot(f0, 10, &hkCreateSwapChain, &oCreateSwapChain, "CreateSwapChain"); f0->Release(); }
    IDXGIFactory1* f1 = nullptr; if (SUCCEEDED(unk->QueryInterface(IID_PPV_ARGS(&f1))) && f1) { HookVTableSlot(f1, 12, &hkEnumAdapters1, &oEnumAdapters1, "EnumAdapters1"); f1->Release(); }
    IDXGIFactory2* f2 = nullptr; if (SUCCEEDED(unk->QueryInterface(IID_PPV_ARGS(&f2))) && f2) { HookVTableSlot(f2, 15, &hkCreateSwapChainForHwnd, &oCreateSwapChainForHwnd, "CreateSwapChainForHwnd"); f2->Release(); }
    IDXGIFactory4* f4 = nullptr; if (SUCCEEDED(unk->QueryInterface(IID_PPV_ARGS(&f4))) && f4) { HookVTableSlot(f4, 26, &hkEnumAdapterByLuid, &oEnumAdapterByLuid, "EnumAdapterByLuid"); f4->Release(); }
    IDXGIFactory6* f6 = nullptr; if (SUCCEEDED(unk->QueryInterface(IID_PPV_ARGS(&f6))) && f6) { HookVTableSlot(f6, 29, &hkEnumAdapterByGpuPreference, &oEnumAdapterByGpuPreference, "EnumAdapterByGpuPreference"); f6->Release(); }
}

namespace SysDXGI {
    HMODULE hMod = nullptr;
    std::once_flag flag;
    FARPROC pCreate0 = nullptr, pCreate1 = nullptr, pCreate2 = nullptr, pDecl = nullptr, pDebug = nullptr;
    FARPROC pQuirk = nullptr, pStr = nullptr, pVal = nullptr, pDump = nullptr, pRep = nullptr;

    void Init() {
        std::call_once(flag, []() {
            Logger::Get().Init();
            HANDLE hProc = GetCurrentProcess();
            SetPriorityClass(hProc, ABOVE_NORMAL_PRIORITY_CLASS);
            PROCESS_POWER_THROTTLING_STATE thr = {};
            thr.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
            thr.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
            thr.StateMask = 0;
            SetProcessInformation(hProc, ProcessPowerThrottling, &thr, sizeof(thr));
            Config::LoadIni();
            std::ostringstream settings;
            settings << "Config loaded: VramBudgetMB=" << (Config::VRAM_BUDGET_CAP_BYTES / (1024 * 1024))
                     << ", TargetFPS=" << Config::TARGET_FPS
                     << ", LargeTextureDim=" << Config::LARGE_TEXTURE_DIM
                     << ", BoostCpuPriority=" << (Config::BOOST_CPU_PRIORITY ? 1 : 0)
                     << ", AutoPatchUE5Ini=" << (Config::AUTO_PATCH_UE5_INI ? 1 : 0);
            Logger::Get().Info(settings.str());
            if (Config::BOOST_CPU_PRIORITY) {
                HANDLE hProc = GetCurrentProcess();
                SetPriorityClass(hProc, ABOVE_NORMAL_PRIORITY_CLASS);
                PROCESS_POWER_THROTTLING_STATE thr = {};
                thr.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
                thr.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
                thr.StateMask = 0;
                SetProcessInformation(hProc, ProcessPowerThrottling, &thr, sizeof(thr));
                Logger::Get().Info("Process priority set to ABOVE_NORMAL_PRIORITY_CLASS (v2.2).");
            }
            UE5ConfigOptimizer::Run();
            if (Config::TARGET_FPS > 0.0)
                HighPrecisionFramePacer::Get().Initialize();
            wchar_t sys[MAX_PATH] = {}; GetSystemDirectoryW(sys, MAX_PATH);
            hMod = LoadLibraryW((std::wstring(sys) + L"\\dxgi.dll").c_str());
            if (!hMod) return;
            pCreate0 = GetProcAddress(hMod, "CreateDXGIFactory");
            pCreate1 = GetProcAddress(hMod, "CreateDXGIFactory1");
            pCreate2 = GetProcAddress(hMod, "CreateDXGIFactory2");
            pDecl    = GetProcAddress(hMod, "DXGIDeclareAdapterRemovalSupport");
            pDebug   = GetProcAddress(hMod, "DXGIGetDebugInterface1");
            pQuirk   = GetProcAddress(hMod, "ApplyCompatResolutionQuirking");
            pStr     = GetProcAddress(hMod, "CompatString");
            pVal     = GetProcAddress(hMod, "CompatValue");
            pDump    = GetProcAddress(hMod, "DXGIDumpJournal");
            pRep     = GetProcAddress(hMod, "DXGIReportAdapterConfiguration");
            Logger::Get().Info("Loaded system32\\dxgi.dll.");
        });
    }
}

extern "C" {
    HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** pp) {
        SysDXGI::Init(); if (!SysDXGI::pCreate0) return E_FAIL;
        HRESULT hr = reinterpret_cast<HRESULT(WINAPI*)(REFIID, void**)>(SysDXGI::pCreate0)(riid, pp);
        if (SUCCEEDED(hr) && pp && *pp) HookFactory(static_cast<IUnknown*>(*pp));
        return hr;
    }
    HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** pp) {
        SysDXGI::Init(); if (!SysDXGI::pCreate1) return E_FAIL;
        HRESULT hr = reinterpret_cast<HRESULT(WINAPI*)(REFIID, void**)>(SysDXGI::pCreate1)(riid, pp);
        if (SUCCEEDED(hr) && pp && *pp) HookFactory(static_cast<IUnknown*>(*pp));
        return hr;
    }
    HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** pp) {
        SysDXGI::Init(); if (!SysDXGI::pCreate2) return E_FAIL;
        HRESULT hr = reinterpret_cast<HRESULT(WINAPI*)(UINT, REFIID, void**)>(SysDXGI::pCreate2)(flags, riid, pp);
        if (SUCCEEDED(hr) && pp && *pp) HookFactory(static_cast<IUnknown*>(*pp));
        return hr;
    }
    HRESULT WINAPI DXGIDeclareAdapterRemovalSupport() {
        SysDXGI::Init(); return SysDXGI::pDecl ? reinterpret_cast<HRESULT(WINAPI*)()>(SysDXGI::pDecl)() : S_OK;
    }
    HRESULT WINAPI DXGIGetDebugInterface1(UINT f, REFIID r, void** p) {
        SysDXGI::Init(); return SysDXGI::pDebug ? reinterpret_cast<HRESULT(WINAPI*)(UINT, REFIID, void**)>(SysDXGI::pDebug)(f, r, p) : E_NOINTERFACE;
    }
    HRESULT WINAPI ApplyCompatResolutionQuirking(void* a, void* b) {
        SysDXGI::Init(); return SysDXGI::pQuirk ? reinterpret_cast<HRESULT(WINAPI*)(void*, void*)>(SysDXGI::pQuirk)(a, b) : S_OK;
    }
    HRESULT WINAPI CompatString(void* a, void* b, void* c, void* d) {
        SysDXGI::Init(); return SysDXGI::pStr ? reinterpret_cast<HRESULT(WINAPI*)(void*, void*, void*, void*)>(SysDXGI::pStr)(a, b, c, d) : S_OK;
    }
    HRESULT WINAPI CompatValue(void* a, void* b) {
        SysDXGI::Init(); return SysDXGI::pVal ? reinterpret_cast<HRESULT(WINAPI*)(void*, void*)>(SysDXGI::pVal)(a, b) : S_OK;
    }
    HRESULT WINAPI DXGIDumpJournal(void* a) {
        SysDXGI::Init(); return SysDXGI::pDump ? reinterpret_cast<HRESULT(WINAPI*)(void*)>(SysDXGI::pDump)(a) : S_OK;
    }
    HRESULT WINAPI DXGIReportAdapterConfiguration(void* a) {
        SysDXGI::Init(); return SysDXGI::pRep ? reinterpret_cast<HRESULT(WINAPI*)(void*)>(SysDXGI::pRep)(a) : S_OK;
    }
}

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(hMod);
    return TRUE;
}
