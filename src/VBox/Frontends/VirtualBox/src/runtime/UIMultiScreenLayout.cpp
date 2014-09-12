/* $Id$ */
/** @file
 * VBox Qt GUI - UIMultiScreenLayout class implementation.
 */

/*
 * Copyright (C) 2010-2013 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

/* Qt includes: */
#include <QApplication>
#include <QDesktopWidget>
#include <QMenu>

/* GUI includes: */
#include "UIDefs.h"
#include "UIMultiScreenLayout.h"
#include "UIActionPoolRuntime.h"
#include "UIMachineLogic.h"
#include "UIFrameBuffer.h"
#include "UISession.h"
#include "UIMessageCenter.h"
#include "UIExtraDataManager.h"
#include "VBoxGlobal.h"

/* COM includes: */
#include "COMEnums.h"
#include "CSession.h"
#include "CConsole.h"
#include "CMachine.h"
#include "CDisplay.h"

UIMultiScreenLayout::UIMultiScreenLayout(UIMachineLogic *pMachineLogic)
    : m_pMachineLogic(pMachineLogic)
{
    /* Calculate host/guest screen count: */
    calculateHostMonitorCount();
    calculateGuestScreenCount();
}

void UIMultiScreenLayout::update()
{
    LogRelFlow(("UIMultiScreenLayout::update: Started...\n"));

    /* Clear screen-map initially: */
    m_screenMap.clear();

    /* Make a pool of available host screens: */
    QList<int> availableScreens;
    for (int i = 0; i < m_cHostScreens; ++i)
        availableScreens << i;

    /* Load all combinations stored in the settings file.
     * We have to make sure they are valid, which means there have to be unique combinations
     * and all guests screens need there own host screen. */
    CDisplay display = m_pMachineLogic->session().GetConsole().GetDisplay();
    bool fShouldWeAutoMountGuestScreens = gEDataManager->autoMountGuestScreensEnabled(vboxGlobal().managedVMUuid());
    LogRelFlow(("UIMultiScreenLayout::update: GUI/AutomountGuestScreens is %s.\n", fShouldWeAutoMountGuestScreens ? "enabled" : "disabled"));
    QDesktopWidget *pDW = QApplication::desktop();
    foreach (int iGuestScreen, m_guestScreens)
    {
        /* Initialize variables: */
        bool fValid = false;
        int iHostScreen = -1;

        if (!fValid)
        {
            /* If the user ever selected a combination in the view menu, we have the following entry: */
            iHostScreen = gEDataManager->hostScreenForPassedGuestScreen(iGuestScreen, vboxGlobal().managedVMUuid());
            /* Revalidate: */
            fValid =    iHostScreen >= 0 && iHostScreen < m_cHostScreens /* In the host screen bounds? */
                     && m_screenMap.key(iHostScreen, -1) == -1; /* Not taken already? */
        }

        if (!fValid)
        {
            /* Check the position of the guest window in normal mode.
             * This makes sure that on first use fullscreen/seamless window opens on the same host-screen as the normal window was before.
             * This even works with multi-screen. The user just have to move all the normal windows to the target host-screens
             * and they will magically open there in fullscreen/seamless also. */
            QRect geo = gEDataManager->machineWindowGeometry(UIVisualStateType_Normal, iGuestScreen, vboxGlobal().managedVMUuid());
            /* If geometry is valid: */
            if (!geo.isNull())
            {
                /* Get top-left corner position: */
                QPoint topLeftPosition(geo.topLeft());
                /* Check which host-screen the position belongs to: */
                iHostScreen = pDW->screenNumber(topLeftPosition);
                /* Revalidate: */
                fValid =    iHostScreen >= 0 && iHostScreen < m_cHostScreens /* In the host screen bounds? */
                         && m_screenMap.key(iHostScreen, -1) == -1; /* Not taken already? */
            }
        }

        if (!fValid)
        {
            /* If still not valid, pick the next one
             * if there is still available host screen: */
            if (!availableScreens.isEmpty())
            {
                iHostScreen = availableScreens.first();
                fValid = true;
            }
        }

        if (fValid)
        {
            /* Register host screen for the guest screen: */
            m_screenMap.insert(iGuestScreen, iHostScreen);
            /* Remove it from the list of available host screens: */
            availableScreens.removeOne(iHostScreen);
        }
        /* Do we have opinion about what to do with excessive guest-screen? */
        else if (fShouldWeAutoMountGuestScreens)
        {
            /* Then we have to disable excessive guest-screen: */
            LogRelFlow(("UIMultiScreenLayout::update: Disabling excessive guest-screen %d.\n", iGuestScreen));
            display.SetVideoModeHint(iGuestScreen, false, false, 0, 0, 0, 0, 0);
        }
    }

    /* Are we still have available host-screens
     * and have opinion about what to do with disabled guest-screens? */
    if (!availableScreens.isEmpty() && fShouldWeAutoMountGuestScreens)
    {
        /* How many excessive host-screens do we have? */
        int cExcessiveHostScreens = availableScreens.size();
        /* How many disabled guest-screens do we have? */
        int cDisabledGuestScreens = m_disabledGuestScreens.size();
        /* We have to try to enable disabled guest-screens if any: */
        int cGuestScreensToEnable = qMin(cExcessiveHostScreens, cDisabledGuestScreens);
        UISession *pSession = m_pMachineLogic->uisession();
        for (int iGuestScreenIndex = 0; iGuestScreenIndex < cGuestScreensToEnable; ++iGuestScreenIndex)
        {
            /* Defaults: */
            ULONG uWidth = 800;
            ULONG uHeight = 600;
            /* Try to get previous guest-screen arguments: */
            int iGuestScreen = m_disabledGuestScreens[iGuestScreenIndex];
            if (UIFrameBuffer *pFrameBuffer = pSession->frameBuffer(iGuestScreen))
            {
                if (pFrameBuffer->width() > 0)
                    uWidth = pFrameBuffer->width();
                if (pFrameBuffer->height() > 0)
                    uHeight = pFrameBuffer->height();
                pFrameBuffer->setAutoEnabled(true);
            }
            /* Re-enable guest-screen with proper resolution: */
            LogRelFlow(("UIMultiScreenLayout::update: Enabling guest-screen %d with following resolution: %dx%d.\n",
                        iGuestScreen, uWidth, uHeight));
            display.SetVideoModeHint(iGuestScreen, true, false, 0, 0, uWidth, uHeight, 32);
        }
    }

    /* Notifies about layout update: */
    emit sigScreenLayoutUpdate();

    LogRelFlow(("UIMultiScreenLayout::update: Finished!\n"));
}

