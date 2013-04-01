/*
 * $Id$
 *
 * (C) 2006-2013 see Authors.txt
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

#define HAVE_AV_CONFIG_H

#include <Windows.h>
#include <WinNT.h>
#include <vfwmsgs.h>
#include <sys/timeb.h>
#include <time.h> // for the _time64 workaround
#include "FfmpegContext.h"

extern BOOL IsWinVistaOrLater(); // requires linking with DSUtils which is always the case
extern unsigned __int64 GetFileVersion(LPCTSTR fn);

extern "C" {
	#include <ffmpeg/libavcodec/dsputil.h>
	#include <ffmpeg/libavcodec/avcodec.h>
// This is kind of an hack but it avoids using a C++ keyword as a struct member name
#define class classFFMPEG
	#include <ffmpeg/libavcodec/mpegvideo.h>
#undef class
	#include <ffmpeg/libavcodec/golomb.h>
// hack since "h264.h" is using "new" as a variable
#define new newFFMPEG
	#include <ffmpeg/libavcodec/h264.h>
#undef new
	#include <ffmpeg/libavcodec/h264data.h>
	#include <ffmpeg/libavcodec/vc1.h>
	#include <ffmpeg/libavcodec/mpeg12.h>

	// Hack to use MinGW64 from 2.x branch
	void __mingw_raise_matherr(int typ, const char *name, double a1, double a2, double rslt) {}
}

#ifdef REGISTER_FILTER
	void *__imp_time64 = _time64;
#endif

static const WORD PCID_NVIDIA_VP5[] = {
	// http://us.download.nvidia.com/XFree86/Linux-x86_64/313.26/README/supportedchips.html
	// Nvidia VDPAU Feature Set D
	0x0FC6, // GeForce GTX 650
	0x0FD1, // GeForce GT 650M
	0x0FD2, // GeForce GT 640M
	0x0FD4, // GeForce GTX 660M
	0x0FD5, // GeForce GT 650M
	0x0FD8, // GeForce GT 640M
	0x0FD9, // GeForce GT 645M
	0x0FE0, // GeForce GTX 660M
	0x0FE1, // GeForce GT 730M
	0x0FF2, // GRID K1
	0x0FF9, // Quadro K2000D
	0x0FFA, // Quadro K600
	0x0FFB, // Quadro K2000M
	0x0FFC, // Quadro K1000M
	0x0FFD, // NVS 510
	0x0FFE, // Quadro K2000
	0x0FFF, // Quadro 410
	0x1005, // GeForce GTX TITAN
	0x1021, // Tesla K20Xm
	0x1022, // Tesla K20c
	0x1026, // Tesla K20s
	0x1028, // Tesla K20m
	0x1040, // GeForce GT 520 (not officially supported or typo) (4k tested)
	0x1042, // GeForce 510
	0x1048, // GeForce 605
	0x104A, // GeForce GT 610 (fully tested)
	0x104B, // GeForce GT 625 (OEM)
	0x1051, // GeForce GT 520MX
	0x1054, // GeForce 410M
	0x1055, // GeForce 410M
	0x1056, // NVS 4200M
	0x1057, // NVS 4200M
	0x105B, // GeForce 705M
	0x107D, // NVS 310
	0x1180, // GeForce GTX 680
	0x1183, // GeForce GTX 660 Ti (fully tested)
	0x1185, // GeForce GTX 660
	0x1188, // GeForce GTX 690
	0x1189, // GeForce GTX 670
	0x118F, // Tesla K10
	0x11A0, // GeForce GTX 680M
	0x11A1, // GeForce GTX 670MX
	0x11A2, // GeForce GTX 675MX
	0x11A3, // GeForce GTX 680MX
	0x11A7, // GeForce GTX 675MX
	0x11BA, // Quadro K5000
	0x11BC, // Quadro K5000M
	0x11BD, // Quadro K4000M
	0x11BE, // Quadro K3000M
	0x11BF, // VGX K2
	0x11C0, // GeForce GTX 660
	0x11C3, // GeForce GTX 650 Ti
	0x11C6, // GeForce GTX 650 Ti
	0x11FA, // Quadro K4000
};

bool CheckPCID(WORD pcid, const WORD* pPCIDs, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (pcid == pPCIDs[i]) {
			return true;
		}
	}

	return false;
}

#define CHECK_AVC_L52_SIZE(w, h) ((w) <= 4096 && (h) <= 4096 && (w) * (h) <= 36864 * 16 * 16)

inline MpegEncContext* GetMpegEncContext(struct AVCodecContext* pAVCtx)
{
	Mpeg1Context*		s1;
	MpegEncContext*		s = NULL;

	switch (pAVCtx->codec_id) {
		case AV_CODEC_ID_VC1 :
		case AV_CODEC_ID_H264 :
			s	= (MpegEncContext*)pAVCtx->priv_data;
			break;
		case AV_CODEC_ID_MPEG2VIDEO:
			s1	= (Mpeg1Context*)pAVCtx->priv_data;
			s 	= (MpegEncContext*)&s1->mpeg_enc_ctx;
			break;
	}
	return s;
}

HRESULT FFH264DecodeFrame (struct AVCodecContext* pAVCtx, struct AVFrame* pFrame, BYTE* pBuffer, UINT nSize, REFERENCE_TIME rtStart, int* pFramePOC, int* pOutPOC, REFERENCE_TIME* pOutrtStart, UINT* SecondFieldOffset, int* Sync)
{
	HRESULT hr = E_FAIL;
	if (pBuffer != NULL) {
		H264Context* h	= (H264Context*) pAVCtx->priv_data;
		int				got_picture	= 0;
		AVPacket		avpkt;
		av_init_packet(&avpkt);
		avpkt.data		= pBuffer;
		avpkt.size		= nSize;
		avpkt.pts		= rtStart;
		avpkt.flags		= AV_PKT_FLAG_KEY;
		int used_bytes	= avcodec_decode_video2(pAVCtx, pFrame, &got_picture, &avpkt);
		
#if defined(_DEBUG) && 0
		av_log(pAVCtx, AV_LOG_INFO, "FFH264DecodeFrame() : %d, %d\n", used_bytes, got_picture);
#endif

		if (used_bytes < 0) {
			return hr;
		}

		hr = S_OK;
		if (h->cur_pic_ptr) {
			if (pOutPOC) {
				*pOutPOC = pFrame->h264_poc_outputed;
			}
			if (pOutrtStart) {
				*pOutrtStart = pFrame->pkt_pts;
			}
			if (pFramePOC) {
				*pFramePOC = h->cur_pic_ptr->poc;
				if (*pFramePOC == INT_MIN) {
					return E_FAIL;
				}
			}
			if (SecondFieldOffset) {
				*SecondFieldOffset = 0;
				if (h->second_field_offset && (h->second_field_offset < nSize)) {
					*SecondFieldOffset = h->second_field_offset;
				}
			}
			if (Sync) {
				*Sync = h->sync;
			}
		}
	}
	return hr;
}

// returns TRUE if version is equal to or higher than A.B.C.D, returns FALSE otherwise
BOOL DriverVersionCheck (LARGE_INTEGER VideoDriverVersion, int A, int B, int C, int D)
{
	if (HIWORD(VideoDriverVersion.HighPart) > A) {
		return TRUE;
	} else if (HIWORD(VideoDriverVersion.HighPart) == A) {
		if (LOWORD(VideoDriverVersion.HighPart) > B) {
			return TRUE;
		} else if (LOWORD(VideoDriverVersion.HighPart) == B) {
			if (HIWORD(VideoDriverVersion.LowPart) > C) {
				return TRUE;
			} else if (HIWORD(VideoDriverVersion.LowPart) == C) {
				if (LOWORD(VideoDriverVersion.LowPart) >= D) {
					return TRUE;
				}
			}
		}
	}
	return FALSE;
}

int FFH264CheckCompatibility (int nWidth, int nHeight, struct AVCodecContext* pAVCtx, struct AVFrame* pFrame, DWORD nPCIVendor, DWORD nPCIDevice, LARGE_INTEGER VideoDriverVersion, bool nIsAtiDXVACompatible)
{
	H264Context*	pContext = (H264Context*) pAVCtx->priv_data;

	int video_is_level51			= 0;
	int no_level51_support			= 1;
	int too_much_ref_frames			= 0;
	int max_ref_frames_dpb41		= min(11, 8388608/(nWidth * nHeight) );

	if (pAVCtx->extradata != NULL) {
		int				got_picture	= 0;
		AVPacket		avpkt;
		av_init_packet(&avpkt);
		avpkt.data		= pAVCtx->extradata;
		avpkt.size		= pAVCtx->extradata_size;
		avpkt.flags		= AV_PKT_FLAG_KEY;
		int used_bytes	= avcodec_decode_video2(pAVCtx, pFrame, &got_picture, &avpkt);
	}

	SPS* cur_sps		= &pContext->sps;

	if (cur_sps != NULL) {
		if (cur_sps->bit_depth_luma > 8 || cur_sps->chroma_format_idc > 1) {
			return DXVA_HIGH_BIT;
		}

		if (cur_sps->profile_idc > 100) {
			return DXVA_PROFILE_HIGHER_THAN_HIGH;
		}

		video_is_level51			= cur_sps->level_idc >= 51 ? 1 : 0;
		int max_ref_frames			= max_ref_frames_dpb41; // default value is calculate

		if (nPCIVendor == PCIV_nVidia) {
			// nVidia cards support level 5.1 since drivers v6.14.11.7800 for XP and drivers v7.15.11.7800 for Vista/7
			if (IsWinVistaOrLater()) {
				if (DriverVersionCheck(VideoDriverVersion, 7, 15, 11, 7800)) {
					no_level51_support = 0;

					// max ref frames is 16 for HD and 11 otherwise
					max_ref_frames = (nWidth >= 1280) ? 16 : 11;
				}
			} else {
				if (DriverVersionCheck(VideoDriverVersion, 6, 14, 11, 7800)) {
					no_level51_support = 0;

					// max ref frames is 14
					max_ref_frames = 14;
				}
			}
		} else if (nPCIVendor == PCIV_S3_Graphics || nPCIVendor == PCIV_Intel) {
			no_level51_support = 0;
			max_ref_frames = 16;
		} else if (nPCIVendor == PCIV_ATI && nIsAtiDXVACompatible) {
			TCHAR path[MAX_PATH];
			GetSystemDirectory(path, MAX_PATH);
			wcscat(path, L"\\drivers\\atikmdag.sys\0");
			unsigned __int64 f_version = GetFileVersion(path);

			if (f_version) {
				LARGE_INTEGER VideoDriverVersion;
				VideoDriverVersion.QuadPart = f_version;

				if (IsWinVistaOrLater()) {
					// file version 8.1.1.1016 - Catalyst 10.4, WinVista & Win7
					if (DriverVersionCheck(VideoDriverVersion, 8, 1, 1, 1016)) {
						no_level51_support = 0;
						max_ref_frames = 16;
					}				
				} else {
					// TODO - need file version for Catalyst 10.4 under WinXP
				}
			
			} else {
				// driver version 8.14.1.6105 - Catalyst 10.4; TODO - verify this information
				if (DriverVersionCheck(VideoDriverVersion, 8, 14, 1, 6105)) {
					no_level51_support = 0;
					max_ref_frames = 16;
				}
			}
		}

		// Check maximum allowed number reference frames
		if (cur_sps->ref_frame_count > max_ref_frames) {
			too_much_ref_frames = 1;
		}
	}

	int Flags = 0;
	if (video_is_level51 * no_level51_support) {
		Flags |= DXVA_UNSUPPORTED_LEVEL;
	}
	if (too_much_ref_frames) {
		Flags |= DXVA_TOO_MANY_REF_FRAMES;
	}

	return Flags;
}

void CopyScalingMatrix (DXVA_Qmatrix_H264* pDest, PPS* pps, DWORD nPCIVendor)
{
	int i, j;
	memset(pDest, 0, sizeof(DXVA_Qmatrix_H264));
	if (/*nPCIVendor == PCIV_ATI*/0) {
		for (i = 0; i < 6; i++)
			for (j = 0; j < 16; j++)
				pDest->bScalingLists4x4[i][j] = pps->scaling_matrix4[i][j];

		for (i = 0; i < 64; i++) {
			pDest->bScalingLists8x8[0][i] = pps->scaling_matrix8[0][i];
			pDest->bScalingLists8x8[1][i] = pps->scaling_matrix8[3][i];
		}
	} else {
		for (i = 0; i < 6; i++)
			for (j = 0; j < 16; j++)
				pDest->bScalingLists4x4[i][j] = pps->scaling_matrix4[i][zigzag_scan[j]];

		for (i = 0; i < 64; i++) {
			pDest->bScalingLists8x8[0][i] = pps->scaling_matrix8[0][ff_zigzag_direct[i]];
			pDest->bScalingLists8x8[1][i] = pps->scaling_matrix8[3][ff_zigzag_direct[i]];
		}
	}
}

