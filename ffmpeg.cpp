#include "ffmpeg.h"

#define HAVE_AV_CONFIG_H
#define __STDC_CONSTANT_MACROS
#include "common/stdint.h"
#include <vector>

#include <boost/intrusive_ptr.hpp>
#include <streams.h>
#include <dvdmedia.h>

#include "common/guid_def.h"
#include "common/dshow_util.h"
#include "common/hardware_env.h"
#include "common/intrusive_ptr_helper.h"
#include "podtypes.h"
#include "libavcodec/dsputil.h"
#include "libavcodec/avcodec.h"
#include "libavcodec/h264.h"
#include "libswscale/swscale.h"

using boost::shared_ptr;
using boost::intrusive_ptr;

extern "C" int av_h264_decode_frame(void*, int*, int64*, const void*, int);
extern "C" void av_init_packet(AVPacket* p);

namespace
{
void releaseCodec(AVCodecContext* cont)
{
    if (cont)
    {
        avcodec_close(cont);
        av_free(cont);
    }
}

struct SupportedType
{
    const GUID& SubType;
    int FourCC;
};

const SupportedType supportedTypes[] = {
    { MEDIASUBTYPE_H264, '462H' },
    { MEDIASUBTYPE_h264, '462h' },
    { MEDIASUBTYPE_X264, '462X' },
    { MEDIASUBTYPE_x264, '462x' },
    { MEDIASUBTYPE_VSSH, 'HSSV' },
    { MEDIASUBTYPE_vssh, 'hssv' },
    { MEDIASUBTYPE_DAVC, 'CVAD' },
    { MEDIASUBTYPE_davc, 'cvad' },
    { MEDIASUBTYPE_PAVC, 'CVAP' },
    { MEDIASUBTYPE_pavc, 'cvap' },
    { MEDIASUBTYPE_AVC1, '1CVA' },
    { MEDIASUBTYPE_avc1, '1cva' },
    { MEDIASUBTYPE_H264_bis, '1cva' }
};

int getFourCCFromSubType(const GUID& subType)
{
    for (int i = 0; i < arraysize(supportedTypes); ++i)
        if (supportedTypes[i].SubType == subType)
            return supportedTypes[i].FourCC;

    return 0;
}

enum KYCbCrRGBMatrixCoefType
{
    YCBCR_RGB_COEFF_ITUR_BT601 = 0,
    YCBCR_RGB_COEFF_ITUR_BT709 = 1,
    YCBCR_RGB_COEFF_SMPTE240M = 2,
};

struct TYCbCr2RGBCoef
{
    double Kr;
    double Kg;
    double Kb;
    double ChrRange;
    double YMul;
    double VrMul;
    double UgMul;
    double VgMul;
    double UbMul;
    int YSub;
    int RGBAdd1;
    int RGBAdd3;
};

void initYCbCr2RGBCoef(TYCbCr2RGBCoef* c,
                       KYCbCrRGBMatrixCoefType iturBt,
                       int whiteCutoff, int blackCutoff,
                       int chromaCutoff, double RGBWhiteLevel,
                       double RGBBlackLevel)
{
    assert(c);
    if (YCBCR_RGB_COEFF_ITUR_BT601 == iturBt)
    {
        c->Kr = 0.299;
        c->Kg = 0.587;
        c->Kb = 0.114;
    }
    else if (YCBCR_RGB_COEFF_SMPTE240M == iturBt)
    {
        c->Kr = 0.2122;
        c->Kg = 0.7013;
        c->Kb = 0.0865;
    }
    else
    {
        c->Kr = 0.2125;
        c->Kg = 0.7154;
        c->Kb = 0.0721;
    }

    double inYRange = whiteCutoff - blackCutoff;
    c->ChrRange = 128 - chromaCutoff;

    double RGBRange =
        RGBWhiteLevel - RGBBlackLevel;
    c->YMul =RGBRange / inYRange;
    c->VrMul=(RGBRange / c->ChrRange) * (1.0 - c->Kr);
    c->UgMul=(RGBRange / c->ChrRange) * (1.0 - c->Kb) * c->Kb / c->Kg;
    c->VgMul=(RGBRange / c->ChrRange) * (1.0 - c->Kr) * c->Kr / c->Kg;
    c->UbMul=(RGBRange / c->ChrRange) * (1.0 - c->Kb);
    int sub = std::min(static_cast<int>(RGBBlackLevel), blackCutoff);
    c->YSub = blackCutoff - sub;
    c->RGBAdd1 = static_cast<int>(RGBBlackLevel) - sub;
    c->RGBAdd3 = (c->RGBAdd1 << 8) + (c->RGBAdd1 << 16) + c->RGBAdd1;
}

}

