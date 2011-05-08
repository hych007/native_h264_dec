#ifndef _H264_DETAIL_H_
#define _H264_DETAIL_H_

#include <windows.h>
#include <dxva.h>

class CCodecContext;

namespace h264_detail
{
void UpdateRefFrameSliceLong(const DXVA_PicParams_H264* picParams,
                             const CCodecContext* cont,
                             DXVA_Slice_H264_Long* slices);
HRESULT BuildPicParams(const CCodecContext* cont,
                       DXVA_PicParams_H264* picParams, int* fieldType,
                       int* sliceType);
HRESULT BuildScalingMatrix(const CCodecContext* cont,
                           DXVA_Qmatrix_H264* scalingMatrix);
void SetCurrentPicIndex(int index, DXVA_PicParams_H264* picParams,
                        CCodecContext* cont);
void UpdateRefFramesList(DXVA_PicParams_H264* picParams,
                         const CCodecContext* cont);
}

#endif  // _H264_DETAIL_H_