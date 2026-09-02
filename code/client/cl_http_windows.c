/*
===========================================================================
Copyright (C) 2025 Tim Angus (tim@ngus.net)

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#ifdef USE_HTTP

#include "client.h"

#include <windows.h>
#include <wininet.h>

static HINTERNET hInternet = NULL;
static HINTERNET hUrl = NULL;

static Q_PRINTF_FUNC(2, 3) void DropIf(qboolean condition, const char *fmt, ...)
{
    char buffer[1024];

    if (!condition)
        return;

    va_list argptr;
    va_start(argptr, fmt);
    Q_vsnprintf(buffer, sizeof(buffer), fmt, argptr);
    va_end(argptr);

    Com_Error(ERR_DROP, "Download Error: %s URL: %s", buffer, clc.downloadURL);
}

/*
=================
CL_HTTP_Init
=================
*/
qboolean CL_HTTP_Init()
{
    OSVERSIONINFO osvi = {sizeof(OSVERSIONINFO)};
    const char *windowsVersion = GetVersionEx(&osvi) ? va("Windows %lu.%lu (build %lu)",
                                                          osvi.dwMajorVersion,
                                                          osvi.dwMinorVersion,
                                                          osvi.dwBuildNumber)
                                                     : "Windows";

    hInternet = InternetOpenA(
        va("%s %s", Q3_VERSION, windowsVersion),
        INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);

    if (hInternet)
        CL_HTTP_ReqInit();

    return hInternet != NULL;
}

/*
=================
CL_HTTP_Available
=================
*/
qboolean CL_HTTP_Available()
{
    return hInternet != NULL;
}

/*
=================
CL_HTTP_Shutdown
=================
*/
void CL_HTTP_Shutdown(void)
{
    // Join workers before the session handle goes away: a thread still inside
    // InternetReadFile on a closed session is a crash on exit that only shows
    // up on a slow network.
    CL_HTTP_ReqShutdown();

    if (hInternet)
    {
        InternetCloseHandle(hInternet);
        hInternet = NULL;
    }
}