CSWScale::CSWScale()
    : m_cont()
    , m_width(0)
    , m_height(0)
    , m_outCsp(0)
{
}

CSWScale::~CSWScale()
{
}

bool CSWScale::Init(const CCodecContext& codec, IMediaSample* sample)
{
    if (m_cont)
        return true;
    
    AM_MEDIA_TYPE* m;
    if (FAILED(sample->GetMediaType(&m)) || !m)
        return false;

    BITMAPINFOHEADER header;
    if (!ExtractBitmapInfoFromMediaType(*m, &header))
        return false;

    m_width = header.biWidth;
    m_height = abs(header.biHeight);
    m_outCsp = (MEDIASUBTYPE_YV12 == m->subtype) ?
        (FF_CSP_420P | FF_CSP_FLAGS_YUV_ADJ) : FF_CSP_YUY2;
    DeleteMediaType(m);

    TYCbCr2RGBCoef coeffs;
    initYCbCr2RGBCoef(&coeffs, YCBCR_RGB_COEFF_ITUR_BT601, 0, 235, 16, 255.0,
                      0.0);
    int32 swscaleTable[7];
    SwsParams params = {0};

    const AVCodecContext* codecCont =
        const_cast<CCodecContext&>(codec).getCodecContext();
    if (codecCont->dsp_mask & CHardwareEnv::PROCESSOR_FEATURE_MMX)
        params.cpu |= SWS_CPU_CAPS_MMX | SWS_CPU_CAPS_MMX2;

    if (codecCont->dsp_mask & CHardwareEnv::PROCESSOR_FEATURE_3DNOW)
        params.cpu |= SWS_CPU_CAPS_3DNOW;

    params.methodLuma.method = SWS_POINT;
    params.methodChroma.method = SWS_POINT;

    swscaleTable[0] = static_cast<int32>(coeffs.VrMul * 65536 + 0.5);
    swscaleTable[1] = static_cast<int32>(coeffs.UbMul * 65536 + 0.5);
    swscaleTable[2] = static_cast<int32>(coeffs.UgMul * 65536 + 0.5);
    swscaleTable[3] = static_cast<int32>(coeffs.VgMul * 65536 + 0.5);
    swscaleTable[4] = static_cast<int32>(coeffs.YMul  * 65536 + 0.5);
    swscaleTable[5] = static_cast<int32>(coeffs.YSub * 65536);
    swscaleTable[6] = coeffs.RGBAdd1;

    m_cont.reset(
        sws_getContext(
            codecCont->width, codecCont->height,
            csp_ffdshow2mplayer(csp_lavc2ffdshow(codecCont->pix_fmt)),
            codecCont->width, codecCont->height,
            csp_ffdshow2mplayer(m_outCsp), &params,
            NULL, NULL, swscaleTable),
        sws_freeContext);
    return true;
}

bool CSWScale::Convert(const CVideoFrame& frame, void* buf)
{
    uint8* dst[4];
    stride_t srcStride[4];
    stride_t dstStride[4];

    const TcspInfo* outcspInfo = csp_getInfo(m_outCsp);
    const AVFrame* rawFrame = const_cast<CVideoFrame&>(frame).getFrame();
    for (int i = 0; i < 4; ++i)
    {
        srcStride[i] = static_cast<stride_t>(rawFrame->linesize[i]);
        dstStride[i] = m_width >> outcspInfo->shiftX[i];
        if (!i)
            dst[i] = reinterpret_cast<uint8*>(buf);
        else
            dst[i] = dst[i - 1] + dstStride[i - 1] *
                (m_height >> outcspInfo->shiftY[i - 1]);
    }

    int csp = m_outCsp;
    if (outcspInfo->id == FF_CSP_420P)
        csp_yuv_adj_to_plane(csp, outcspInfo, (m_height + 1) / 2 * 2,
                             (unsigned char**)dst, dstStride);
    else
        csp_yuv_adj_to_plane(csp,outcspInfo, m_height,
                             (unsigned char**)dst,dstStride);

    sws_scale_ordered(reinterpret_cast<SwsContext*>(m_cont.get()),
                      const_cast<uint8_t**>(rawFrame->data), srcStride, 0,
                      m_height, dst, dstStride);
    return true;
}