USHORT FFH264FindRefFrameIndex (USHORT num_frame, DXVA_PicParams_H264* pDXVAPicParams)
{
	for (int i=0; i<pDXVAPicParams->num_ref_frames; i++) {
		if (pDXVAPicParams->FrameNumList[i] == num_frame) {
			return pDXVAPicParams->RefFrameList[i].Index7Bits;
		}
	}

#ifdef _DEBUG
	//	DebugBreak();		// Ref frame not found !
#endif

	return 127;
}

HRESULT FFH264BuildPicParams (DXVA_PicParams_H264* pDXVAPicParams, DXVA_Qmatrix_H264* pDXVAScalingMatrix, int* nFieldType, int* nSliceType, struct AVCodecContext* pAVCtx, DWORD nPCIVendor)
{
	H264Context*			h = (H264Context*) pAVCtx->priv_data;
	SPS*					cur_sps;
	PPS*					cur_pps;
	int						field_pic_flag;
	HRESULT					hr = E_FAIL;
	const					Picture *current_picture = h->cur_pic_ptr;

	field_pic_flag = (h->picture_structure != PICT_FRAME);

	cur_sps	= &h->sps;
	cur_pps = &h->pps;

	if (cur_sps && cur_pps) {
		*nFieldType = current_picture->f.interlaced_frame ? PICT_TOP_FIELD : h->picture_structure;
		if (h->sps.pic_struct_present_flag) {
			switch (h->sei_pic_struct) {
				case SEI_PIC_STRUCT_TOP_FIELD:
				case SEI_PIC_STRUCT_TOP_BOTTOM:
				case SEI_PIC_STRUCT_TOP_BOTTOM_TOP:
					*nFieldType = PICT_TOP_FIELD;
					break;
				case SEI_PIC_STRUCT_BOTTOM_FIELD:
				case SEI_PIC_STRUCT_BOTTOM_TOP:
				case SEI_PIC_STRUCT_BOTTOM_TOP_BOTTOM:
					*nFieldType = PICT_BOTTOM_FIELD;
					break;
				case SEI_PIC_STRUCT_FRAME_DOUBLING:
				case SEI_PIC_STRUCT_FRAME_TRIPLING:
				case SEI_PIC_STRUCT_FRAME:
					*nFieldType = PICT_FRAME;
					break;
			}
		}

		*nSliceType = h->slice_type;

		if (cur_sps->mb_width == 0 || cur_sps->mb_height == 0) {
			return VFW_E_INVALID_FILE_FORMAT;
		}

		pDXVAPicParams->wFrameWidthInMbsMinus1					= cur_sps->mb_width  - 1;		// pic_width_in_mbs_minus1;
		pDXVAPicParams->wFrameHeightInMbsMinus1					= cur_sps->mb_height * (2 - cur_sps->frame_mbs_only_flag) - 1;		// pic_height_in_map_units_minus1;
		pDXVAPicParams->num_ref_frames							= cur_sps->ref_frame_count;		// num_ref_frames;
		pDXVAPicParams->field_pic_flag							= field_pic_flag;
		pDXVAPicParams->MbaffFrameFlag							= (h->sps.mb_aff && (field_pic_flag==0));
		pDXVAPicParams->residual_colour_transform_flag			= cur_sps->residual_color_transform_flag;
		pDXVAPicParams->sp_for_switch_flag						= h->sp_for_switch_flag;
		pDXVAPicParams->chroma_format_idc						= cur_sps->chroma_format_idc;
		pDXVAPicParams->RefPicFlag								= h->ref_pic_flag;
		pDXVAPicParams->constrained_intra_pred_flag				= cur_pps->constrained_intra_pred;
		pDXVAPicParams->weighted_pred_flag						= cur_pps->weighted_pred;
		pDXVAPicParams->weighted_bipred_idc						= cur_pps->weighted_bipred_idc;
		pDXVAPicParams->frame_mbs_only_flag						= cur_sps->frame_mbs_only_flag;
		pDXVAPicParams->transform_8x8_mode_flag					= cur_pps->transform_8x8_mode;
		pDXVAPicParams->MinLumaBipredSize8x8Flag				= h->sps.level_idc >= 31;
		pDXVAPicParams->IntraPicFlag							= (h->slice_type == AV_PICTURE_TYPE_I || h->slice_type == AV_PICTURE_TYPE_SI);

		pDXVAPicParams->bit_depth_luma_minus8					= cur_sps->bit_depth_luma   - 8;	// bit_depth_luma_minus8
		pDXVAPicParams->bit_depth_chroma_minus8					= cur_sps->bit_depth_chroma - 8;	// bit_depth_chroma_minus8

		//	pDXVAPicParams->StatusReportFeedbackNumber				= SET IN DecodeFrame;
		//	pDXVAPicParams->CurrFieldOrderCnt						= SET IN UpdateRefFramesList;
		//	pDXVAPicParams->FieldOrderCntList						= SET IN UpdateRefFramesList;
		//	pDXVAPicParams->FrameNumList							= SET IN UpdateRefFramesList;
		//	pDXVAPicParams->UsedForReferenceFlags					= SET IN UpdateRefFramesList;
		//	pDXVAPicParams->NonExistingFrameFlags

		pDXVAPicParams->frame_num								= h->frame_num;

		pDXVAPicParams->log2_max_frame_num_minus4				= cur_sps->log2_max_frame_num - 4;					// log2_max_frame_num_minus4;
		pDXVAPicParams->pic_order_cnt_type						= cur_sps->poc_type;								// pic_order_cnt_type;
		if (cur_sps->poc_type == 0)
			pDXVAPicParams->log2_max_pic_order_cnt_lsb_minus4	= cur_sps->log2_max_poc_lsb - 4;					// log2_max_pic_order_cnt_lsb_minus4;
		else if (cur_sps->poc_type == 1)
			pDXVAPicParams->delta_pic_order_always_zero_flag	= cur_sps->delta_pic_order_always_zero_flag;
		pDXVAPicParams->direct_8x8_inference_flag				= cur_sps->direct_8x8_inference_flag;
		pDXVAPicParams->entropy_coding_mode_flag				= cur_pps->cabac;									// entropy_coding_mode_flag;
		pDXVAPicParams->pic_order_present_flag					= cur_pps->pic_order_present;						// pic_order_present_flag;
		pDXVAPicParams->num_slice_groups_minus1					= cur_pps->slice_group_count - 1;					// num_slice_groups_minus1;
		pDXVAPicParams->slice_group_map_type					= cur_pps->mb_slice_group_map_type;					// slice_group_map_type;
		pDXVAPicParams->deblocking_filter_control_present_flag	= cur_pps->deblocking_filter_parameters_present;	// deblocking_filter_control_present_flag;
		pDXVAPicParams->redundant_pic_cnt_present_flag			= cur_pps->redundant_pic_cnt_present;				// redundant_pic_cnt_present_flag;

		pDXVAPicParams->chroma_qp_index_offset					= cur_pps->chroma_qp_index_offset[0];
		pDXVAPicParams->second_chroma_qp_index_offset			= cur_pps->chroma_qp_index_offset[1];
		pDXVAPicParams->num_ref_idx_l0_active_minus1			= cur_pps->ref_count[0]-1;							// num_ref_idx_l0_active_minus1;
		pDXVAPicParams->num_ref_idx_l1_active_minus1			= cur_pps->ref_count[1]-1;							// num_ref_idx_l1_active_minus1;
		pDXVAPicParams->pic_init_qp_minus26						= cur_pps->init_qp - 26;
		pDXVAPicParams->pic_init_qs_minus26						= cur_pps->init_qs - 26;

		pDXVAPicParams->CurrPic.AssociatedFlag = field_pic_flag && (h->picture_structure == PICT_BOTTOM_FIELD);
		pDXVAPicParams->CurrFieldOrderCnt[0] = 0;
		if ((h->picture_structure & PICT_TOP_FIELD) && current_picture->field_poc[0] != INT_MAX) {
			pDXVAPicParams->CurrFieldOrderCnt[0] = current_picture->field_poc[0];
		}
		pDXVAPicParams->CurrFieldOrderCnt[1] = 0;
		if ((h->picture_structure & PICT_BOTTOM_FIELD) && current_picture->field_poc[1] != INT_MAX) {
			pDXVAPicParams->CurrFieldOrderCnt[1] = current_picture->field_poc[1];
		}

		CopyScalingMatrix (pDXVAScalingMatrix, cur_pps, nPCIVendor);
		hr = S_OK;
	}

	return hr;
}