/*
=================
Sys_HTTP_Transfer

Blocking request/response over WinINet. Runs on a worker thread only.

Note this deliberately does NOT use InternetOpenUrlA the way the download path
above does: that convenience wrapper cannot send a request body, which rules
out every POST the matchmaker needs.
=================
*/
qboolean Sys_HTTP_Transfer(const char *method, const char *url,
                           const char *body, const char *headers,
                           int *code, char *out, int outSize, int *outLen,
                           char *err, int errSize)
{
    URL_COMPONENTSA parts;
    char        host[256];
    char        path[HTTP_MAX_URL];
    HINTERNET   hConnect = NULL, hRequest = NULL;
    DWORD       flags, status = 0, statusLen = sizeof(status), zero = 0;
    DWORD       timeout;
    int         total = 0;
    qboolean    result = qfalse;

    *code = 0;
    *outLen = 0;
    out[0] = '\0';

    if (!hInternet)
    {
        Q_strncpyz(err, "HTTP not initialised", errSize);
        return qfalse;
    }

    Com_Memset(&parts, 0, sizeof(parts));
    parts.dwStructSize = sizeof(parts);
    parts.lpszHostName = host;
    parts.dwHostNameLength = sizeof(host);
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = sizeof(path);

    if (!InternetCrackUrlA(url, 0, 0, &parts))
    {
        Q_strncpyz(err, va("bad URL (%lu)", GetLastError()), errSize);
        return qfalse;
    }

    hConnect = InternetConnectA(hInternet, host, parts.nPort, NULL, NULL,
                                INTERNET_SERVICE_HTTP, 0, 0);
    if (!hConnect)
    {
        Q_strncpyz(err, va("connect failed (%lu)", GetLastError()), errSize);
        return qfalse;
    }

    // Without these a matchmaker that is simply down leaves the worker parked
    // for WinINet's default, which is minutes.
    timeout = 5000;
    InternetSetOptionA(hConnect, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    timeout = 10000;
    InternetSetOptionA(hConnect, INTERNET_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionA(hConnect, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

    flags = INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_COOKIES |
            INTERNET_FLAG_NO_UI | INTERNET_FLAG_RELOAD | INTERNET_FLAG_PRAGMA_NOCACHE;
    if (parts.nScheme == INTERNET_SCHEME_HTTPS)
        flags |= INTERNET_FLAG_SECURE;

    hRequest = HttpOpenRequestA(hConnect, method, path, NULL, NULL, NULL, flags, 0);
    if (!hRequest)
    {
        Q_strncpyz(err, va("open request failed (%lu)", GetLastError()), errSize);
        goto done;
    }

    if (headers && *headers)
    {
        HttpAddRequestHeadersA(hRequest, headers, (DWORD)-1,
                               HTTP_ADDREQ_FLAG_ADD | HTTP_ADDREQ_FLAG_REPLACE);
    }

    if (!HttpSendRequestA(hRequest, NULL, 0,
                          (LPVOID)body, body ? (DWORD)strlen(body) : 0))
    {
        Q_strncpyz(err, va("send failed (%lu)", GetLastError()), errSize);
        goto done;
    }

    if (!HttpQueryInfoA(hRequest, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                        &status, &statusLen, &zero))
    {
        Q_strncpyz(err, va("no status code (%lu)", GetLastError()), errSize);
        goto done;
    }
    *code = (int)status;

    for (;;)
    {
        DWORD read = 0;

        if (total >= outSize - 1)
            break;      // response larger than we care to hold; keep what fits

        if (!InternetReadFile(hRequest, out + total,
                              (DWORD)(outSize - 1 - total), &read))
        {
            Q_strncpyz(err, va("read failed (%lu)", GetLastError()), errSize);
            goto done;
        }
        if (read == 0)
            break;
        total += (int)read;
    }

    out[total] = '\0';
    *outLen = total;
    result = qtrue;

done:
    if (hRequest)
        InternetCloseHandle(hRequest);
    if (hConnect)
        InternetCloseHandle(hConnect);

    return result;
}

/*
=================
CL_HTTP_BeginDownload
=================
*/
void CL_HTTP_BeginDownload(const char *remoteURL)
{
    DWORD httpCode = 0;
    DWORD contentLength = 0;
    DWORD len = sizeof(httpCode);
    DWORD zero = 0;
    BOOL success;

    hUrl = InternetOpenUrlA(hInternet, remoteURL,
                            va("Referer: ioQ3://%s\r\n", NET_AdrToString(clc.serverAddress)), (DWORD)-1,
                            INTERNET_FLAG_HYPERLINK |
                                INTERNET_FLAG_NO_CACHE_WRITE |
                                INTERNET_FLAG_NO_COOKIES |
                                INTERNET_FLAG_NO_UI |
                                INTERNET_FLAG_RESYNCHRONIZE |
                                INTERNET_FLAG_RELOAD,
                            0);

    DropIf(hUrl == NULL, "InternetOpenUrlA failed %lu", GetLastError());

    success = HttpQueryInfo(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &httpCode, &len, &zero);
    DropIf(!success, "Get HTTP_QUERY_STATUS_CODE failed %lu", GetLastError());

    DropIf(httpCode >= 400, "HTTP code %lu", httpCode);
    DropIf(httpCode != 200, "Unhandled HTTP code %lu", httpCode);

    success = HttpQueryInfo(hUrl, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER, &contentLength, &len, &zero);
    DropIf(!success, "Get HTTP_QUERY_CONTENT_LENGTH failed %lu", GetLastError());

    clc.downloadSize = (int)contentLength;
    Cvar_SetValue("cl_downloadSize", clc.downloadSize);
}

/*
=================
CL_HTTP_PerformDownload
=================
*/
qboolean CL_HTTP_PerformDownload(void)
{
    static BYTE readBuffer[256 * 1024];
    DWORD bytesRead = 0;
    BOOL success;

    DropIf(hUrl == NULL, "hUrl is NULL");

    success = InternetReadFile(hUrl, readBuffer, sizeof(readBuffer), &bytesRead);
    DropIf(!success, "InternetReadFile failed %lu", GetLastError());

    if (bytesRead > 0)
    {
        clc.downloadCount += bytesRead;
        Cvar_SetValue("cl_downloadCount", clc.downloadCount);

        DWORD bytesWritten = (DWORD)FS_Write(readBuffer, bytesRead, clc.download);
        DropIf(bytesWritten != bytesRead, "bytesWritten != bytesRead");

        return qfalse;
    }

    InternetCloseHandle(hUrl);
    return qtrue;
}

#endif /* USE_HTTP */