void UIMultiScreenLayout::rebuild()
{
    LogRelFlow(("UIMultiScreenLayout::rebuild: Started...\n"));

    /* Recalculate host/guest screen count: */
    calculateHostMonitorCount();
    calculateGuestScreenCount();
    /* Update layout: */
    update();

    LogRelFlow(("UIMultiScreenLayout::rebuild: Finished!\n"));
}

int UIMultiScreenLayout::hostScreenCount() const
{
    return m_cHostScreens;
}

int UIMultiScreenLayout::guestScreenCount() const
{
    return m_guestScreens.size();
}

int UIMultiScreenLayout::hostScreenForGuestScreen(int iScreenId) const
{
    return m_screenMap.value(iScreenId, 0);
}

bool UIMultiScreenLayout::hasHostScreenForGuestScreen(int iScreenId) const
{
    return m_screenMap.contains(iScreenId);
}

quint64 UIMultiScreenLayout::memoryRequirements() const
{
    return memoryRequirements(m_screenMap);
}

bool UIMultiScreenLayout::isHostTaskbarCovert() const
{
    /* Check for all screens which are in use if they have some
     * taskbar/menubar/dock on it. Its done by comparing the available with the
     * screen geometry. Only if they are the same for all screens, there are no
     * host area covert. This is a little bit ugly, but there seems no other
     * way to find out if we are on a screen where the taskbar/dock or whatever
     * is present. */
    QDesktopWidget *pDW = QApplication::desktop();
    for (int i = 0; i < m_screenMap.size(); ++i)
    {
        int hostScreen = m_screenMap.value(i);
        if (pDW->availableGeometry(hostScreen) != pDW->screenGeometry(hostScreen))
            return true;
    }
    return false;
}

