#include <OverlayApp.hpp>
#include <iostream>
#include <cstring>
#include <cstdio>
#include <TiltedCore/Filesystem.hpp>

namespace TiltedPhoques
{
    OverlayApp::OverlayApp(RenderProvider *apRenderProvider, OverlayClient *apCustomClient, std::wstring aProcessName) noexcept
        : m_pBrowserProcessHandler(new OverlayBrowserProcessHandler)
        , m_pRenderProvider(apRenderProvider)
        , m_processName(std::move(aProcessName))
    {
        if (apCustomClient)
            m_pClient = apCustomClient;

    }

    // Diretório onde ficam os recursos do CEF (libcef.dll, *.pak, locales/,
    // icudtl.dat). No modo in-process TiltedPhoques::GetPath() (dir do executável)
    // coincidia com a pasta do STR. No modo externo (Proton) o processo é o
    // SkyrimSE.exe, então GetPath() aponta para a raiz do jogo e o CEF não acha os
    // .pak/locales — CefInitialize dá int3 (CHECK do Chromium). Aqui resolvemos o
    // dir a partir da própria DLL do overlay, que fica junto dos recursos.
    static std::filesystem::path GetOverlayResourceDir()
    {
        HMODULE hSelf = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&GetOverlayResourceDir), &hSelf) && hSelf)
        {
            wchar_t modulePath[MAX_PATH]{};
            if (GetModuleFileNameW(hSelf, modulePath, MAX_PATH))
                return std::filesystem::path(modulePath).parent_path();
        }

        // Fallback: comportamento antigo (dir do executável).
        return TiltedPhoques::GetPath();
    }

    // Diagnóstico temporário (Proton): marca com fsync no dir de recursos correto.
    static void CefDiag(const char* apMsg)
    {
        const auto p = (GetOverlayResourceDir() / "logs" / "st_cef_diag.log").wstring();
        HANDLE h = CreateFileW(p.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            char line[128];
            const int n = _snprintf(line, sizeof(line), "%s\r\n", apMsg);
            DWORD w = 0;
            WriteFile(h, line, n > 0 ? (DWORD)n : 0, &w, nullptr);
            FlushFileBuffers(h);
            CloseHandle(h);
        }
    }

    bool OverlayApp::Initialize() noexcept
    {
        CefMainArgs args(GetModuleHandleW(nullptr));

        const auto currentPath = GetOverlayResourceDir();

        CefSettings settings;
        settings.no_sandbox = true;
        settings.multi_threaded_message_loop = true;
        settings.windowless_rendering_enabled = true;

        settings.log_severity = LOGSEVERITY_WARNING;
        // settings.remote_debugging_port = 8384;

        // Only the first instance will use real CEF cache, extra instances' caches go in
        // %TEMP% (see `root_cache_path` docs)
        const std::wstring cachePath = GetCefCachePath(currentPath);

        CefString(&settings.log_file).FromWString(currentPath / L"logs" / L"cef_debug.log");
        CefString(&settings.root_cache_path).FromWString(cachePath);
        CefString(&settings.cache_path).FromWString(cachePath);
        CefString(&settings.framework_dir_path).FromWString(currentPath);
        CefString(&settings.resources_dir_path).FromWString(currentPath);
        CefString(&settings.locales_dir_path).FromWString(currentPath / L"locales");
        CefString(&settings.browser_subprocess_path).FromWString(currentPath / m_processName);

        CefDiag("[cef] paths set, calling CefInitialize");
        if (!CefInitialize(args, settings, this, nullptr))
        {
            CefDiag("[cef] CefInitialize returned FALSE");
            return false;
        }
        CefDiag("[cef] CefInitialize OK");

        if (!m_pClient)
            m_pClient = new OverlayClient(m_pRenderProvider->Create());

        CefBrowserSettings browserSettings{};
        browserSettings.windowless_frame_rate = 60;

        CefWindowInfo info;
        info.SetAsWindowless(m_pRenderProvider->GetWindow());

        CefDiag("[cef] before CreateBrowser");
        const auto ret = CefBrowserHost::CreateBrowser(info, m_pClient.get(),
            (currentPath / L"UI" / L"index.html").wstring(), browserSettings, nullptr, nullptr);
        CefDiag("[cef] CreateBrowser returned");

        return ret;
    }

    void OverlayApp::Shutdown() noexcept
    {
        CefShutdown();
    }

    void OverlayApp::ExecuteAsync(const std::string& acFunction, const CefRefPtr<CefListValue>& apArguments) const noexcept
    {
        if (!m_pClient)
            return;

        auto pMessage = CefProcessMessage::Create("browser-event");
        auto pArguments = pMessage->GetArgumentList();

        const auto pFunctionArguments = apArguments ? apArguments : CefListValue::Create();

        pArguments->SetString(0, acFunction);
        pArguments->SetList(1, pFunctionArguments);

        auto pBrowser = m_pClient->GetBrowser();
        if (pBrowser)
        {
            pBrowser->GetMainFrame()->SendProcessMessage(PID_RENDERER, pMessage);
        }
    }

    void OverlayApp::InjectKey(const cef_key_event_type_t aType, const uint32_t aModifiers, const uint16_t aKey, const uint16_t aScanCode) const noexcept
    {
        if (m_pClient)
        {
            CefKeyEvent ev;

            ev.type = aType;
            ev.modifiers = aModifiers;
            ev.windows_key_code = aKey;
            ev.native_key_code = aScanCode;

            if (m_pClient->GetBrowser() && m_pClient->GetBrowser()->GetHost())
                m_pClient->GetBrowser()->GetHost()->SendKeyEvent(ev);
        }
    }

    void OverlayApp::InjectMouseButton(const uint16_t aX, const uint16_t aY, const cef_mouse_button_type_t aButton, const bool aUp, const uint32_t aModifier) const noexcept
    {
        if (m_pClient)
        {
            CefMouseEvent ev;

            ev.x = aX;
            ev.y = aY;
            ev.modifiers = aModifier;

            if (m_pClient->GetBrowser() && m_pClient->GetBrowser()->GetHost())
                m_pClient->GetBrowser()->GetHost()->SendMouseClickEvent(ev, aButton, aUp, 1);
        }
    }

    void OverlayApp::InjectMouseMove(const uint16_t aX, const uint16_t aY, const uint32_t aModifier) const noexcept
    {
        if (m_pClient && m_pClient->GetOverlayRenderHandler())
        {
            CefMouseEvent ev;

            ev.x = aX;
            ev.y = aY;
            ev.modifiers = aModifier;

            m_pClient->GetOverlayRenderHandler()->SetCursorLocation(aX, aY);

            if(m_pClient->GetBrowser() && m_pClient->GetBrowser()->GetHost())
                m_pClient->GetBrowser()->GetHost()->SendMouseMoveEvent(ev, false);
        }
    }

    void OverlayApp::InjectMouseWheel(const uint16_t aX, const uint16_t aY, const int16_t aDelta, const uint32_t aModifier) const noexcept
    {
        if (m_pClient && m_pClient->GetOverlayRenderHandler())
        {
            CefMouseEvent ev;

            ev.x = aX;
            ev.y = aY;
            ev.modifiers = aModifier;

            if (m_pClient->GetBrowser() && m_pClient->GetBrowser()->GetHost())
                m_pClient->GetBrowser()->GetHost()->SendMouseWheelEvent(ev, 0, aDelta);
        }
    }

    void OverlayApp::OnBeforeCommandLineProcessing(const CefString& aProcessType, CefRefPtr<CefCommandLine> aCommandLine)
    {
        aCommandLine->AppendSwitch("allow-file-access-from-files");
        aCommandLine->AppendSwitch("allow-universal-access-from-files");

        // Linux/Proton fix: the overlay renders through the CPU OnPaint path
        // (see OverlayRenderHandlerD3D11), so the Chromium GPU process is not
        // needed. Under Wine/Proton, initializing the GPU pipeline (ANGLE -> D3D)
        // crashes libcef.dll inside D3DCompiler_47 while compiling shaders.
        // Disabling GPU/compositing routes CEF straight to OnPaint from the
        // compositor and avoids the crash. On Windows this is a no-op for OSR,
        // which already copies frames on the CPU.
        aCommandLine->AppendSwitch("disable-gpu");
        aCommandLine->AppendSwitch("disable-gpu-compositing");
    }

    std::wstring OverlayApp::GetCefCachePath(const std::filesystem::path& currentPath) const noexcept
    {
        static HANDLE g_tiltedUiMutex = CreateMutexW(0, FALSE, L"TiltedUiAppMutex");
        if (g_tiltedUiMutex && GetLastError() != ERROR_ALREADY_EXISTS)
        {
            return currentPath / L"cache";
        }

        // 2nd instance's cache will go in "%TEMP%/tiltedui_cef_cache/1", 3rd in "%TEMP%/tiltedui_cef_cache/2", ...
        uint8_t slot = 1;
        std::filesystem::path tempCacheDir = std::filesystem::temp_directory_path() / L"tiltedui_cef_cache" / std::to_wstring(slot);
        while (std::filesystem::exists(tempCacheDir / L"lockfile"))
        {
            slot++;
            tempCacheDir = tempCacheDir.parent_path() / std::to_wstring(slot);
        }
        return tempCacheDir;
    }
}