//------------------------------------------------------------------------------
CVideoFrame::CVideoFrame()
    : m_frame(avcodec_alloc_frame(), av_free)
    , m_isComplete(false)
{
}

CVideoFrame::~CVideoFrame()
{
}

bool CVideoFrame::GetTime(int64* start, int64* stop)
{
    if (start)
        *start = m_frame.get()->reordered_opaque;

    if (stop)
        *stop = m_frame.get()->reordered_opaque + 1;

    return true;
}

void CVideoFrame::SetTypeSpecificFlags(IMediaSample* sample)
{
    assert(sample);
    intrusive_ptr<IMediaSample2> sample2;
    HRESULT r = sample->QueryInterface(IID_IMediaSample2,
                                       reinterpret_cast<void**>(&sample2));
    if (FAILED(r))
        return;

    AM_SAMPLE2_PROPERTIES props;
    if (SUCCEEDED(sample2->GetProperties(sizeof(props),
                                         reinterpret_cast<BYTE*>(&props))))
    {
        props.dwTypeSpecificFlags &= ~0x7F;
        AVFrame* frame = m_frame.get();
        if (!frame->interlaced_frame)
            props.dwTypeSpecificFlags |= AM_VIDEO_FLAG_WEAVE;
        else
        {
            if(frame->top_field_first)
                props.dwTypeSpecificFlags |= AM_VIDEO_FLAG_FIELD1FIRST;
        }

        switch (frame->pict_type)
        {
            case FF_I_TYPE :
            case FF_SI_TYPE :
                props.dwTypeSpecificFlags |= AM_VIDEO_FLAG_I_SAMPLE;
                break;
            case FF_P_TYPE :
            case FF_SP_TYPE :
                props.dwTypeSpecificFlags |= AM_VIDEO_FLAG_P_SAMPLE;
                break;
            default :
                props.dwTypeSpecificFlags |= AM_VIDEO_FLAG_B_SAMPLE;
                break;
            }

        sample2->SetProperties(sizeof(props), (BYTE*)&props);
    }
}

inline AVFrame* CVideoFrame::getFrame()
{
    return reinterpret_cast<AVFrame*>(m_frame.get());
}

//------------------------------------------------------------------------------
void CCodecContext::ReviseTypeSpecFlags(int firstFieldType, int picType,
                                        DWORD* flags)
{
    assert(flags);

    if (PICT_FRAME == firstFieldType)
        *flags |= AM_VIDEO_FLAG_WEAVE;
    else if(PICT_TOP_FIELD == firstFieldType)
        *flags |= AM_VIDEO_FLAG_FIELD1FIRST;

    switch (picType)
    {
        case FF_I_TYPE:
        case FF_SI_TYPE:
            *flags |= AM_VIDEO_FLAG_I_SAMPLE;
            break;
        case FF_P_TYPE:
        case FF_SP_TYPE:
            *flags |= AM_VIDEO_FLAG_P_SAMPLE;
            break;
        default :
            *flags |= AM_VIDEO_FLAG_B_SAMPLE;
            break;
    }
}

CCodecContext::CCodecContext()
    : m_cont(avcodec_alloc_context(), releaseCodec)
    , m_extraData()
{
}

CCodecContext::~CCodecContext()
{
    if (m_cont->thread_count > 1)
    {
        avcodec_thread_free(m_cont.get());
        m_cont->thread_count = 1;
    }
}