void UIMultiScreenLayout::sltHandleScreenLayoutChange(int iRequestedGuestScreen, int iRequestedHostScreen)
{
    /* Search for the virtual screen which is currently displayed on the
     * requested host screen. When there is one found, we swap both. */
    QMap<int,int> tmpMap(m_screenMap);
    int iCurrentGuestScreen = tmpMap.key(iRequestedHostScreen, -1);
    if (iCurrentGuestScreen != -1 && tmpMap.contains(iRequestedGuestScreen))
        tmpMap.insert(iCurrentGuestScreen, tmpMap.value(iRequestedGuestScreen));
    else
        tmpMap.remove(iCurrentGuestScreen);
    tmpMap.insert(iRequestedGuestScreen, iRequestedHostScreen);

    /* Check the memory requirements first: */
    bool fSuccess = true;
    CMachine machine = m_pMachineLogic->session().GetMachine();
    if (m_pMachineLogic->uisession()->isGuestAdditionsActive())
    {
        quint64 availBits = machine.GetVRAMSize() * _1M * 8;
        quint64 usedBits = memoryRequirements(tmpMap);
        fSuccess = availBits >= usedBits;
        if (!fSuccess)
        {
            /* We have too little video memory for the new layout, so say it to the user and revert all the changes: */
            if (m_pMachineLogic->visualStateType() == UIVisualStateType_Seamless)
                msgCenter().cannotSwitchScreenInSeamless((((usedBits + 7) / 8 + _1M - 1) / _1M) * _1M);
            else
                fSuccess = msgCenter().cannotSwitchScreenInFullscreen((((usedBits + 7) / 8 + _1M - 1) / _1M) * _1M);
        }
    }
    /* Make sure memory requirements matched: */
    if (!fSuccess)
        return;

    /* Swap the maps: */
    m_screenMap = tmpMap;

    /* Save guest-to-host mapping: */
    saveScreenMapping();

    /* Notifies about layout change: */
    emit sigScreenLayoutChange();
}

void UIMultiScreenLayout::calculateHostMonitorCount()
{
    m_cHostScreens = QApplication::desktop()->screenCount();
}

void UIMultiScreenLayout::calculateGuestScreenCount()
{
    /* Get machine: */
    CMachine machine = m_pMachineLogic->session().GetMachine();
    /* Enumerate all the guest screens: */
    m_guestScreens.clear();
    m_disabledGuestScreens.clear();
    for (uint iGuestScreen = 0; iGuestScreen < machine.GetMonitorCount(); ++iGuestScreen)
        if (m_pMachineLogic->uisession()->isScreenVisible(iGuestScreen))
            m_guestScreens << iGuestScreen;
        else
            m_disabledGuestScreens << iGuestScreen;
}

void UIMultiScreenLayout::saveScreenMapping()
{
    foreach (const int &iGuestScreen, m_guestScreens)
    {
        const int iHostScreen = m_screenMap.value(iGuestScreen, -1);
        gEDataManager->setHostScreenForPassedGuestScreen(iGuestScreen, iHostScreen, vboxGlobal().managedVMUuid());
    }
}

quint64 UIMultiScreenLayout::memoryRequirements(const QMap<int, int> &screenLayout) const
{
    ULONG width = 0;
    ULONG height = 0;
    ULONG guestBpp = 0;
    LONG xOrigin = 0;
    LONG yOrigin = 0;
    quint64 usedBits = 0;
    CDisplay display = m_pMachineLogic->uisession()->session().GetConsole().GetDisplay();
    foreach (int iGuestScreen, m_guestScreens)
    {
        QRect screen;
        if (m_pMachineLogic->visualStateType() == UIVisualStateType_Seamless)
            screen = QApplication::desktop()->availableGeometry(screenLayout.value(iGuestScreen, 0));
        else
            screen = QApplication::desktop()->screenGeometry(screenLayout.value(iGuestScreen, 0));
        display.GetScreenResolution(iGuestScreen, width, height, guestBpp, xOrigin, yOrigin);
        usedBits += screen.width() * /* display width */
                    screen.height() * /* display height */
                    guestBpp + /* guest bits per pixel */
                    _1M * 8; /* current cache per screen - may be changed in future */
    }
    usedBits += 4096 * 8; /* adapter info */
    return usedBits;
}

