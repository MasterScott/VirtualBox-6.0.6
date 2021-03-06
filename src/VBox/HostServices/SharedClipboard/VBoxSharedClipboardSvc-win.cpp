/* $Id$ */
/** @file
 * Shared Clipboard Service - Win32 host.
 */

/*
 * Copyright (C) 2006-2019 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#define LOG_GROUP LOG_GROUP_SHARED_CLIPBOARD
#include <iprt/win/windows.h>

#include <VBox/HostServices/VBoxClipboardSvc.h>
#include <VBox/GuestHost/SharedClipboard-win.h>
#ifdef VBOX_WITH_SHARED_CLIPBOARD_URI_LIST
# include <VBox/GuestHost/SharedClipboard-uri.h>
#endif

#include <iprt/alloc.h>
#include <iprt/string.h>
#include <iprt/asm.h>
#include <iprt/assert.h>
#include <iprt/ldr.h>
#include <iprt/semaphore.h>
#include <iprt/thread.h>
#ifdef VBOX_WITH_SHARED_CLIPBOARD_URI_LIST
# include <iprt/utf16.h>
#endif

#include <process.h>
#include <shlobj.h> /* Needed for shell objects. */

#include "VBoxClipboard.h"

/** Static window class name. */
static char s_szClipWndClassName[] = VBOX_CLIPBOARD_WNDCLASS_NAME;


/*********************************************************************************************************************************
*   Internal Functions                                                                                                           *
*********************************************************************************************************************************/
static int ConvertCFHtmlToMime(const char *pszSource, const uint32_t cch, char **ppszOutput, uint32_t *pch);
static int ConvertMimeToCFHTML(const char *pszSource, size_t cb, char **ppszOutput, uint32_t *pcbOutput);
static bool IsWindowsHTML(const char *source);
static int vboxClipboardSyncInternal(PVBOXCLIPBOARDCONTEXT pCtx);

struct _VBOXCLIPBOARDCONTEXT
{
    /** Handle for window message handling thread. */
    RTTHREAD                 hThread;
    /** Event which gets triggered if the host clipboard needs to render its data. */
    RTSEMEVENT               hRenderEvent;
    /** Structure for keeping and communicating with client data (from the guest). */
    PVBOXCLIPBOARDCLIENTDATA pClient;
    /** Windows-specific context data. */
    VBOXCLIPBOARDWINCTX      Win;
};

/* Only one client is supported. There seems to be no need for more clients. */
static VBOXCLIPBOARDCONTEXT g_ctx;


#ifdef LOG_ENABLED
static void vboxClipboardDump(const void *pv, size_t cb, uint32_t u32Format)
{
    if (u32Format & VBOX_SHARED_CLIPBOARD_FMT_UNICODETEXT)
    {
        LogFunc(("VBOX_SHARED_CLIPBOARD_FMT_UNICODETEXT:\n"));
        if (pv && cb)
            LogFunc(("%ls\n", pv));
        else
            LogFunc(("%p %zu\n", pv, cb));
    }
    else if (u32Format & VBOX_SHARED_CLIPBOARD_FMT_BITMAP)
        LogFunc(("VBOX_SHARED_CLIPBOARD_FMT_BITMAP\n"));
    else if (u32Format & VBOX_SHARED_CLIPBOARD_FMT_HTML)
    {
        LogFunc(("VBOX_SHARED_CLIPBOARD_FMT_HTML:\n"));
        if (pv && cb)
        {
            LogFunc(("%s\n", pv));

            //size_t cb = RTStrNLen(pv, );
            char *pszBuf = (char *)RTMemAllocZ(cb + 1);
            RTStrCopy(pszBuf, cb + 1, (const char *)pv);
            for (size_t off = 0; off < cb; ++off)
            {
                if (pszBuf[off] == '\n' || pszBuf[off] == '\r')
                    pszBuf[off] = ' ';
            }

            LogFunc(("%s\n", pszBuf));
            RTMemFree(pszBuf);
        }
        else
            LogFunc(("%p %zu\n", pv, cb));
    }
    else
        LogFunc(("Invalid format %02X\n", u32Format));
}
#else  /* !LOG_ENABLED */
#   define vboxClipboardDump(__pv, __cb, __format) do { NOREF(__pv); NOREF(__cb); NOREF(__format); } while (0)
#endif /* !LOG_ENABLED */

/** @todo Someone please explain the protocol wrt overflows...  */
static void vboxClipboardGetData(uint32_t u32Format, const void *pvSrc, uint32_t cbSrc,
                                 void *pvDst, uint32_t cbDst, uint32_t *pcbActualDst)
{
    LogFlowFunc(("cbSrc = %d, cbDst = %d\n", cbSrc, cbDst));

    if (   u32Format == VBOX_SHARED_CLIPBOARD_FMT_HTML
        && IsWindowsHTML((const char *)pvSrc))
    {
        /** @todo r=bird: Why the double conversion? */
        char *pszBuf = NULL;
        uint32_t cbBuf = 0;
        int rc = ConvertCFHtmlToMime((const char *)pvSrc, cbSrc, &pszBuf, &cbBuf);
        if (RT_SUCCESS(rc))
        {
            *pcbActualDst = cbBuf;
            if (cbBuf > cbDst)
            {
                /* Do not copy data. The dst buffer is not enough. */
                RTMemFree(pszBuf);
                return;
            }
            memcpy(pvDst, pszBuf, cbBuf);
            RTMemFree(pszBuf);
        }
        else
            *pcbActualDst = 0;
    }
    else
    {
        *pcbActualDst = cbSrc;

        if (cbSrc > cbDst)
        {
            /* Do not copy data. The dst buffer is not enough. */
            return;
        }

        memcpy(pvDst, pvSrc, cbSrc);
    }

    vboxClipboardDump(pvDst, cbSrc, u32Format);

    return;
}

