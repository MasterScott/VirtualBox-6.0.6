/* $Id$ */
/** @file
 * IPRT Testcase - Error Messages.
 */

/*
 * Copyright (C) 2006-2012 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 *
 * The contents of this file may alternatively be used under the terms
 * of the Common Development and Distribution License Version 1.0
 * (CDDL) only, as it comes in the "COPYING.CDDL" file of the
 * VirtualBox OSE distribution, in which case the provisions of the
 * CDDL are applicable instead of those of the GPL.
 *
 * You may elect to license modified versions of this file under the
 * terms and conditions of either the GPL or the CDDL or both.
 */


/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#include <iprt/err.h>
#include <iprt/string.h>
#include <iprt/test.h>
#ifdef VBOX
# include <VBox/err.h>
#endif


/*******************************************************************************
*   Global Variables                                                           *
*******************************************************************************/
/** Array of messages.
 * The data is generated by a sed script.
 */
static const RTSTATUSMSG  g_aErrorMessages[] =
{
#include "errmsgdata.h"
};


static bool strIsPermissibleDuplicate(const RTSTATUSMSG *pMsg)
{
    const char *pszDefine = pMsg->pszDefine;
    size_t      cchDefine = strlen(pszDefine);

#define STR_ENDS_WITH(a_psz, a_cch, a_sz) \
    ( (a_cch) >= sizeof(a_sz) && !strncmp((a_psz) + (a_cch) - sizeof(a_sz) + 1, RT_STR_TUPLE(a_sz)) )

    return  STR_ENDS_WITH(pszDefine, cchDefine, "_FIRST")
         || STR_ENDS_WITH(pszDefine, cchDefine, "_LAST")
         || STR_ENDS_WITH(pszDefine, cchDefine, "_LOWEST")
         || STR_ENDS_WITH(pszDefine, cchDefine, "_HIGHEST")
         || strstr(pMsg->pszMsgShort, "(mapped to") != 0;
}


int main()
{
    RTTEST hTest;
    RTEXITCODE rcExit = RTTestInitAndCreate("tstRTErrUnique", &hTest);
    if (rcExit != RTEXITCODE_SUCCESS)
        return rcExit;
    RTTestBanner(hTest);

    RTTestSub(hTest, "IPRT status code");
    for (uint32_t i = 0; i < RT_ELEMENTS(g_aErrorMessages) - 1; i++)
        if (!strIsPermissibleDuplicate(&g_aErrorMessages[i]))
            for (uint32_t j = i + 1; j < RT_ELEMENTS(g_aErrorMessages); j++)
                if (   !strIsPermissibleDuplicate(&g_aErrorMessages[j])
                    && g_aErrorMessages[i].iCode == g_aErrorMessages[j].iCode)
                    RTTestFailed(hTest, "Status code %d can mean both '%s' and '%s'",
                                 g_aErrorMessages[i].iCode,
                                 g_aErrorMessages[i].pszDefine,
                                 g_aErrorMessages[j].pszDefine);

    return RTTestSummaryAndDestroy(hTest);
}

