
// hook_app.h : PROJECT_NAME Ӧ�ó������ͷ�ļ�
//

#pragma once

#ifndef __AFXWIN_H__
    #error "�ڰ������ļ�֮ǰ������stdafx.h�������� PCH �ļ�"
#endif

#include "resource/resource.h"        // ������


// HookApp:
// �йش����ʵ�֣������ hook_app.cpp
//

class HookApp : public CWinAppEx
{
public:
    HookApp();

// ��д
    public:
    virtual BOOL InitInstance();

// ʵ��

    DECLARE_MESSAGE_MAP()
};

extern HookApp theApp;