void FFH264SetCurrentPicture (int nIndex, DXVA_PicParams_H264* pDXVAPicParams, struct AVCodecContext* pAVCtx)
{
	H264Context* h = (H264Context*) pAVCtx->priv_data;

	pDXVAPicParams->CurrPic.Index7Bits		= nIndex;
	if (h->cur_pic_ptr) {
		h->cur_pic_ptr->f.opaque	= (void*)nIndex;
	}
}

void FFH264UpdateRefFramesList (DXVA_PicParams_H264* pDXVAPicParams, struct AVCodecContext* pAVCtx)
{
	H264Context*	h = (H264Context*) pAVCtx->priv_data;
	UINT			nUsedForReferenceFlags = 0;
	int				i, j;
	Picture*		pic;
	UCHAR			AssociatedFlag;

	for (i=0, j=0; i<16; i++) {
		if (i < h->short_ref_count) {
			// Short list reference frames
			pic				= h->short_ref[h->short_ref_count - i - 1];
			AssociatedFlag	= pic->long_ref != 0;
		} else {
			// Long list reference frames
			pic = NULL;
			while (!pic && j < h->short_ref_count + 16) {
				pic = h->long_ref[j++ - h->short_ref_count];
			}
			AssociatedFlag	= 1;
		}

		if (pic != NULL) {
			pDXVAPicParams->FrameNumList[i]					= pic->long_ref ? pic->pic_id : pic->frame_num;
			pDXVAPicParams->FieldOrderCntList[i][0]			= 0;
			pDXVAPicParams->FieldOrderCntList[i][1]			= 0;

			if (pic->field_poc[0] != INT_MAX) {
				pDXVAPicParams->FieldOrderCntList[i][0]		= pic->field_poc [0];
				nUsedForReferenceFlags						|= 1<<(i*2);
			}

			if (pic->field_poc[1] != INT_MAX) {
				pDXVAPicParams->FieldOrderCntList[i][1]		= pic->field_poc [1];
				nUsedForReferenceFlags						|= 2<<(i*2);
			}

			pDXVAPicParams->RefFrameList[i].AssociatedFlag	= AssociatedFlag;
			pDXVAPicParams->RefFrameList[i].Index7Bits		= (UCHAR)pic->f.opaque;
		} else {
			pDXVAPicParams->FrameNumList[i]					= 0;
			pDXVAPicParams->FieldOrderCntList[i][0]			= 0;
			pDXVAPicParams->FieldOrderCntList[i][1]			= 0;
			pDXVAPicParams->RefFrameList[i].AssociatedFlag	= 1;
			pDXVAPicParams->RefFrameList[i].Index7Bits		= 127;
		}
	}

	pDXVAPicParams->UsedForReferenceFlags = nUsedForReferenceFlags;
}

