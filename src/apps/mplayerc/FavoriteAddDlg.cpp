/*
 * $Id$
 *
 * (C) 2003-2006 Gabest
 * (C) 2006-2012 see Authors.txt
 *
 * This file is part of MPC-BE.
 *
 * MPC-BE is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * MPC-BE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "stdafx.h"
#include "mplayerc.h"
#include "FavoriteAddDlg.h"
#include "SettingsDefines.h"

// CFavoriteAddDlg dialog

IMPLEMENT_DYNAMIC(CFavoriteAddDlg, CCmdUIDialog)
CFavoriteAddDlg::CFavoriteAddDlg(CString shortname, CString fullname, CWnd* pParent)
	: CCmdUIDialog(CFavoriteAddDlg::IDD, pParent)
	, m_shortname(shortname)
	, m_fullname(fullname)
	, m_bRememberPos(TRUE)
	, m_bRelativeDrive(FALSE)
{
}

CFavoriteAddDlg::~CFavoriteAddDlg()
{
}

void CFavoriteAddDlg::DoDataExchange(CDataExchange* pDX)
{
	__super::DoDataExchange(pDX);

	DDX_Control(pDX, IDC_COMBO1, m_namectrl);
	DDX_CBString(pDX, IDC_COMBO1, m_name);
	DDX_Check(pDX, IDC_CHECK1, m_bRememberPos);
	DDX_Check(pDX, IDC_CHECK2, m_bRelativeDrive);
}

BOOL CFavoriteAddDlg::OnInitDialog()
{
	__super::OnInitDialog();

	AppSettings& s = AfxGetAppSettings();

	if ( !m_shortname.IsEmpty() ) {
		m_namectrl.AddString( m_shortname );
	}

	if ( !m_fullname.IsEmpty() ) {
		m_namectrl.AddString( m_fullname );
	}

	::CorrectComboListWidth(m_namectrl);

	m_bRememberPos = s.bFavRememberPos;
	m_bRelativeDrive = s.bFavRelativeDrive;

	UpdateData(FALSE);

	m_namectrl.SetCurSel( 0 );

	return TRUE;
}

BEGIN_MESSAGE_MAP(CFavoriteAddDlg, CCmdUIDialog)
	ON_UPDATE_COMMAND_UI(IDOK, OnUpdateOk)
END_MESSAGE_MAP()

// CFavoriteAddDlg message handlers

void CFavoriteAddDlg::OnUpdateOk(CCmdUI *pCmdUI)
{
	UpdateData();

	pCmdUI->Enable( !m_name.IsEmpty() );
}

void CFavoriteAddDlg::OnOK()
{
	UpdateData();

	AppSettings& s = AfxGetAppSettings();

	s.bFavRememberPos = !!m_bRememberPos;
	s.bFavRelativeDrive = !!m_bRelativeDrive;

	CCmdUIDialog::OnOK();
}