bool CCodecContext::Init(AVCodec* c, const CMediaType& mediaType)
{
    BITMAPINFOHEADER header;
    if (!ExtractBitmapInfoFromMediaType(mediaType, &header))
        return false;

    AVCodecContext* cont = m_cont.get();
    cont->width = header.biWidth;
    cont->height = abs(header.biHeight);
    cont->codec_tag = header.biCompression;

    if (FORMAT_MPEG2Video == *mediaType.FormatType())
    {
        if (!header.biCompression)
        {
            cont->codec_tag = mediaType.Subtype()->Data1;
        }
        else if (('1cva' == cont->codec_tag) || ('1CVA' == cont->codec_tag))
        {
            MPEG2VIDEOINFO* m =
                reinterpret_cast<MPEG2VIDEOINFO*>(mediaType.Format());
            cont->nal_length_size = m->dwFlags;
        }
    }

    cont->codec_tag = getFourCCFromSubType(*mediaType.Subtype());
    cont->workaround_bugs = FF_BUG_AUTODETECT;
    cont->error_concealment = FF_EC_DEBLOCK | FF_EC_GUESS_MVS;
    cont->error_recognition = FF_ER_CAREFUL;
    cont->idct_algo = FF_IDCT_AUTO;
    cont->skip_loop_filter = AVDISCARD_DEFAULT;
    cont->dsp_mask = FF_MM_FORCE | CHardwareEnv::get()->GetProcessorFeatures();
    cont->postgain = 1.0f;
    cont->debug_mv = 0;
    cont->get_buffer = avcodec_default_get_buffer;
    cont->release_buffer = avcodec_default_release_buffer;
    cont->reget_buffer = avcodec_default_reget_buffer;
    cont->handle_user_data =
        reinterpret_cast<void (__cdecl*)(AVCodecContext*,const uint8_t *,int)>(
            handleUserData);

    allocExtraData(mediaType);
    if (avcodec_open(cont, c) < 0)
        return false;

    return true;
}

int CCodecContext::GetVideoLevel() const
{
    H264Context* info = reinterpret_cast<H264Context*>(m_cont->priv_data);
    if (!info)
        return -1;

    SPS* s = info->sps_buffers[0];
    if (s)
        return s->level_idc;

    return -1;
}

int CCodecContext::GetRefFrameCount() const
{
    H264Context* info = reinterpret_cast<H264Context*>(m_cont->priv_data);
    if (!info)
        return -1;

    SPS* s = info->sps_buffers[0];
    if (s)
        return s->ref_frame_count;

    return -1;
}

int CCodecContext::GetWidth() const
{
    return m_cont.get()->width;
}

int CCodecContext::GetHeight() const
{
    return m_cont.get()->height;
}

int CCodecContext::GetNALLength() const
{
    return m_cont.get()->nal_length_size;
}

bool CCodecContext::IsRefFrameInUse(int frameNum) const
{
    H264Context* info = reinterpret_cast<H264Context*>(m_cont->priv_data);
    if (!info)
        return false;

    for (int i = 0; i < info->short_ref_count; ++i)
        if (reinterpret_cast<intptr_t>(info->short_ref[i]->opaque) == frameNum)
            return true;

    for (int i = 0; i < info->long_ref_count; ++i)
        if (reinterpret_cast<intptr_t>(info->long_ref[i]->opaque) == frameNum)
            return true;

    return false;
}

void CCodecContext::SetThreadNumber(int n)
{
    if (n == m_cont->thread_count)
        return;

    if (m_cont->thread_count > 1)
    {
        avcodec_thread_free(m_cont.get());
        m_cont->thread_count = 1;
    }

    if (n > 1)
        avcodec_thread_init(m_cont.get(), n);
}

void CCodecContext::SetSliceLong(void* sliceLong)
{
    H264Context* info = reinterpret_cast<H264Context*>(m_cont->priv_data);
    if (info)
        info->dxva_slice_long = sliceLong;
}

void CCodecContext::UpdateTime(int64 start, int64 stop)
{
    m_cont.get()->reordered_opaque = start;
    m_cont.get()->reordered_opaque2 = stop;
}

void CCodecContext::PreDecodeBuffer(const void* data, int size, int* framePOC,
                                    int* outPOC, int64* startTime)
{
    assert(data);
    assert(framePOC);
    av_h264_decode_frame(m_cont.get(), outPOC, startTime, data, size);
    H264Context* info = reinterpret_cast<H264Context*>(m_cont->priv_data);
    if (info->s.current_picture_ptr)
        *framePOC = info->s.current_picture_ptr->field_poc[0];
}

const void* CCodecContext::GetPrivateData() const
{
    return m_cont.get()->priv_data;
}

