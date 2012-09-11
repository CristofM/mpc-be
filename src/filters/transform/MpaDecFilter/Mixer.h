/*
 * $Id:
 *
 * (C) 2012 see Authors.txt
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

#pragma once

struct AVAudioResampleContext;

class CMixer
{
protected:
	AVAudioResampleContext* m_pAVRCxt;

public:
	CMixer();

	HRESULT Mixing(float* pOutput, WORD out_ch, DWORD out_layout, float* pInput, int samples, WORD in_ch, DWORD in_layout);
	void CMixer::Reset();
};