static int vboxClipboardReadDataFromClient(VBOXCLIPBOARDCONTEXT *pCtx, VBOXCLIPBOARDFORMAT fFormat)
{
    Assert(pCtx->pClient);
    Assert(pCtx->hRenderEvent);
    Assert(pCtx->pClient->data.pv == NULL && pCtx->pClient->data.cb == 0 && pCtx->pClient->data.u32Format == 0);

    LogFlowFunc(("fFormat=%02X\n", fFormat));

    vboxSvcClipboardReportMsg(pCtx->pClient, VBOX_SHARED_CLIPBOARD_HOST_MSG_READ_DATA, fFormat);

    return RTSemEventWait(pCtx->hRenderEvent, 30 * 1000 /* Timeout in ms */);
}

static LRESULT CALLBACK vboxClipboardWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    LRESULT rc = 0;

    const PVBOXCLIPBOARDCONTEXT pCtx    = &g_ctx;
    const PVBOXCLIPBOARDWINCTX  pWinCtx = &pCtx->Win;

    switch (msg)
    {
        case WM_CLIPBOARDUPDATE:
        {
            LogFunc(("WM_CLIPBOARDUPDATE\n"));

            if (GetClipboardOwner() != hwnd)
            {
                /* Clipboard was updated by another application, retrieve formats and report back. */
                int vboxrc = vboxClipboardSyncInternal(pCtx);
                AssertRC(vboxrc);
            }
        } break;

        case WM_CHANGECBCHAIN:
        {
            LogFunc(("WM_CHANGECBCHAIN\n"));

            if (VBoxClipboardWinIsNewAPI(&pWinCtx->newAPI))
            {
                rc = DefWindowProc(hwnd, msg, wParam, lParam);
                break;
            }

            HWND hwndRemoved = (HWND)wParam;
            HWND hwndNext    = (HWND)lParam;

            if (hwndRemoved == pWinCtx->hWndNextInChain)
            {
                /* The window that was next to our in the chain is being removed.
                 * Relink to the new next window.
                 */
                pWinCtx->hWndNextInChain = hwndNext;
            }
            else
            {
                if (pWinCtx->hWndNextInChain)
                {
                    /* Pass the message further. */
                    DWORD_PTR dwResult;
                    rc = SendMessageTimeout(pWinCtx->hWndNextInChain, WM_CHANGECBCHAIN, wParam, lParam, 0,
                                            VBOX_CLIPBOARD_CBCHAIN_TIMEOUT_MS,
                                            &dwResult);
                    if (!rc)
                        rc = (LRESULT)dwResult;
                }
            }
        } break;

        case WM_DRAWCLIPBOARD:
        {
            LogFunc(("WM_DRAWCLIPBOARD\n"));

            if (GetClipboardOwner() != hwnd)
            {
                /* Clipboard was updated by another application, retrieve formats and report back. */
                int vboxrc = vboxClipboardSyncInternal(pCtx);
                AssertRC(vboxrc);
            }

            if (pWinCtx->hWndNextInChain)
            {
                LogFunc(("WM_DRAWCLIPBOARD next %p\n", pWinCtx->hWndNextInChain));
                /* Pass the message to next windows in the clipboard chain. */
                DWORD_PTR dwResult;
                rc = SendMessageTimeout(pWinCtx->hWndNextInChain, msg, wParam, lParam, 0, VBOX_CLIPBOARD_CBCHAIN_TIMEOUT_MS,
                                        &dwResult);
                if (!rc)
                    rc = dwResult;
            }
        } break;

        case WM_TIMER:
        {
            if (VBoxClipboardWinIsNewAPI(&pWinCtx->newAPI))
                break;

            HWND hViewer = GetClipboardViewer();

            /* Re-register ourselves in the clipboard chain if our last ping
             * timed out or there seems to be no valid chain. */
            if (!hViewer || pWinCtx->oldAPI.fCBChainPingInProcess)
            {
                VBoxClipboardWinRemoveFromCBChain(&pCtx->Win);
                VBoxClipboardWinAddToCBChain(&pCtx->Win);
            }

            /* Start a new ping by passing a dummy WM_CHANGECBCHAIN to be
             * processed by ourselves to the chain. */
            pWinCtx->oldAPI.fCBChainPingInProcess = TRUE;

            hViewer = GetClipboardViewer();
            if (hViewer)
                SendMessageCallback(hViewer, WM_CHANGECBCHAIN,
                                    (WPARAM)pWinCtx->hWndNextInChain, (LPARAM)pWinCtx->hWndNextInChain,
                                    VBoxClipboardWinChainPingProc, (ULONG_PTR)pWinCtx);
        } break;

        case WM_RENDERFORMAT:
        {
            /* Insert the requested clipboard format data into the clipboard. */
            VBOXCLIPBOARDFORMAT fFormat = VBOX_SHARED_CLIPBOARD_FMT_NONE;

            UINT format = (UINT)wParam;

            LogFunc(("WM_RENDERFORMAT: Format %u\n", format));

            switch (format)
            {
                case CF_UNICODETEXT:
                    fFormat |= VBOX_SHARED_CLIPBOARD_FMT_UNICODETEXT;
                    break;

                case CF_DIB:
                    fFormat |= VBOX_SHARED_CLIPBOARD_FMT_BITMAP;
                    break;

                default:
                    if (format >= 0xC000)
                    {
                        TCHAR szFormatName[256]; /** @todo r=andy Unicode, 256 is enough? */
                        int cActual = GetClipboardFormatName(format, szFormatName, sizeof(szFormatName) / sizeof (TCHAR));
                        if (cActual)
                        {
                            if (RTStrCmp(szFormatName, "HTML Format") == 0)
                                fFormat |= VBOX_SHARED_CLIPBOARD_FMT_HTML;
#ifdef VBOX_WITH_SHARED_CLIPBOARD_URI_LIST
                            if (   RTStrCmp(szFormatName, CFSTR_FILEDESCRIPTOR) == 0
                                || RTStrCmp(szFormatName, CFSTR_FILECONTENTS) == 0)
                                fFormat |= VBOX_SHARED_CLIPBOARD_FMT_URI_LIST;
#endif
                        }
                    }
                    break;
            }

            if (   fFormat       == VBOX_SHARED_CLIPBOARD_FMT_NONE
                || pCtx->pClient == NULL)
            {
                /* Unsupported clipboard format is requested. */
                LogFunc(("WM_RENDERFORMAT unsupported format requested or client is not active\n"));
                EmptyClipboard();
            }
            else
            {
                int vboxrc = vboxClipboardReadDataFromClient(pCtx, fFormat);

                LogFunc(("vboxClipboardReadDataFromClient vboxrc = %d, pv %p, cb %d, u32Format %d\n",
                          vboxrc, pCtx->pClient->data.pv, pCtx->pClient->data.cb, pCtx->pClient->data.u32Format));

                if (   RT_SUCCESS (vboxrc)
                    && pCtx->pClient->data.pv != NULL
                    && pCtx->pClient->data.cb > 0
                    && pCtx->pClient->data.u32Format == fFormat)
                {
                    HANDLE hMem = GlobalAlloc(GMEM_DDESHARE | GMEM_MOVEABLE, pCtx->pClient->data.cb);

                    LogFunc(("hMem %p\n", hMem));

                    if (hMem)
                    {
                        void *pMem = GlobalLock(hMem);

                        LogFunc(("pMem %p, GlobalSize %d\n", pMem, GlobalSize(hMem)));

                        if (pMem)
                        {
                            LogFunc(("WM_RENDERFORMAT setting data\n"));

                            if (pCtx->pClient->data.pv)
                            {
                                memcpy(pMem, pCtx->pClient->data.pv, pCtx->pClient->data.cb);

                                RTMemFree(pCtx->pClient->data.pv);
                                pCtx->pClient->data.pv        = NULL;
                            }

                            pCtx->pClient->data.cb        = 0;
                            pCtx->pClient->data.u32Format = 0;

                            /* The memory must be unlocked before inserting to the Clipboard. */
                            GlobalUnlock(hMem);

                            /* 'hMem' contains the host clipboard data.
                             * size is 'cb' and format is 'format'.
                             */
                            HANDLE hClip = SetClipboardData(format, hMem);

                            LogFunc(("vboxClipboardHostEvent hClip %p\n", hClip));

                            if (hClip)
                            {
                                /* The hMem ownership has gone to the system. Nothing to do. */
                                break;
                            }
                        }

                        GlobalFree(hMem);
                    }
                }

                RTMemFree(pCtx->pClient->data.pv);
                pCtx->pClient->data.pv        = NULL;
                pCtx->pClient->data.cb        = 0;
                pCtx->pClient->data.u32Format = 0;

                /* Something went wrong. */
                VBoxClipboardWinClear();
            }
        } break;

        case WM_RENDERALLFORMATS:
        {
            LogFunc(("WM_RENDERALLFORMATS\n"));

            /* Do nothing. The clipboard formats will be unavailable now, because the
             * windows is to be destroyed and therefore the guest side becomes inactive.
             */
            int vboxrc = VBoxClipboardWinOpen(hwnd);
            if (RT_SUCCESS(vboxrc))
            {
                VBoxClipboardWinClear();
                VBoxClipboardWinClose();
            }
            else
            {
                LogFlowFunc(("WM_RENDERALLFORMATS: error in open clipboard. hwnd: %x, rc: %Rrc\n", hwnd, vboxrc));
            }
        } break;

        case VBOX_CLIPBOARD_WM_SET_FORMATS:
        {
            if (pCtx->pClient == NULL || pCtx->pClient->fMsgFormats)
            {
                /* Host has pending formats message. Ignore the guest announcement,
                 * because host clipboard has more priority.
                 */
                LogFunc(("VBOX_CLIPBOARD_WM_SET_FORMATS ignored\n"));
                break;
            }

            /* Announce available formats. Do not insert data, they will be inserted in WM_RENDER*. */
            uint32_t u32Formats = (uint32_t)lParam;

            LogFunc(("VBOX_CLIPBOARD_WM_SET_FORMATS: u32Formats=%02X\n", u32Formats));

            int vboxrc = VBoxClipboardWinOpen(hwnd);
            if (RT_SUCCESS(vboxrc))
            {
                VBoxClipboardWinClear();

                LogFunc(("VBOX_CLIPBOARD_WM_SET_FORMATS emptied clipboard\n"));

                HANDLE hClip = NULL;

                if (u32Formats & VBOX_SHARED_CLIPBOARD_FMT_UNICODETEXT)
                    hClip = SetClipboardData(CF_UNICODETEXT, NULL);

                if (u32Formats & VBOX_SHARED_CLIPBOARD_FMT_BITMAP)
                    hClip = SetClipboardData(CF_DIB, NULL);

                if (u32Formats & VBOX_SHARED_CLIPBOARD_FMT_HTML)
                {
                    UINT format = RegisterClipboardFormat ("HTML Format");
                    if (format != 0)
                    {
                        hClip = SetClipboardData (format, NULL);
                    }
                }

#ifdef VBOX_WITH_SHARED_CLIPBOARD_URI_LIST
                if (u32Formats & VBOX_SHARED_CLIPBOARD_FMT_URI_LIST)
                {
                    UINT format = RegisterClipboardFormat(CFSTR_FILEDESCRIPTOR);
                    if (format)
                        hClip = SetClipboardData(format, NULL);
                }
#endif
                VBoxClipboardWinClose();

                LogFunc(("VBOX_CLIPBOARD_WM_SET_FORMATS: hClip=%p, lastErr=%ld\n", hClip, GetLastError ()));
            }
        } break;

        case WM_DESTROY:
        {
            /* MS recommends to remove from Clipboard chain in this callback. */
            VBoxClipboardWinRemoveFromCBChain(&pCtx->Win);
            if (pWinCtx->oldAPI.timerRefresh)
            {
                Assert(pWinCtx->hWnd);
                KillTimer(pWinCtx->hWnd, 0);
            }
            PostQuitMessage(0);
        } break;

        default:
        {
            LogFunc(("WM_ %p\n", msg));
            rc = DefWindowProc(hwnd, msg, wParam, lParam);
        }
    }

    LogFunc(("WM_ rc %d\n", rc));
    return rc;
}