int CCodecContext::Decode(CVideoFrame* frame, const void* buf, int size)
{
    assert(frame);
    frame->SetComplete(false);

    AVPacket packet;
    av_init_packet(&packet);
    packet.data = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(buf));
    packet.size = size;

    int frameFinished;
    int usedBytes = avcodec_decode_video2(
        reinterpret_cast<AVCodecContext*>(m_cont.get()), frame->getFrame(),
        &frameFinished, &packet);

    frame->SetComplete(frameFinished && frame->getFrame()->data[0]);
    return usedBytes;
}

void CCodecContext::FlushBuffers()
{
    avcodec_flush_buffers(m_cont.get());
}

void CCodecContext::handleUserData(AVCodecContext* c, const void* buf,
                                   int bufSize)
{
}

AVCodecContext* CCodecContext::getCodecContext()
{
    return m_cont.get();
}

void CCodecContext::allocExtraData(const CMediaType& mediaType)
{
    const void* data = NULL;
    int size = 0;

    if (FORMAT_VideoInfo == *mediaType.FormatType())
    {
        size = mediaType.FormatLength() - sizeof(VIDEOINFOHEADER);
        data = size ? mediaType.Format() + sizeof(VIDEOINFOHEADER) : NULL;
    }
    else if (FORMAT_VideoInfo2 == *mediaType.FormatType())
    {
        size = mediaType.FormatLength() - sizeof(VIDEOINFOHEADER2);
        data = size?mediaType.Format() + sizeof(VIDEOINFOHEADER2) : NULL;
    }
    else if (FORMAT_MPEGVideo == *mediaType.FormatType())
    {
        MPEG1VIDEOINFO* mpeg1info =
            reinterpret_cast<MPEG1VIDEOINFO*>(mediaType.Format());
        if (mpeg1info->cbSequenceHeader)
        {
            size = mpeg1info->cbSequenceHeader;
            data = mpeg1info->bSequenceHeader;
        }
    }
    else if (FORMAT_MPEG2Video == *mediaType.FormatType())
    {
        MPEG2VIDEOINFO* mpeg2info =
            reinterpret_cast<MPEG2VIDEOINFO*>(mediaType.Format());
        if (mpeg2info->cbSequenceHeader)
        {
            size = mpeg2info->cbSequenceHeader;
            data = reinterpret_cast<const void*>(mpeg2info->dwSequenceHeader);
        }
    }

    if (size)
    {
        m_cont.get()->extradata_size = size;
        m_extraData.reset(new int8[size + FF_INPUT_BUFFER_PADDING_SIZE]);
        m_cont.get()->extradata = reinterpret_cast<uint8_t*>(m_extraData.get());
        memcpy(m_extraData.get(), data, size);
        memset(m_extraData.get() + size, 0, FF_INPUT_BUFFER_PADDING_SIZE);
    }
}

//------------------------------------------------------------------------------
bool CFFMPEG::IsSubTypeSupported(const CMediaType& mediaType)
{
    for (int i = 0; i < arraysize(supportedTypes); ++i)
        if (supportedTypes[i].SubType == *mediaType.Subtype())
            return true;

    return false;
}

int CFFMPEG::GetInputBufferPaddingSize()
{
    return FF_INPUT_BUFFER_PADDING_SIZE;
}

shared_ptr<CCodecContext> CFFMPEG::CreateCodec(const CMediaType& mediaType)
{
    if (IsSubTypeSupported(mediaType))
    {
        AVCodec* c = avcodec_find_decoder(CODEC_ID_H264);
        if (!c)
            return shared_ptr<CCodecContext>();

        shared_ptr<CCodecContext> cont(new CCodecContext());
        if (!cont->Init(c, mediaType))
            return shared_ptr<CCodecContext>();

        return cont;
    }

    return shared_ptr<CCodecContext>();
}

CFFMPEG::CFFMPEG()
{
    // Initialize FFMPEG
    avcodec_init();
    avcodec_register_all();
    av_log_set_callback(logCallback);
}

CFFMPEG::~CFFMPEG()
{
}

void CFFMPEG::logCallback(void* p, int level, const char* format, va_list v)
{
    const int debugMessageSize = 1024;
    boost::scoped_array<char> buf(new char[debugMessageSize]);
    buf[0] = L'\0';
    _vsnprintf_s(buf.get(), debugMessageSize, debugMessageSize - 1, format, v);
    OutputDebugStringA(buf.get());
}