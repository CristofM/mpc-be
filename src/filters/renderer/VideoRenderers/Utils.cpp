/*
 * (C) 2016 see Authors.txt
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
#include <memory>

#include "Utils.h"

HRESULT DumpDX9Surface(IDirect3DDevice9* pD3DDev, IDirect3DSurface9* pSurface, wchar_t* filename)
{
	CheckPointer(pD3DDev, E_POINTER);
	CheckPointer(pSurface, E_POINTER);

	HRESULT hr;
	D3DSURFACE_DESC desc = {};

	if (FAILED(hr = pSurface->GetDesc(&desc))) {
		return hr;
	};

	CComPtr<IDirect3DSurface9> pTarget;
	if (FAILED(hr = pD3DDev->CreateRenderTarget(desc.Width, desc.Height, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, TRUE, &pTarget, NULL))
		|| FAILED(hr = pD3DDev->StretchRect(pSurface, NULL, pTarget, NULL, D3DTEXF_NONE))) {
		return hr;
	}

	unsigned len = desc.Width * desc.Height * 4;
	std::unique_ptr<BYTE[]> dib(DNew BYTE[sizeof(BITMAPINFOHEADER) + len]);
	if (!dib) {
		return E_OUTOFMEMORY;
	}

	BITMAPINFOHEADER* bih = (BITMAPINFOHEADER*)dib.get();
	memset(bih, 0, sizeof(BITMAPINFOHEADER));
	bih->biSize = sizeof(BITMAPINFOHEADER);
	bih->biWidth = desc.Width;
	bih->biHeight = desc.Height;
	bih->biBitCount = 32;
	bih->biPlanes = 1;
	bih->biSizeImage = DIBSIZE(*bih);

	D3DLOCKED_RECT r;
	hr = pTarget->LockRect(&r, NULL, D3DLOCK_READONLY);

	BitBltFromRGBToRGB(bih->biWidth, bih->biHeight,
		(BYTE*)(bih + 1), bih->biWidth * 4, 32,
		(BYTE*)r.pBits + r.Pitch * (desc.Height - 1), -(int)r.Pitch, 32);

	pTarget->UnlockRect();

	BITMAPFILEHEADER bfh;
	bfh.bfType = 0x4d42;
	bfh.bfOffBits = sizeof(bfh) + sizeof(BITMAPINFOHEADER);
	bfh.bfSize = bfh.bfOffBits + len;
	bfh.bfReserved1 = bfh.bfReserved2 = 0;

	FILE* fp;
	_wfopen_s(&fp, filename, L"wb");
	if (fp) {
		fwrite(&bfh, sizeof(bfh), 1, fp);
		fwrite(dib.get(), sizeof(BITMAPINFOHEADER) + len, 1, fp);
		fclose(fp);
	}

	return hr;
}

HRESULT DumpDX9Texture(IDirect3DDevice9* pD3DDev, IDirect3DTexture9* pTexture, wchar_t* filename)
{
	CheckPointer(pTexture, E_POINTER);

	CComPtr<IDirect3DSurface9> pSurface;
	pTexture->GetSurfaceLevel(0, &pSurface);

	return DumpDX9Surface(pD3DDev, pSurface, filename);
}