BOOL FFH264IsRefFrameInUse (int nFrameNum, struct AVCodecContext* pAVCtx)
{
	H264Context*	h = (H264Context*) pAVCtx->priv_data;
	int				i;

	for (i=0; i<h->short_ref_count; i++) {
		if ((int)h->short_ref[i]->f.opaque == nFrameNum) {
			return TRUE;
		}
	}

	for (i=0; i<h->long_ref_count; i++) {
		if ((int)h->long_ref[i]->f.opaque == nFrameNum) {
			return TRUE;
		}
	}

	return FALSE;
}

void FF264UpdateRefFrameSliceLong (DXVA_PicParams_H264* pDXVAPicParams, DXVA_Slice_H264_Long* pSlice, struct AVCodecContext* pAVCtx)
{
	H264Context*			h	= (H264Context*) pAVCtx->priv_data;
	HRESULT					hr	= E_FAIL;
	unsigned				i, list;

	for (list = 0; list < 2; list++) {
		for (i = 0; i < 32; i++) {
			pSlice->RefPicList[list][i].AssociatedFlag	= 1;
			pSlice->RefPicList[list][i].Index7Bits		= 127;
			pSlice->RefPicList[list][i].bPicEntry		= 255;
		}
	}

	for (list = 0; list < 2; list++) {
		for (i = 0; i < 32; i++) {
			if (list < h->list_count && i < h->ref_count[list]) {
				const Picture *r = &h->ref_list[list][i];
				pSlice->RefPicList[list][i].Index7Bits		= FFH264FindRefFrameIndex (h->ref_list[list][i].frame_num, pDXVAPicParams);
				pSlice->RefPicList[list][i].AssociatedFlag	= r->f.reference == PICT_BOTTOM_FIELD;
			}
		}
	}
}

