#include "h264_detail.h"

#include <vector>
#include <limits>

#include <vfwmsgs.h>

#define HAVE_AV_CONFIG_H
#define __STDC_CONSTANT_MACROS

#include "common/stdint.h"
#include "common/hardware_env.h"
#include "PODtypes.h"
#include "libavcodec/dsputil.h"
#include "libavcodec/avcodec.h"
#include "libavcodec/h264.h"
#include "ffmpeg.h"

namespace
{
int findRefFrameIndex(int frameCount, const DXVA_PicParams_H264* picParams)
{
    for (int i = 0; i < picParams->num_ref_frames; ++i)
        if (picParams->FrameNumList[i] == frameCount)
            return picParams->RefFrameList[i].Index7Bits;

    return 127;
}

const int8 ZZScan[16] =
{
    0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15
};

const int8 ZZScan8[64] =
{
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

void copyScalingMatrix(DXVA_Qmatrix_H264* dest, const DXVA_Qmatrix_H264* source)
{
    static int vendor = CHardwareEnv::get()->GetVideoCardVendor();
    if (CHardwareEnv::PCI_VENDOR_ATI == vendor)
    {
        memcpy(dest, source, sizeof(DXVA_Qmatrix_H264));
        return;
    }

    // The nVidia way(and other manufacturers compliant with specifications...).
    for (int i = 0; i < arraysize(dest->bScalingLists4x4); ++i)
        for (int j = 0; j < arraysize(dest->bScalingLists4x4[i]); ++j)
            dest->bScalingLists4x4[i][j] =
                source->bScalingLists4x4[i][ZZScan[j]];

    for (int i = 0; i < arraysize(dest->bScalingLists8x8); ++i)
        for (int j = 0; j < arraysize(dest->bScalingLists8x8[i]); ++j)
            dest->bScalingLists8x8[i][j] =
                source->bScalingLists8x8[i][ZZScan8[j]];
}
}

namespace h264_detail
{
void UpdateRefFrameSliceLong(const DXVA_PicParams_H264* picParams,
                             const CCodecContext* cont,
                             DXVA_Slice_H264_Long* slices)
{
    const H264Context* info =
        reinterpret_cast<const H264Context*>(cont->GetPrivateData());
    assert(info);
    if (!info)
        return;

    for (int i = 0; i < arraysize(slices->RefPicList[0]); ++i)
    {
        DXVA_PicEntry_H264& entry = slices->RefPicList[0][i];
        entry.AssociatedFlag = 1;
        entry.bPicEntry = 255;
        entry.Index7Bits = 127;
        entry.AssociatedFlag = 1;
        entry.bPicEntry = 255;
        entry.Index7Bits = 127;
    }

    if ((info->slice_type != FF_I_TYPE) && (info->slice_type != FF_SI_TYPE))
    {
        for (int i = 0; i < static_cast<int>(info->ref_count[0]); ++i)
        {
            DXVA_PicEntry_H264& entry = slices->RefPicList[0][i];
            entry.Index7Bits = findRefFrameIndex(info->ref_list[0][i].frame_num,
                                                 picParams);
            entry.AssociatedFlag = 0;

            if ((info->s.picture_structure != PICT_FRAME))
            {
                if((SEI_PIC_STRUCT_BOTTOM_FIELD == info->sei_pic_struct) ||
                    (SEI_PIC_STRUCT_TOP_BOTTOM == info->sei_pic_struct) ||
                    (SEI_PIC_STRUCT_TOP_BOTTOM_TOP == info->sei_pic_struct))
                    entry.AssociatedFlag = 1;
            }
        }
    }
    else
    {
        slices->num_ref_idx_l0_active_minus1 = 0;
    }

    if ((FF_B_TYPE == info->slice_type) || (FF_S_TYPE == info->slice_type) ||
        (FF_BI_TYPE == info->slice_type))
    {
        for (int i = 0; i < static_cast<int>(info->ref_count[1]); ++i)
        {
            DXVA_PicEntry_H264& entry = slices->RefPicList[1][i];
            entry.Index7Bits = findRefFrameIndex(info->ref_list[1][i].frame_num,
                                                 picParams);
            entry.AssociatedFlag = 0;

            if ((info->s.picture_structure != PICT_FRAME))
            {
                if((SEI_PIC_STRUCT_BOTTOM_FIELD == info->sei_pic_struct) ||
                    (SEI_PIC_STRUCT_TOP_BOTTOM == info->sei_pic_struct) ||
                    (SEI_PIC_STRUCT_TOP_BOTTOM_TOP == info->sei_pic_struct))
                    entry.AssociatedFlag = 1;
            }
        }
    }
    else
    {
        slices->num_ref_idx_l1_active_minus1 = 0;
    }


    if ((FF_I_TYPE == info->slice_type) || (FF_SI_TYPE == info->slice_type))
    {
        for (int i = 0; i < 16; ++i)
            slices->RefPicList[0][i].bPicEntry = 0xFF;
    }

    if ((FF_P_TYPE == info->slice_type) || (FF_I_TYPE == info->slice_type) ||
        (FF_SP_TYPE == info->slice_type) || (FF_SI_TYPE == info->slice_type))
    {
        for (int i = 0; i < 16; ++i)
            slices->RefPicList[1][i].bPicEntry = 0xFF;
    }
}

HRESULT BuildPicParams(const CCodecContext* cont,
                       DXVA_PicParams_H264* picParams, int* fieldType,
                       int* sliceType)
{
    assert(cont);
    assert(picParams);
    assert(scalingMatrix);
    assert(fieldType);
    assert(sliceType);

    const H264Context* info =
        reinterpret_cast<const H264Context*>(cont->GetPrivateData());
    assert(info);
    if (!info)
        return E_FAIL;

    const SPS* sps = &info->sps;
    const PPS* pps = &info->pps;
    if (!sps || !pps)
        return E_FAIL;

    if (!sps->mb_width || !sps->mb_height)
        return VFW_E_INVALID_FILE_FORMAT;

    *fieldType = info->s.picture_structure;
    if (info->sps.pic_struct_present_flag)
    {
        switch (info->sei_pic_struct)
        {
            case SEI_PIC_STRUCT_TOP_FIELD:
            case SEI_PIC_STRUCT_TOP_BOTTOM:
            case SEI_PIC_STRUCT_TOP_BOTTOM_TOP:
                *fieldType = PICT_TOP_FIELD;
                break;
            case SEI_PIC_STRUCT_BOTTOM_FIELD:
            case SEI_PIC_STRUCT_BOTTOM_TOP:
            case SEI_PIC_STRUCT_BOTTOM_TOP_BOTTOM:
                *fieldType = PICT_BOTTOM_FIELD;
                break;
            case SEI_PIC_STRUCT_FRAME_DOUBLING:
            case SEI_PIC_STRUCT_FRAME_TRIPLING:
            case SEI_PIC_STRUCT_FRAME:
                *fieldType = PICT_FRAME;
                break;
        }
    }

    *sliceType = info->slice_type;

    const int fieldPicFlag = (info->s.picture_structure != PICT_FRAME);
    picParams->wFrameWidthInMbsMinus1 =
        sps->mb_width - 1; // pic_width_in_mbs_minus1;

    // pic_height_in_map_units_minus1;
    picParams->wFrameHeightInMbsMinus1 =
        sps->mb_height * (2 - sps->frame_mbs_only_flag) - 1;
    picParams->num_ref_frames = sps->ref_frame_count; // num_ref_frames;
    picParams->field_pic_flag = fieldPicFlag;
    picParams->MbaffFrameFlag = (info->sps.mb_aff && (fieldPicFlag==0));
    picParams->residual_colour_transform_flag =
        sps->residual_color_transform_flag;
    picParams->sp_for_switch_flag = info->sp_for_switch_flag;
    picParams->chroma_format_idc = sps->chroma_format_idc;
    picParams->RefPicFlag = info->ref_pic_flag;
    picParams->constrained_intra_pred_flag = pps->constrained_intra_pred;
    picParams->weighted_pred_flag = pps->weighted_pred;
    picParams->weighted_bipred_idc = pps->weighted_bipred_idc;
    picParams->frame_mbs_only_flag = sps->frame_mbs_only_flag;
    picParams->transform_8x8_mode_flag = pps->transform_8x8_mode;
    picParams->MinLumaBipredSize8x8Flag = (info->sps.level_idc >= 31);
    picParams->IntraPicFlag = (FF_I_TYPE == info->slice_type);
    picParams->bit_depth_luma_minus8 =
        sps->bit_depth_luma - 8; // bit_depth_luma_minus8
    picParams->bit_depth_chroma_minus8 =
        sps->bit_depth_chroma - 8; // bit_depth_chroma_minus8
    picParams->frame_num = info->frame_num;
    picParams->log2_max_frame_num_minus4 =
        sps->log2_max_frame_num - 4; // log2_max_frame_num_minus4;
    picParams->pic_order_cnt_type = sps->poc_type; // pic_order_cnt_type;

    // log2_max_pic_order_cnt_lsb_minus4;
    picParams->log2_max_pic_order_cnt_lsb_minus4 = sps->log2_max_poc_lsb - 4;
    picParams->delta_pic_order_always_zero_flag =
        sps->delta_pic_order_always_zero_flag;
    picParams->direct_8x8_inference_flag = sps->direct_8x8_inference_flag;
    picParams->entropy_coding_mode_flag =
        pps->cabac; // entropy_coding_mode_flag;
    picParams->pic_order_present_flag =
        pps->pic_order_present; // pic_order_present_flag;
    picParams->num_slice_groups_minus1 =
        pps->slice_group_count - 1; // num_slice_groups_minus1;
    picParams->slice_group_map_type =
        pps->mb_slice_group_map_type; // slice_group_map_type;

    // deblocking_filter_control_present_flag;
    picParams->deblocking_filter_control_present_flag =
        pps->deblocking_filter_parameters_present;
    picParams->redundant_pic_cnt_present_flag =
        pps->redundant_pic_cnt_present; // redundant_pic_cnt_present_flag;
    picParams->slice_group_change_rate_minus1 =
        pps->slice_group_change_rate_minus1;

    picParams->chroma_qp_index_offset = pps->chroma_qp_index_offset[0];
    picParams->second_chroma_qp_index_offset = pps->chroma_qp_index_offset[1];
    picParams->num_ref_idx_l0_active_minus1 =
        pps->ref_count[0]-1; // num_ref_idx_l0_active_minus1;
    picParams->num_ref_idx_l1_active_minus1 =
        pps->ref_count[1]-1; // num_ref_idx_l1_active_minus1;
    picParams->pic_init_qp_minus26 = pps->init_qp - 26;
    picParams->pic_init_qs_minus26 = pps->init_qs - 26;

    if (fieldPicFlag)
    {
        picParams->CurrPic.AssociatedFlag =
            (PICT_BOTTOM_FIELD == info->s.picture_structure);

        if (picParams->CurrPic.AssociatedFlag)
        {
            // Bottom field
            picParams->CurrFieldOrderCnt[0] = 0;
            picParams->CurrFieldOrderCnt[1] = info->poc_lsb + info->poc_msb;
        }
        else
        {
            // Top field
            picParams->CurrFieldOrderCnt[0] = info->poc_lsb + info->poc_msb;
            picParams->CurrFieldOrderCnt[1] = 0;
        }
    }
    else
    {
        picParams->CurrPic.AssociatedFlag = 0;
        picParams->CurrFieldOrderCnt[0] = info->poc_lsb + info->poc_msb;
        picParams->CurrFieldOrderCnt[1] = info->poc_lsb + info->poc_msb;
    }

    return S_OK;
}

HRESULT BuildScalingMatrix(const CCodecContext* cont,
                           DXVA_Qmatrix_H264* scalingMatrix)
{
    assert(cont);
    assert(scalingMatrix);

    const H264Context* info =
        reinterpret_cast<const H264Context*>(cont->GetPrivateData());
    assert(info);
    if (!info)
        return E_FAIL;

    const PPS* pps = &info->pps;
    if (!pps)
        return E_FAIL;

    copyScalingMatrix(
        scalingMatrix,
        reinterpret_cast<const DXVA_Qmatrix_H264*>(pps->scaling_matrix4));
    return S_OK;
}

void SetCurrentPicIndex(int index, DXVA_PicParams_H264* picParams,
                        CCodecContext* cont)
{
    assert(picParams);
    assert(cont);

    picParams->CurrPic.Index7Bits = index;

    const H264Context* info =
        reinterpret_cast<const H264Context*>(cont->GetPrivateData());
    assert(info);
    if (!info)
        return;

    if (info->s.current_picture_ptr)
        info->s.current_picture_ptr->opaque =
            reinterpret_cast<void*>(static_cast<intptr_t>(index));
}

void UpdateRefFramesList(DXVA_PicParams_H264* picParams,
                         const CCodecContext* cont)
{
    const H264Context* info =
        reinterpret_cast<const H264Context*>(cont->GetPrivateData());
    uint32 usedForReferenceFlags = 0;
    for (int i = 0; i < 16; ++i)
    {
        int8 associatedFlag = 0;
        Picture* pic = NULL;
        if (i < info->short_ref_count)
        {
            // Short list reference frames
            pic = info->short_ref[info->short_ref_count - i - 1];
            associatedFlag = (pic->long_ref != 0) ? 1 : 0;
        }
        else if (i < info->long_ref_count)
        {
            // Long list reference frames
            const int index =
                info->short_ref_count + info->long_ref_count - i - 1;
            pic = info->short_ref[index];
            associatedFlag = 1;
        }

        if (pic)
        {
            picParams->FrameNumList[i] =
                pic->long_ref ? pic->pic_id : pic->frame_num;

            if (pic->field_poc[0] != std::numeric_limits<int>::max())
            {
                picParams->FieldOrderCntList[i][0] = pic->field_poc[0];
                usedForReferenceFlags |= (1 << (i * 2));
            }
            else
            {
                picParams->FieldOrderCntList[i][0] = 0;
            }

            if (pic->field_poc[1] != std::numeric_limits<int>::max())
            {
                picParams->FieldOrderCntList[i][1] = pic->field_poc[1];
                usedForReferenceFlags |= ( 2 << (i * 2));
            }
            else
            {
                picParams->FieldOrderCntList[i][1] = 0;
            }

            picParams->RefFrameList[i].AssociatedFlag = associatedFlag;
            picParams->RefFrameList[i].Index7Bits =
                reinterpret_cast<UCHAR>(pic->opaque);
        }
        else
        {
            picParams->FrameNumList[i] = 0;
            picParams->FieldOrderCntList[i][0] = 0;
            picParams->FieldOrderCntList[i][1] = 0;
            picParams->RefFrameList[i].AssociatedFlag = 1;
            picParams->RefFrameList[i].Index7Bits = 127;
        }
    }

    picParams->UsedForReferenceFlags = usedForReferenceFlags;
}
} // namespace h264_detail