DECLCALLBACK(int) VBoxClipboardThread(RTTHREAD hThreadSelf, void *pvUser)
{
    RT_NOREF(hThreadSelf, pvUser);

    /* Create a window and make it a clipboard viewer. */
    int rc = VINF_SUCCESS;

    LogFlowFuncEnter();

    const PVBOXCLIPBOARDCONTEXT pCtx    = &g_ctx;
    const PVBOXCLIPBOARDWINCTX  pWinCtx = &pCtx->Win;

    HINSTANCE hInstance = (HINSTANCE)GetModuleHandle(NULL);

    /* Register the Window Class. */
    WNDCLASS wc;
    RT_ZERO(wc);

    wc.style         = CS_NOCLOSE;
    wc.lpfnWndProc   = vboxClipboardWndProc;
    wc.hInstance     = hInstance;
    wc.hbrBackground = (HBRUSH)(COLOR_BACKGROUND + 1);
    wc.lpszClassName = s_szClipWndClassName;

    ATOM atomWindowClass = RegisterClass(&wc);

    if (atomWindowClass == 0)
    {
        LogFunc(("Failed to register window class\n"));
        rc = VERR_NOT_SUPPORTED;
    }
    else
    {
        /* Create the window. */
        pWinCtx->hWnd = CreateWindowEx(WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_TOPMOST,
                                       s_szClipWndClassName, s_szClipWndClassName,
                                       WS_POPUPWINDOW,
                                       -200, -200, 100, 100, NULL, NULL, hInstance, NULL);
        if (pWinCtx->hWnd == NULL)
        {
            LogFunc(("Failed to create window\n"));
            rc = VERR_NOT_SUPPORTED;
        }
        else
        {
            SetWindowPos(pWinCtx->hWnd, HWND_TOPMOST, -200, -200, 0, 0,
                         SWP_NOACTIVATE | SWP_HIDEWINDOW | SWP_NOCOPYBITS | SWP_NOREDRAW | SWP_NOSIZE);

            VBoxClipboardWinAddToCBChain(&pCtx->Win);
            if (!VBoxClipboardWinIsNewAPI(&pWinCtx->newAPI))
                pWinCtx->oldAPI.timerRefresh = SetTimer(pWinCtx->hWnd, 0, 10 * 1000, NULL);

            MSG msg;
            BOOL msgret = 0;
            while ((msgret = GetMessage(&msg, NULL, 0, 0)) > 0)
            {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            /*
            * Window procedure can return error,
            * but this is exceptional situation
            * that should be identified in testing
            */
            Assert(msgret >= 0);
            LogFunc(("Message loop finished. GetMessage returned %d, message id: %d \n", msgret, msg.message));
        }
    }

    pWinCtx->hWnd = NULL;

    if (atomWindowClass != 0)
    {
        UnregisterClass(s_szClipWndClassName, hInstance);
        atomWindowClass = 0;
    }

    return 0;
}

/**
 * Synchronizes the host and the guest clipboard formats by sending all supported host clipboard
 * formats to the guest.
 *
 * @returns VBox status code, VINF_NO_CHANGE if no synchronization was required.
 * @param   pCtx                Clipboard context to synchronize.
 */
static int vboxClipboardSyncInternal(PVBOXCLIPBOARDCONTEXT pCtx)
{
    AssertPtrReturn(pCtx, VERR_INVALID_POINTER);

    int rc;

    if (pCtx->pClient)
    {
        uint32_t uFormats;
        rc = VBoxClipboardWinGetFormats(&pCtx->Win, &uFormats);
        if (RT_SUCCESS(rc))
            vboxSvcClipboardReportMsg(pCtx->pClient, VBOX_SHARED_CLIPBOARD_HOST_MSG_REPORT_FORMATS, uFormats);
    }
    else /* If we don't have any client data (yet), bail out. */
        rc = VINF_NO_CHANGE;

    LogFlowFuncLeaveRC(rc);
    return rc;
}

/*
 * Public platform dependent functions.
 */
int vboxClipboardInit(void)
{
    RT_ZERO(g_ctx); /* Be careful not messing up non-POD types! */

    /* Check that new Clipboard API is available. */
    VBoxClipboardWinCheckAndInitNewAPI(&g_ctx.Win.newAPI);

    int rc = RTSemEventCreate(&g_ctx.hRenderEvent);
    if (RT_SUCCESS(rc))
    {
        rc = RTThreadCreate(&g_ctx.hThread, VBoxClipboardThread, NULL, _64K /* Stack size */,
                            RTTHREADTYPE_IO, RTTHREADFLAGS_WAITABLE, "SHCLIP");
    }

    if (RT_FAILURE(rc))
        RTSemEventDestroy(g_ctx.hRenderEvent);

    return rc;
}

void vboxClipboardDestroy(void)
{
    LogFlowFuncEnter();

    if (g_ctx.Win.hWnd)
    {
        PostMessage(g_ctx.Win.hWnd, WM_CLOSE, 0, 0);
    }

    int rc = RTSemEventDestroy(g_ctx.hRenderEvent);
    AssertRC(rc);

    /* Wait for the window thread to terminate. */
    rc = RTThreadWait(g_ctx.hThread, 30 * 1000 /* Timeout in ms */, NULL);
    if (RT_FAILURE(rc))
        LogRel(("Shared Clipboard: Waiting for window thread termination failed with rc=%Rrc\n", rc));

    g_ctx.hThread = NIL_RTTHREAD;
}

int vboxClipboardConnect(VBOXCLIPBOARDCLIENTDATA *pClient, bool fHeadless)
{
    RT_NOREF(fHeadless);

    LogFlowFuncEnter();

    if (g_ctx.pClient != NULL)
    {
        /* One client only. */
        return VERR_NOT_SUPPORTED;
    }

    pClient->pCtx = &g_ctx;

    pClient->pCtx->pClient = pClient;

    /* Sync the host clipboard content with the client. */
    vboxClipboardSync(pClient);

    return VINF_SUCCESS;
}

int vboxClipboardSync(VBOXCLIPBOARDCLIENTDATA *pClient)
{
    /* Sync the host clipboard content with the client. */
    return vboxClipboardSyncInternal(pClient->pCtx);
}

void vboxClipboardDisconnect(VBOXCLIPBOARDCLIENTDATA *pClient)
{
    RT_NOREF(pClient);

    LogFlowFuncEnter();

    g_ctx.pClient = NULL;
}

void vboxClipboardFormatAnnounce(VBOXCLIPBOARDCLIENTDATA *pClient, uint32_t u32Formats)
{
    AssertPtrReturnVoid(pClient);
    AssertPtrReturnVoid(pClient->pCtx);

    /*
     * The guest announces formats. Forward to the window thread.
     */
    PostMessage(pClient->pCtx->Win.hWnd, WM_USER, 0, u32Formats);
}

#ifdef VBOX_STRICT
static int vboxClipboardDbgDumpHtml(const char *pszSrc, size_t cb)
{
    size_t cchIgnored = 0;
    int rc = RTStrNLenEx(pszSrc, cb, &cchIgnored);
    if (RT_SUCCESS(rc))
    {
        char *pszBuf = (char *)RTMemAllocZ(cb + 1);
        if (pszBuf != NULL)
        {
            rc = RTStrCopy(pszBuf, cb + 1, (const char *)pszSrc);
            if (RT_SUCCESS(rc))
            {
                for (size_t i = 0; i < cb; ++i)
                    if (pszBuf[i] == '\n' || pszBuf[i] == '\r')
                        pszBuf[i] = ' ';
            }
            else
                LogFunc(("Error in copying string\n"));
            LogFunc(("Removed \\r\\n: %s\n", pszBuf));
            RTMemFree(pszBuf);
        }
        else
        {
            rc = VERR_NO_MEMORY;
            LogFunc(("Not enough memory to allocate buffer\n"));
        }
    }
    return rc;
}
#endif

int vboxClipboardReadData(VBOXCLIPBOARDCLIENTDATA *pClient, uint32_t u32Format, void *pv, uint32_t cb, uint32_t *pcbActual)
{
    AssertPtrReturn(pClient,       VERR_INVALID_POINTER);
    AssertPtrReturn(pClient->pCtx, VERR_INVALID_POINTER);

    LogFlowFunc(("u32Format=%02X\n", u32Format));

    HANDLE hClip = NULL;

    const PVBOXCLIPBOARDWINCTX pWinCtx = &pClient->pCtx->Win;

    /*
     * The guest wants to read data in the given format.
     */
    int rc = VBoxClipboardWinOpen(pWinCtx->hWnd);
    if (RT_SUCCESS(rc))
    {
        LogFunc(("Clipboard opened\n"));

        if (u32Format & VBOX_SHARED_CLIPBOARD_FMT_BITMAP)
        {
            hClip = GetClipboardData(CF_DIB);
            if (hClip != NULL)
            {
                LPVOID lp = GlobalLock(hClip);

                if (lp != NULL)
                {
                    LogFunc(("CF_DIB\n"));

                    vboxClipboardGetData(VBOX_SHARED_CLIPBOARD_FMT_BITMAP, lp, GlobalSize(hClip),
                                         pv, cb, pcbActual);

                    GlobalUnlock(hClip);
                }
                else
                {
                    hClip = NULL;
                }
            }
        }
        else if (u32Format & VBOX_SHARED_CLIPBOARD_FMT_UNICODETEXT)
        {
            hClip = GetClipboardData(CF_UNICODETEXT);
            if (hClip != NULL)
            {
                LPWSTR uniString = (LPWSTR)GlobalLock(hClip);

                if (uniString != NULL)
                {
                    LogFunc(("CF_UNICODETEXT\n"));

                    vboxClipboardGetData(VBOX_SHARED_CLIPBOARD_FMT_UNICODETEXT, uniString, (lstrlenW(uniString) + 1) * 2,
                                         pv, cb, pcbActual);

                    GlobalUnlock(hClip);
                }
                else
                {
                    hClip = NULL;
                }
            }
        }
        else if (u32Format & VBOX_SHARED_CLIPBOARD_FMT_HTML)
        {
            UINT format = RegisterClipboardFormat("HTML Format");
            if (format != 0)
            {
                hClip = GetClipboardData(format);
                if (hClip != NULL)
                {
                    LPVOID lp = GlobalLock(hClip);
                    if (lp != NULL)
                    {
                        LogFunc(("CF_HTML\n"));

                        vboxClipboardGetData(VBOX_SHARED_CLIPBOARD_FMT_HTML, lp, GlobalSize(hClip),
                                             pv, cb, pcbActual);
#ifdef VBOX_STRICT
                        LogFlowFunc(("Raw HTML clipboard data from host:"));
                        vboxClipboardDbgDumpHtml((char *)pv, cb);
#endif
                        GlobalUnlock(hClip);
                    }
                    else
                    {
                        hClip = NULL;
                    }
                }
            }
        }
#ifdef VBOX_WITH_SHARED_CLIPBOARD_URI_LIST
        else if (u32Format & VBOX_SHARED_CLIPBOARD_FMT_URI_LIST)
        {
            hClip = GetClipboardData(CF_HDROP);
            if (hClip != NULL)
            {
                LPVOID lp = GlobalLock(hClip);
                if (lp)
                {
                    /* Convert to a string list, separated by \r\n. */
                    DROPFILES *pDropFiles = (DROPFILES *)hClip;
                    AssertPtr(pDropFiles);

                    /* Do we need to do Unicode stuff? */
                    const bool fUnicode = RT_BOOL(pDropFiles->fWide);

                    /* Get the offset of the file list. */
                    Assert(pDropFiles->pFiles >= sizeof(DROPFILES));

                    /* Note: This is *not* pDropFiles->pFiles! DragQueryFile only
                     *       will work with the plain storage medium pointer! */
                    HDROP hDrop = (HDROP)(hClip);

                    /* First, get the file count. */
                    /** @todo Does this work on Windows 2000 / NT4? */
                    char *pszFiles = NULL;
                    uint32_t cchFiles = 0;
                    UINT cFiles = DragQueryFile(hDrop, UINT32_MAX /* iFile */, NULL /* lpszFile */, 0 /* cchFile */);

                    LogRel(("Shared Clipboard: Got %RU16 file(s), fUnicode=%RTbool\n", cFiles, fUnicode));

                    for (UINT i = 0; i < cFiles; i++)
                    {
                        UINT cchFile = DragQueryFile(hDrop, i /* File index */, NULL /* Query size first */, 0 /* cchFile */);
                        Assert(cchFile);

                        if (RT_FAILURE(rc))
                            break;

                        char *pszFileUtf8 = NULL; /* UTF-8 version. */
                        UINT cchFileUtf8 = 0;
                        if (fUnicode)
                        {
                            /* Allocate enough space (including terminator). */
                            WCHAR *pwszFile = (WCHAR *)RTMemAlloc((cchFile + 1) * sizeof(WCHAR));
                            if (pwszFile)
                            {
                                const UINT cwcFileUtf16 = DragQueryFileW(hDrop, i /* File index */,
                                                                         pwszFile, cchFile + 1 /* Include terminator */);

                                AssertMsg(cwcFileUtf16 == cchFile, ("cchFileUtf16 (%RU16) does not match cchFile (%RU16)\n",
                                                                    cwcFileUtf16, cchFile));
                                RT_NOREF(cwcFileUtf16);

                                rc = RTUtf16ToUtf8(pwszFile, &pszFileUtf8);
                                if (RT_SUCCESS(rc))
                                {
                                    cchFileUtf8 = (UINT)strlen(pszFileUtf8);
                                    Assert(cchFileUtf8);
                                }

                                RTMemFree(pwszFile);
                            }
                            else
                                rc = VERR_NO_MEMORY;
                        }
                        else /* ANSI */
                        {
                            /* Allocate enough space (including terminator). */
                            pszFileUtf8 = (char *)RTMemAlloc((cchFile + 1) * sizeof(char));
                            if (pszFileUtf8)
                            {
                                cchFileUtf8 = DragQueryFileA(hDrop, i /* File index */,
                                                             pszFileUtf8, cchFile + 1 /* Include terminator */);

                                AssertMsg(cchFileUtf8 == cchFile, ("cchFileUtf8 (%RU16) does not match cchFile (%RU16)\n",
                                                                   cchFileUtf8, cchFile));
                            }
                            else
                                rc = VERR_NO_MEMORY;
                        }

                        if (RT_SUCCESS(rc))
                        {
                            LogFlowFunc(("\tFile: %s (cchFile=%RU16)\n", pszFileUtf8, cchFileUtf8));

                            LogRel2(("Shared Clipboard: Adding host file '%s'\n", pszFileUtf8));

                            rc = RTStrAAppendExN(&pszFiles, 1 /* cPairs */, pszFileUtf8, cchFileUtf8);
                            if (RT_SUCCESS(rc))
                                cchFiles += cchFileUtf8;
                        }
                        else
                            LogRel(("Shared Clipboard: Error handling file entry #%u, rc=%Rrc\n", i, rc));

                        if (pszFileUtf8)
                            RTStrFree(pszFileUtf8);

                        if (RT_FAILURE(rc))
                            break;

                        /* Add separation between filenames.
                         * Note: Also do this for the last element of the list. */
                        rc = RTStrAAppendExN(&pszFiles, 1 /* cPairs */, "\r\n", 2 /* Bytes */);
                        if (RT_SUCCESS(rc))
                            cchFiles += 2; /* Include \r\n */
                    }

                    if (RT_SUCCESS(rc))
                    {
                        cchFiles += 1; /* Add string termination. */
                        uint32_t cbFiles = cchFiles * sizeof(char);

                        LogFlowFunc(("cFiles=%u, cchFiles=%RU32, cbFiles=%RU32, pszFiles=0x%p\n",
                                     cFiles, cchFiles, cbFiles, pszFiles));

                        /* Translate the list into URI elements. */
                        SharedClipboardURIList lstURI;
                        rc = lstURI.AppendNativePathsFromList(pszFiles, cbFiles,
                                                              SHAREDCLIPBOARDURILIST_FLAGS_ABSOLUTE_PATHS);
                        if (RT_SUCCESS(rc))
                        {
                            RTCString strRoot = lstURI.GetRootEntries();
                            size_t cbRoot = strRoot.length() + 1; /* Include termination */

                            if (cbRoot > cb) /** @todo Add overflow handling! */
                                cbRoot = cb; /* Never copy more than the available buffer supplies. */

                            memcpy(pv, strRoot.c_str(), cbRoot);

                            *pcbActual = (uint32_t)cbRoot;
                        }
                    }

                    LogFlowFunc(("Building CF_HDROP list rc=%Rrc, pszFiles=0x%p, cFiles=%RU16, cchFiles=%RU32\n",
                                 rc, pszFiles, cFiles, cchFiles));

                    if (pszFiles)
                        RTStrFree(pszFiles);

                    GlobalUnlock(hClip);
                }
            }
        }
#endif /* VBOX_WITH_SHARED_CLIPBOARD_URI_LIST */
        VBoxClipboardWinClose();
    }

    if (hClip == NULL)
    {
        /* Reply with empty data. */
        vboxClipboardGetData(0, NULL, 0, pv, cb, pcbActual);
    }

    return VINF_SUCCESS; /** @todo r=andy Return rc here? */
}

void vboxClipboardWriteData(VBOXCLIPBOARDCLIENTDATA *pClient, void *pv, uint32_t cb, uint32_t u32Format)
{
    LogFlowFuncEnter();

    /*
     * The guest returns data that was requested in the WM_RENDERFORMAT handler.
     */
    Assert(pClient->data.pv == NULL && pClient->data.cb == 0 && pClient->data.u32Format == 0);

    vboxClipboardDump(pv, cb, u32Format);

    if (cb > 0)
    {
        char *pszResult = NULL;

        if (   u32Format == VBOX_SHARED_CLIPBOARD_FMT_HTML
            && !IsWindowsHTML((const char*)pv))
        {
            /* check that this is not already CF_HTML */
            uint32_t cbResult;
            int rc = ConvertMimeToCFHTML((const char *)pv, cb, &pszResult, &cbResult);
            if (RT_SUCCESS(rc))
            {
                if (pszResult != NULL && cbResult != 0)
                {
                    pClient->data.pv        = pszResult;
                    pClient->data.cb        = cbResult;
                    pClient->data.u32Format = u32Format;
                }
            }
        }
        else
        {
            pClient->data.pv = RTMemDup(pv, cb);
            if (pClient->data.pv)
            {
                pClient->data.cb = cb;
                pClient->data.u32Format = u32Format;
            }
        }
    }

    AssertPtr(pClient->pCtx);
    int rc = RTSemEventSignal(pClient->pCtx->hRenderEvent);
    AssertRC(rc);

    /** @todo Return rc. */
}


/**
 * Extracts field value from CF_HTML struct
 *
 * @returns VBox status code
 * @param   pszSrc      source in CF_HTML format
 * @param   pszOption   Name of CF_HTML field
 * @param   puValue     Where to return extracted value of CF_HTML field
 */
static int GetHeaderValue(const char *pszSrc, const char *pszOption, uint32_t *puValue)
{
    int rc = VERR_INVALID_PARAMETER;

    Assert(pszSrc);
    Assert(pszOption);

    const char *pszOptionValue = RTStrStr(pszSrc, pszOption);
    if (pszOptionValue)
    {
        size_t cchOption = strlen(pszOption);
        Assert(cchOption);

        rc = RTStrToUInt32Ex(pszOptionValue + cchOption, NULL, 10, puValue);
    }
    return rc;
}


/**
 * Check that the source string contains CF_HTML struct
 *
 * @param   pszSource   source string.
 *
 * @returns @c true if the @a pszSource string is in CF_HTML format
 */
static bool IsWindowsHTML(const char *pszSource)
{
    return    RTStrStr(pszSource, "Version:") != NULL
           && RTStrStr(pszSource, "StartHTML:") != NULL;
}


/**
 * Converts clipboard data from CF_HTML format to MIME clipboard format.
 *
 * Returns allocated buffer that contains html converted to text/html mime type
 *
 * @returns VBox status code.
 * @param   pszSource   The input.
 * @param   cch         The length of the input.
 * @param   ppszOutput  Where to return the result.  Free using RTMemFree.
 * @param   pcbOutput   Where to the return length of the result (bytes/chars).
 */
static int ConvertCFHtmlToMime(const char *pszSource, const uint32_t cch, char **ppszOutput, uint32_t *pcbOutput)
{
    Assert(pszSource);
    Assert(cch);
    Assert(ppszOutput);
    Assert(pcbOutput);

    uint32_t offStart;
    int rc = GetHeaderValue(pszSource, "StartFragment:", &offStart);
    if (RT_SUCCESS(rc))
    {
        uint32_t offEnd;
        rc = GetHeaderValue(pszSource, "EndFragment:", &offEnd);
        if (RT_SUCCESS(rc))
        {
            if (   offStart > 0
                && offEnd > 0
                && offEnd > offStart
                && offEnd <= cch)
            {
                uint32_t cchSubStr = offEnd - offStart;
                char *pszResult = (char *)RTMemAlloc(cchSubStr + 1);
                if (pszResult)
                {
                    rc = RTStrCopyEx(pszResult, cchSubStr + 1, pszSource + offStart, cchSubStr);
                    if (RT_SUCCESS(rc))
                    {
                        *ppszOutput = pszResult;
                        *pcbOutput  = (uint32_t)(cchSubStr + 1);
                        rc = VINF_SUCCESS;
                    }
                    else
                    {
                        LogRelFlowFunc(("Error: Unknown CF_HTML format. Expected EndFragment. rc = %Rrc\n", rc));
                        RTMemFree(pszResult);
                    }
                }
                else
                {
                    LogRelFlowFunc(("Error: Unknown CF_HTML format. Expected EndFragment\n"));
                    rc = VERR_NO_MEMORY;
                }
            }
            else
            {
                LogRelFlowFunc(("Error: CF_HTML out of bounds - offStart=%#x offEnd=%#x cch=%#x\n", offStart, offEnd, cch));
                rc = VERR_INVALID_PARAMETER;
            }
        }
        else
        {
            LogRelFlowFunc(("Error: Unknown CF_HTML format. Expected EndFragment. rc = %Rrc\n", rc));
            rc = VERR_INVALID_PARAMETER;
        }
    }
    else
    {
        LogRelFlowFunc(("Error: Unknown CF_HTML format. Expected StartFragment. rc = %Rrc\n", rc));
        rc = VERR_INVALID_PARAMETER;
    }

    return rc;
}



/**
 * Converts source UTF-8 MIME HTML clipboard data to UTF-8 CF_HTML format.
 *
 * This is just encapsulation work, slapping a header on the data.
 *
 * It allocates
 *
 * Calculations:
 *   Header length = format Length + (2*(10 - 5('%010d'))('digits')) - 2('%s') = format length + 8
 *   EndHtml       = Header length + fragment length
 *   StartHtml     = 105(constant)
 *   StartFragment = 141(constant) may vary if the header html content will be extended
 *   EndFragment   = Header length + fragment length - 38(ending length)
 *
 * @param   pszSource   Source buffer that contains utf-16 string in mime html format
 * @param   cb          Size of source buffer in bytes
 * @param   ppszOutput  Where to return the allocated output buffer to put converted UTF-8
 *                      CF_HTML clipboard data.  This function allocates memory for this.
 * @param   pcbOutput   Where to return the size of allocated result buffer in bytes/chars, including zero terminator
 *
 * @note    output buffer should be free using RTMemFree()
 * @note    Everything inside of fragment can be UTF8. Windows allows it. Everything in header should be Latin1.
 */
static int ConvertMimeToCFHTML(const char *pszSource, size_t cb, char **ppszOutput, uint32_t *pcbOutput)
{
    Assert(ppszOutput);
    Assert(pcbOutput);
    Assert(pszSource);
    Assert(cb);

    /* construct CF_HTML formatted string */
    char *pszResult = NULL;
    size_t cchFragment;
    int rc = RTStrNLenEx(pszSource, cb, &cchFragment);
    if (!RT_SUCCESS(rc))
    {
        LogRelFlowFunc(("Error: invalid source fragment. rc = %Rrc\n"));
        return VERR_INVALID_PARAMETER;
    }

    /*
    @StartHtml - pos before <html>
    @EndHtml - whole size of text excluding ending zero char
    @StartFragment - pos after <!--StartFragment-->
    @EndFragment - pos before <!--EndFragment-->
    @note: all values includes CR\LF inserted into text
    Calculations:
    Header length = format Length + (3*6('digits')) - 2('%s') = format length + 16 (control value - 183)
    EndHtml  = Header length + fragment length
    StartHtml = 105(constant)
    StartFragment = 143(constant)
    EndFragment  = Header length + fragment length - 40(ending length)
    */
    static const char s_szFormatSample[] =
    /*   0:   */ "Version:1.0\r\n"
    /*  13:   */ "StartHTML:000000101\r\n"
    /*  34:   */ "EndHTML:%0000009u\r\n" // END HTML = Header length + fragment length
    /*  53:   */ "StartFragment:000000137\r\n"
    /*  78:   */ "EndFragment:%0000009u\r\n"
    /* 101:   */ "<html>\r\n"
    /* 109:   */ "<body>\r\n"
    /* 117:   */ "<!--StartFragment-->"
    /* 137:   */ "%s"
    /* 137+2: */ "<!--EndFragment-->\r\n"
    /* 157+2: */ "</body>\r\n"
    /* 166+2: */ "</html>\r\n";
    /* 175+2: */
    AssertCompile(sizeof(s_szFormatSample) == 175 + 2 + 1);

    /* calculate parameters of CF_HTML header */
    size_t cchHeader      = sizeof(s_szFormatSample) - 1;
    size_t offEndHtml     = cchHeader + cchFragment;
    size_t offEndFragment = cchHeader + cchFragment - 38; /* 175-137 = 38 */
    pszResult = (char *)RTMemAlloc(offEndHtml + 1);
    if (pszResult == NULL)
    {
        LogRelFlowFunc(("Error: Cannot allocate memory for result buffer. rc = %Rrc\n"));
        return VERR_NO_MEMORY;
    }

    /* format result CF_HTML string */
    size_t cchFormatted = RTStrPrintf(pszResult, offEndHtml + 1,
                                      s_szFormatSample, offEndHtml, offEndFragment, pszSource);
    Assert(offEndHtml == cchFormatted); NOREF(cchFormatted);

#ifdef VBOX_STRICT
    /* Control calculations. check consistency.*/
    static const char s_szStartFragment[] = "<!--StartFragment-->";
    static const char s_szEndFragment[] = "<!--EndFragment-->";

    /* check 'StartFragment:' value */
    const char *pszRealStartFragment = RTStrStr(pszResult, s_szStartFragment);
    Assert(&pszRealStartFragment[sizeof(s_szStartFragment) - 1] - pszResult == 137);

    /* check 'EndFragment:' value */
    const char *pszRealEndFragment = RTStrStr(pszResult, s_szEndFragment);
    Assert((size_t)(pszRealEndFragment - pszResult) == offEndFragment);
#endif

    *ppszOutput = pszResult;
    *pcbOutput = (uint32_t)cchFormatted + 1;
    Assert(*pcbOutput == cchFormatted + 1);

    return VINF_SUCCESS;
}