void FFH264SetDxvaSliceLong (struct AVCodecContext* pAVCtx, void* pSliceLong)
{
	H264Context* h		= (H264Context*) pAVCtx->priv_data;
	h->dxva_slice_long	= pSliceLong;
}

HRESULT FFVC1UpdatePictureParam (DXVA_PictureParameters* pPicParams, struct AVCodecContext* pAVCtx, int* nFieldType, int* nSliceType, BYTE* pBuffer, UINT nSize, UINT* nFrameSize, BOOL b_SecondField, BOOL* b_repeat_pict)
{
	VC1Context* vc1		= (VC1Context*) pAVCtx->priv_data;
	int out_nFrameSize	= 0;

	if (pBuffer && !b_SecondField) {
		av_vc1_decode_frame (pAVCtx, pBuffer, nSize, &out_nFrameSize);
	}

	// WARNING : vc1->interlace is not reliable (always set for progressive video on HD-DVD material)
	if (vc1->fcm == 0) {
		if (nFieldType) {
			*nFieldType = PICT_FRAME;
		}
	} else {	// fcm : 2 or 3 frame or field interlaced
		if (nFieldType) {
			*nFieldType = (vc1->tff ? PICT_TOP_FIELD : PICT_BOTTOM_FIELD);
		}
	}

	if (b_SecondField) {
		vc1->second_field = 1;
		vc1->s.picture_structure = PICT_TOP_FIELD + vc1->tff;
		vc1->s.pict_type = (vc1->fptype & 1) ? AV_PICTURE_TYPE_P : AV_PICTURE_TYPE_I;
		if (vc1->fptype & 4)
			vc1->s.pict_type = (vc1->fptype & 1) ? AV_PICTURE_TYPE_BI : AV_PICTURE_TYPE_B;
	}

	if (nFrameSize) {
		*nFrameSize = out_nFrameSize;
	}

	if (vc1->profile == PROFILE_ADVANCED) {
		/* It is the cropped width/height -1 of the frame */
		pPicParams->wPicWidthInMBminus1	= pAVCtx->width  - 1;
		pPicParams->wPicHeightInMBminus1= pAVCtx->height - 1;
	} else {
		/* It is the coded width/height in macroblock -1 of the frame */
		pPicParams->wPicWidthInMBminus1 = vc1->s.mb_width  - 1;
		pPicParams->wPicHeightInMBminus1= vc1->s.mb_height - 1;
	}

	pPicParams->bSecondField			= (vc1->interlace && vc1->fcm == ILACE_FIELD && vc1->second_field);
	pPicParams->bPicIntra               = vc1->s.pict_type == AV_PICTURE_TYPE_I;
	pPicParams->bPicBackwardPrediction  = vc1->s.pict_type == AV_PICTURE_TYPE_B;


	// Init    Init    Init    Todo
	// iWMV9 - i9IRU - iOHIT - iINSO - iWMVA - 0 - 0 - 0		| Section 3.2.5
	pPicParams->bBidirectionalAveragingMode	= (pPicParams->bBidirectionalAveragingMode & 0xE0) |	// init in SetExtraData
										   ((vc1->lumshift!=0 || vc1->lumscale!=32) ? 0x10 : 0)| // iINSO
										   ((vc1->profile == PROFILE_ADVANCED)	 <<3 );			// iWMVA

	// Section 3.2.20.3
	pPicParams->bPicSpatialResid8	= (vc1->panscanflag   << 7) | (vc1->refdist_flag << 6) |
									  (vc1->s.loop_filter << 5) | (vc1->fastuvmc     << 4) |
									  (vc1->extended_mv   << 3) | (vc1->dquant       << 1) |
									  (vc1->vstransform);

	// Section 3.2.20.4
	pPicParams->bPicOverflowBlocks  = (vc1->quantizer_mode  << 6) | (vc1->multires << 5) |
									  (vc1->s.resync_marker << 4) | (vc1->rangered << 3) |
									  (vc1->s.max_b_frames);

	// Section 3.2.20.2
	pPicParams->bPicDeblockConfined	= (vc1->postprocflag << 7) | (vc1->broadcast  << 6) |
									  (vc1->interlace    << 5) | (vc1->tfcntrflag << 4) |
									  (vc1->finterpflag  << 3) | // (refpic << 2) set in DecodeFrame !
									  (vc1->psf << 1)		   | vc1->extended_dmv;


	pPicParams->bPicStructure = 0;
	if (vc1->s.picture_structure & PICT_TOP_FIELD) {
		pPicParams->bPicStructure |= 0x01;
	}
	if (vc1->s.picture_structure & PICT_BOTTOM_FIELD) {
		pPicParams->bPicStructure |= 0x02;
	}

	pPicParams->bMVprecisionAndChromaRelation =	((vc1->mv_mode == MV_PMODE_1MV_HPEL_BILIN) << 3) |
												  (1                                       << 2) |
												  (0                                       << 1) |
												  (!vc1->s.quarter_sample					   );

	// Cf page 17 : 2 for interlaced, 0 for progressive
	pPicParams->bPicExtrapolation = (!vc1->interlace || vc1->fcm == PROGRESSIVE) ? 1 : 2;

	if (vc1->s.picture_structure == PICT_FRAME) {
		pPicParams->wBitstreamFcodes        = vc1->lumscale;
		pPicParams->wBitstreamPCEelements   = vc1->lumshift;
	} else {
		/* Syntax: (top_field_param << 8) | bottom_field_param */
		pPicParams->wBitstreamFcodes        = (vc1->lumscale << 8) | vc1->lumscale;
		pPicParams->wBitstreamPCEelements   = (vc1->lumshift << 8) | vc1->lumshift;
	}

	if (vc1->profile == PROFILE_ADVANCED) {
		pPicParams->bPicOBMC = (vc1->range_mapy_flag  << 7) |
							   (vc1->range_mapy       << 4) |
							   (vc1->range_mapuv_flag << 3) |
							   (vc1->range_mapuv          );
	}

	// Section 3.2.16
	if (nSliceType) {
		*nSliceType = vc1->s.pict_type;
	}

	// Cf section 7.1.1.25 in VC1 specification, section 3.2.14.3 in DXVA spec
	pPicParams->bRcontrol	= vc1->rnd;

	pPicParams->bPicDeblocked	= ((vc1->profile == PROFILE_ADVANCED && vc1->overlap == 1 &&
									pPicParams->bPicBackwardPrediction == 0)				<< 6) |
								  ((vc1->profile != PROFILE_ADVANCED && vc1->rangeredfrm)	<< 5) |
								  (vc1->s.loop_filter										<< 1);

	if (vc1->profile == PROFILE_ADVANCED && (vc1->rff || vc1->rptfrm)) {
		*b_repeat_pict = TRUE;
	}

	return S_OK;
}

