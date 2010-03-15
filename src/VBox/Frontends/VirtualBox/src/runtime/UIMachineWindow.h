/** @file
 *
 * VBox frontends: Qt GUI ("VirtualBox"):
 * UIMachineWindow class declaration
 */

/*
 * Copyright (C) 2010 Sun Microsystems, Inc.
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa
 * Clara, CA 95054 USA or visit http://www.sun.com if you need
 * additional information or have any questions.
 */

#ifndef __UIMachineWindow_h__
#define __UIMachineWindow_h__

/* Local includes */
#include "UIMachineDefs.h"

/* Global forwards */
class QWidget;
class QGridLayout;
class QSpacerItem;
class QCloseEvent;

/* Local forwards */
class CSession;
class UISession;
class UIMachineLogic;
class UIMachineView;

class UIMachineWindow
{
public:

    /* Factory function to create required machine window child: */
    static UIMachineWindow* create(UIMachineLogic *pMachineLogic, UIVisualStateType visualStateType, ulong uScreenId = 0);
    static void destroy(UIMachineWindow *pWhichWindow);

    /* Abstract slot to close machine window: */
    virtual void sltTryClose();

    /* Public getters: */
    virtual UIMachineLogic* machineLogic() const { return m_pMachineLogic; }
    virtual QWidget* machineWindow() const { return m_pMachineWindow; }
    virtual UIMachineView* machineView() const { return m_pMachineView; }
    UISession* uisession() const;
    CSession& session() const;

    virtual void reshow() {}

protected:

    /* Machine window constructor/destructor: */
    UIMachineWindow(UIMachineLogic *pMachineLogic, ulong uScreenId);
    virtual ~UIMachineWindow();

    /* Protected getters: */
    const QString& defaultWindowTitle() const { return m_strWindowTitlePrefix; }

    /* Translate routine: */
    virtual void retranslateUi();

    /* Common machine window event handlers: */
    void closeEvent(QCloseEvent *pEvent);

    /* Prepare helpers: */
    virtual void prepareWindowIcon();
    virtual void prepareConsoleConnections();
    virtual void prepareAdditionsDownloader();
    virtual void prepareMachineViewContainer();
    //virtual void loadWindowSettings() {}

    /* Cleanup helpers: */
    //virtual void saveWindowSettings() {}
    //virtual void cleanupMachineViewContainer() {}
    //virtual void cleanupConsoleConnections() {}
    //virtual void cleanupWindowIcon() {}

    /* Update routines: */
    virtual void updateAppearanceOf(int iElement);

    /* Protected slots: */
    virtual void sltMachineStateChanged();

    /* Protected variables: */
    UIMachineLogic *m_pMachineLogic;
    QWidget *m_pMachineWindow;

    /* Virtual screen number: */
    ulong m_uScreenId;

    QGridLayout *m_pMachineViewContainer;
    QSpacerItem *m_pTopSpacer;
    QSpacerItem *m_pBottomSpacer;
    QSpacerItem *m_pLeftSpacer;
    QSpacerItem *m_pRightSpacer;

    UIMachineView *m_pMachineView;
    QString m_strWindowTitlePrefix;

    friend class UIMachineLogic;
};

#endif // __UIMachineWindow_h__