int	MPEG2CheckCompatibility (struct AVCodecContext* pAVCtx, struct AVFrame* pFrame)
{
	Mpeg1Context*	s1			= (Mpeg1Context*)pAVCtx->priv_data;
	MpegEncContext*	s			= (MpegEncContext*)&s1->mpeg_enc_ctx;

	int				got_picture	= 0;
	AVPacket		avpkt;
	av_init_packet(&avpkt);
	avpkt.data		= pAVCtx->extradata;
	avpkt.size		= pAVCtx->extradata_size;
	avpkt.flags		= AV_PKT_FLAG_KEY;
	int used_bytes	= avcodec_decode_video2(pAVCtx, pFrame, &got_picture, &avpkt);

	// restore codec_id value, it's can be changed to AV_CODEC_ID_MPEG1VIDEO on .wtv/.dvr-ms + StreamBufferSource (possible broken extradata)
	pAVCtx->codec_id = AV_CODEC_ID_MPEG2VIDEO;

	return (s->chroma_format<2);
}

HRESULT FFMpeg2DecodeFrame (DXVA_PictureParameters* pPicParams, DXVA_QmatrixData* pQMatrixData, DXVA_SliceInfo* pSliceInfo, int* nSliceCount,
							struct AVCodecContext* pAVCtx, struct AVFrame* pFrame, int* nNextCodecIndex, int* nFieldType, int* nSliceType, BYTE* pBuffer, UINT nSize,
							bool* bIsField, int* b_repeat_pict)
{
	HRESULT			hr = E_FAIL;
	int				i;
	Mpeg1Context*	s1			= (Mpeg1Context*)pAVCtx->priv_data;
	MpegEncContext*	s			= (MpegEncContext*)&s1->mpeg_enc_ctx;
	int				is_field	= 0;
	unsigned		mb_count	= 0;
	int				got_picture	= 0;

	if (pBuffer) {
		s1->pSliceInfo	= pSliceInfo;

		AVPacket		avpkt;
		av_init_packet(&avpkt);
		avpkt.data		= pBuffer;
		avpkt.size		= nSize;
		avpkt.flags		= AV_PKT_FLAG_KEY;
		int used_bytes	= avcodec_decode_video2(pAVCtx, pFrame, &got_picture, &avpkt);

#if defined(_DEBUG) && 0
		av_log(pAVCtx, AV_LOG_INFO, "FFMpeg2DecodeFrame() : %d, %d\n", used_bytes, got_picture);
#endif

		if (used_bytes < 0) {
			return hr;
		}

		hr				= S_OK;
		*nSliceCount	= s1->slice_count;
		*nFieldType		= s->progressive_frame ? PICT_FRAME : s->current_picture.f.top_field_first ? PICT_TOP_FIELD : PICT_BOTTOM_FIELD;
		*nSliceType		= s->pict_type;
	}

	// pPicParams->wDecodedPictureIndex;			set in DecodeFrame
	// pPicParams->wDeblockedPictureIndex;			0 for Mpeg2
	// pPicParams->wForwardRefPictureIndex;			set in DecodeFrame
	// pPicParams->wBackwardRefPictureIndex;		set in DecodeFrame

	is_field									= s->picture_structure != PICT_FRAME;
	if (bIsField) {
		*bIsField								= is_field;
	}

	if (b_repeat_pict) {
		*b_repeat_pict							= s->current_picture.f.repeat_pict;
	}

	pPicParams->wPicWidthInMBminus1				= s->mb_width-1;
	pPicParams->wPicHeightInMBminus1			= (s->mb_height >> is_field) - 1;

	pPicParams->bMacroblockWidthMinus1			= 15;	// This is equal to "15" for MPEG-1, MPEG-2, H.263, and MPEG-4
	pPicParams->bMacroblockHeightMinus1			= 15;	// This is equal to "15" for MPEG-1, MPEG-2, H.261, H.263, and MPEG-4

	pPicParams->bBlockWidthMinus1				= 7;	// This is equal to "7" for MPEG-1, MPEG-2, H.261, H.263, and MPEG-4
	pPicParams->bBlockHeightMinus1				= 7;	// This is equal to "7" for MPEG-1, MPEG-2, H.261, H.263, and MPEG-4

	pPicParams->bBPPminus1						= 7;	// It is equal to "7" for MPEG-1, MPEG-2, H.261, and H.263

	pPicParams->bPicStructure					= s->picture_structure;
	pPicParams->bSecondField					= is_field && !s->first_field;
	pPicParams->bPicIntra						= (s->current_picture.f.pict_type == AV_PICTURE_TYPE_I);
	pPicParams->bPicBackwardPrediction			= (s->current_picture.f.pict_type == AV_PICTURE_TYPE_B);

	pPicParams->bBidirectionalAveragingMode		= 0;	// The value "0" indicates MPEG-1 and MPEG-2 rounded averaging (//2),
	//pPicParams->bMVprecisionAndChromaRelation	= 0;	// Indicates that luminance motion vectors have half-sample precision and that chrominance motion vectors are derived from luminance motion vectors according to the rules in MPEG-2
	pPicParams->bChromaFormat					= 0x01;	// For MPEG-1, MPEG-2 "Main Profile," H.261 and H.263 bitstreams, this value shall always be set to "01", indicating "4:2:0" format

	// pPicParams->bPicScanFixed				= 1;	// set in UpdatePicParams
	// pPicParams->bPicScanMethod				= 1;	// set in UpdatePicParams
	// pPicParams->bPicReadbackRequests;				// ??

	// pPicParams->bRcontrol					= 0;	// It shall be set to "0" for all MPEG-1, and MPEG-2 bitstreams in order to conform with the rounding operator defined by those standards
	// pPicParams->bPicSpatialResid8;					// set in UpdatePicParams
	// pPicParams->bPicOverflowBlocks;					// set in UpdatePicParams
	// pPicParams->bPicExtrapolation;			= 0;	// by H.263 Annex D and MPEG-4

	// pPicParams->bPicDeblocked;				= 0;	// MPEG2_A Restricted Profile
	// pPicParams->bPicDeblockConfined;					// ??
	// pPicParams->bPic4MVallowed;						// See H.263 Annexes F and J
	// pPicParams->bPicOBMC;							// H.263 Annex F
	// pPicParams->bPicBinPB;							// Annexes G and M of H.263
	// pPicParams->bMV_RPS;								// ???
	// pPicParams->bReservedBits;						// ??

	pPicParams->wBitstreamFcodes				= (s->mpeg_f_code[0][0]<<12)  | (s->mpeg_f_code[0][1]<<8) |
												  (s->mpeg_f_code[1][0]<<4)   | (s->mpeg_f_code[1][1]);

	pPicParams->wBitstreamPCEelements			= (s->intra_dc_precision<<14) | (s->picture_structure<<12) |
												  (s->top_field_first<<11)  | (s->frame_pred_frame_dct<<10)|
												  (s->concealment_motion_vectors<<9) | (s->q_scale_type<<8)|
												  (s->intra_vlc_format<<7)    |      (s->alternate_scan<<6)|
												  (s->repeat_first_field<<5)  |     (s->chroma_420_type<<4)|
												  (s->progressive_frame<<3);

	pPicParams->bBitstreamConcealmentNeed		= 0;
	pPicParams->bBitstreamConcealmentMethod		= 0;

	pQMatrixData->bNewQmatrix[0] = 1;
	pQMatrixData->bNewQmatrix[1] = 1;
	pQMatrixData->bNewQmatrix[2] = 1;
	pQMatrixData->bNewQmatrix[3] = 1;
	for (i = 0; i < 64; i++) {
		int n = s->dsp.idct_permutation[ff_zigzag_direct[i]];
		pQMatrixData->Qmatrix[0][i] = s->intra_matrix[n];
		pQMatrixData->Qmatrix[1][i] = s->inter_matrix[n];
		pQMatrixData->Qmatrix[2][i] = s->chroma_intra_matrix[n];
		pQMatrixData->Qmatrix[3][i] = s->chroma_inter_matrix[n];
	}

	mb_count = s->mb_width * (s->mb_height >> is_field);
	for (i = 0; i < s1->slice_count; i++) {
		DXVA_SliceInfo *slice		= &s1->pSliceInfo[i];

		if (i < s1->slice_count - 1) {
			slice->wNumberMBsInSlice = slice[1].wNumberMBsInSlice - slice[0].wNumberMBsInSlice;
		} else {
			slice->wNumberMBsInSlice = mb_count - slice[0].wNumberMBsInSlice;
		}
	}

	if (got_picture) {
		*nNextCodecIndex = pFrame->coded_picture_number;
	}

	return S_OK;
}

unsigned long FFGetMBNumber (struct AVCodecContext* pAVCtx)
{
	MpegEncContext* s = GetMpegEncContext(pAVCtx);

	return (s != NULL) ? s->mb_num : 0;
}

int FFGetThreadType (enum AVCodecID nCodecId)
{
	switch (nCodecId)
	{
		case AV_CODEC_ID_H264			:
			return FF_THREAD_FRAME|FF_THREAD_SLICE;
			break;
		case AV_CODEC_ID_MPEG1VIDEO		:
		case AV_CODEC_ID_MPEG2VIDEO		:
		case AV_CODEC_ID_DVVIDEO		:
		case AV_CODEC_ID_FFV1			:
		case AV_CODEC_ID_PRORES			:
			return FF_THREAD_SLICE;
			break;
		case AV_CODEC_ID_VP3			:
		case AV_CODEC_ID_VP8			:
		case AV_CODEC_ID_THEORA			:
		case AV_CODEC_ID_RV30			:
		case AV_CODEC_ID_RV40			:
		case AV_CODEC_ID_UTVIDEO		:
		case AV_CODEC_ID_LAGARITH		:
		case AV_CODEC_ID_JPEG2000		:
			return FF_THREAD_FRAME;
			break;
		default :
			return 0;
	}
}

void FFSetThreadNumber (struct AVCodecContext* pAVCtx, enum AVCodecID nCodecId, int nThreadCount)
{
	pAVCtx->thread_count	= nThreadCount;
	pAVCtx->thread_type		= nThreadCount ? FFGetThreadType(nCodecId) : 0;
}

int FFGetCodedPicture (struct AVCodecContext* pAVCtx)
{
	MpegEncContext* s = GetMpegEncContext(pAVCtx);

	return (s != NULL) ? s->current_picture.f.coded_picture_number : 0;
}

BOOL FFGetAlternateScan (struct AVCodecContext* pAVCtx)
{
	MpegEncContext* s = GetMpegEncContext(pAVCtx);

	return (s != NULL) ? s->alternate_scan : 0;
}

void FFGetOutputSize (struct AVCodecContext* pAVCtx, AVFrame* pFrame, int* OutWidth, int* OutHeight)
{
	if (pAVCtx->codec_id == AV_CODEC_ID_H264) {
		H264Context*	h = (H264Context*) pAVCtx->priv_data;
		SPS*			cur_sps;
		if (pAVCtx->extradata_size) {
			int				got_picture	= 0;
			AVPacket		avpkt;
			av_init_packet(&avpkt);
			avpkt.data		= pAVCtx->extradata;
			avpkt.size		= pAVCtx->extradata_size;
			avpkt.flags		= AV_PKT_FLAG_KEY;
			int used_bytes	= avcodec_decode_video2(pAVCtx, pFrame, &got_picture, &avpkt);
		}

		cur_sps	= &h->sps;

		if (cur_sps) {
			if (OutWidth) {
				*OutWidth	= cur_sps->mb_width * 16;
			}
			if (OutHeight) {
				*OutHeight	= cur_sps->mb_height * (2 - cur_sps->frame_mbs_only_flag) * 16;
			}
		}
		return;
	}

	if (pAVCtx->codec_id == AV_CODEC_ID_MPEG2VIDEO) {
		Mpeg1Context*	s1	= (Mpeg1Context*)pAVCtx->priv_data;
		MpegEncContext*	s	= (MpegEncContext*)&s1->mpeg_enc_ctx;

		if (pAVCtx->extradata_size) {
			int				got_picture	= 0;
			AVPacket		avpkt;
			av_init_packet(&avpkt);
			avpkt.data		= pAVCtx->extradata;
			avpkt.size		= pAVCtx->extradata_size;
			avpkt.flags		= AV_PKT_FLAG_KEY;
			int used_bytes	= avcodec_decode_video2(pAVCtx, pFrame, &got_picture, &avpkt);
		}

		if (OutWidth) {
			*OutWidth	= FFALIGN(s->width, 16);
		}
		if (OutHeight) {
			*OutHeight	= FFALIGN(s->height, 16);
			if (!s->progressive_sequence) {
				*OutHeight = int((s->height + 31) / 32 * 2) * 16;
			}
		}
	}
}

BOOL DXVACheckFramesize (int width, int height, DWORD nPCIVendor, DWORD nPCIDevice)
{
	width = (width + 15) & ~15; // (width + 15) / 16 * 16;
	height = (height + 15) & ~15; // (height + 15) / 16 * 16;

	if (nPCIVendor == PCIV_nVidia) {
		if (CheckPCID((WORD)nPCIDevice, PCID_NVIDIA_VP5, _countof(PCID_NVIDIA_VP5)) && width <= 4096 && height <= 4096 && width * height <= 4080 * 4080) {
			// tested H.264 on VP5 (GT 610, GTX 660 Ti)
			// 4080x4080 = 65025 macroblocks
			return TRUE;
		} else if (width <= 2032 && height <= 2032 && width * height <= 8190 * 16 * 16) {
			// tested H.264, VC-1 and MPEG-2 on VP4 (feature set C) (G210M, GT220)
			return TRUE;
		}
	} else if (nPCIVendor == PCIV_ATI) {
		if (width <= 2048 && height <= 2304 && width * height <= 2048 * 2048) {
			// tested H.264 on UVD 2.2 (HD5670, HD5770, HD5850)
			// it may also work if width = 2064, but unstable
			return TRUE;
		}
	} else if (nPCIVendor == PCIV_Intel && nPCIDevice == PCID_Intel_HD4000) {
		//if (width <= 4096 && height <= 4096 && width * height <= 56672 * 16 * 16) {
		if (width <= 4096 && height <= 4096) { // driver v.9.17.10.2867
			// complete test was performed
			return TRUE;
		}
	} else if (nPCIVendor == PCIV_Intel && nPCIDevice == PCID_Intel_HD2500) {
		if (CHECK_AVC_L52_SIZE(width, height)) {
			// tested some media files with AVC Livel 5.1
			// complete test was NOT performed
			return TRUE;
		}
	} else if (width <= 1920 && height <= 1088) {
		return TRUE;
	}

	return FALSE;
}
