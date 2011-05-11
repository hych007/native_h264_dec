#include "h264_decoder_filter.h"

#include <cassert>

#include <initguid.h>
#include <dvdmedia.h>

#include "ffmpeg.h"
#include "h264_decoder.h"
#include "chromium/base/win_util.h"
#include "common/dshow_util.h"
#include "common/hardware_env.h"
#include "common/intrusive_ptr_helper.h"
#include "utils.h"

using std::vector;
using boost::shared_ptr;
using boost::intrusive_ptr;

namespace
{
inline int getDecodeSurfacesCount()
{
    return (win_util::GetWinVersion() >= win_util::WINVERSION_VISTA) ? 22 : 16;
}

const wchar_t* outputPinName = L"CH264DecoderOutputPin";
const wchar_t* inputPinName = L"CH264DecoderInputPin";
}

CH264DecoderOutputPin::CH264DecoderOutputPin(CH264DecoderFilter* decoder,
                                             HRESULT* r)
    : CTransformOutputPin(outputPinName, decoder, r, outputPinName)
    , m_decoder(decoder)
    , m_DXVA1SurfCount(0)
    , m_DXVA1DecoderID(GUID_NULL)
    , m_uncompPixelFormat()
{
    memset(&m_uncompPixelFormat, 0, sizeof(m_uncompPixelFormat));
}

CH264DecoderOutputPin::~CH264DecoderOutputPin()
{
}

HRESULT CH264DecoderOutputPin::NonDelegatingQueryInterface(const IID& ID,
                                                             void** o)
{
    if (!o)
        return E_POINTER;

    if (IID_IAMVideoAcceleratorNotify == ID)
    {
        IAMVideoAcceleratorNotify* i = this;
        i->AddRef();
        *o = i;
        return S_OK;
    }

    return CTransformOutputPin::NonDelegatingQueryInterface(ID, o);
}

HRESULT CH264DecoderOutputPin::GetUncompSurfacesInfo(
    const GUID* profileID, AMVAUncompBufferInfo* uncompBufInfo)
{
    HRESULT r = E_INVALIDARG;
    if (m_decoder->IsFormatSupported(*profileID))
    {
        intrusive_ptr<IAMVideoAccelerator> accel;
        IPin* connected = GetConnected();
        if (!connected)
            return E_UNEXPECTED;

        r = connected->QueryInterface(IID_IAMVideoAccelerator,
                                      reinterpret_cast<void**>(&accel));
        if (SUCCEEDED(r) && accel)
        {
            const int surfCount = getDecodeSurfacesCount();
            uncompBufInfo->dwMaxNumSurfaces = surfCount;
            uncompBufInfo->dwMinNumSurfaces = surfCount;
            r = m_decoder->ConfirmDXVA1UncompFormat(
                accel.get(), profileID,
                &uncompBufInfo->ddUncompPixelFormat);
            if (SUCCEEDED(r))
            {
                memcpy(&m_uncompPixelFormat,
                       &uncompBufInfo->ddUncompPixelFormat,
                       sizeof(m_uncompPixelFormat));
                m_DXVA1DecoderID = *profileID;
            }
        }
    }

    return r;
}

HRESULT CH264DecoderOutputPin::SetUncompSurfacesInfo(
    DWORD actualUncompSurfacesAllocated)
{
    m_DXVA1SurfCount = actualUncompSurfacesAllocated;
    return S_OK;
}

#define DXVA_RESTRICTED_MODE_H264_E             0x68
HRESULT CH264DecoderOutputPin::GetCreateVideoAcceleratorData(
    const GUID* profileID, DWORD* miscDataSize, void** miscData)
{
    IPin* connected = GetConnected();
    if (!connected)
        return E_UNEXPECTED;

    intrusive_ptr<IAMVideoAccelerator> accel;
    HRESULT r = connected->QueryInterface(IID_IAMVideoAccelerator,
                                          reinterpret_cast<void**>(&accel));
    if (FAILED(r))
        return r;

    AMVAUncompDataInfo uncompDataInfo;
    memcpy(&uncompDataInfo.ddUncompPixelFormat, &m_uncompPixelFormat,
           sizeof(m_uncompPixelFormat));
    uncompDataInfo.dwUncompWidth = 720;
    uncompDataInfo.dwUncompHeight = 480;

    AMVACompBufferInfo compInfo[30];
    DWORD numTypesCompBuffers = arraysize(compInfo);
    r = accel->GetCompBufferInfo(&m_DXVA1DecoderID, &uncompDataInfo,
                                 &numTypesCompBuffers, compInfo);
    if (FAILED(r))
        return r;

    r = m_decoder->ActivateDXVA1(accel.get(), profileID, uncompDataInfo,
                                 m_DXVA1SurfCount);
    if (SUCCEEDED(r))
    {
        m_decoder->SetDXVA1PixelFormat(m_uncompPixelFormat);
        DXVA_ConnectMode* connectMode =
            reinterpret_cast<DXVA_ConnectMode*>(
                CoTaskMemAlloc(sizeof(DXVA_ConnectMode)));
        connectMode->guidMode = m_DXVA1DecoderID;
        connectMode->wRestrictedMode = DXVA_RESTRICTED_MODE_H264_E;
        *miscDataSize = sizeof(*connectMode);
        *miscData = connectMode;
    }

    return r;
}

//------------------------------------------------------------------------------
namespace
{
struct { const GUID& SubType; int16 PlaneCount; int16 BitCount; int FourCC; }
    supportedFormats[] =
{
    // Hardware formats
    { DXVA_ModeH264_E, 1, 12, MAKEFOURCC('d','x','v','a') },
    { DXVA_ModeH264_F, 1, 12, MAKEFOURCC('d','x','v','a') },

    // Software formats
    { MEDIASUBTYPE_YV12, 3, 12, MAKEFOURCC('Y','V','1','2') },
    { MEDIASUBTYPE_YUY2, 1, 16, MAKEFOURCC('Y','U','Y','2') }
};

enum KDXVAH264Compatibility
{
    DXVA_UNSUPPORTED_LEVEL = 1,
    DXVA_TOO_MUCH_REF_FRAMES = 2,
    DXVA_INCOMPATIBLE_SAR = 4
};

bool hasDriverVersionReached(LARGE_INTEGER version, int a, int b, int c, int d)
{
    if (HIWORD(version.HighPart) > a)
        return true;
    
    if (HIWORD(version.HighPart) == a)
    {
        if (LOWORD(version.HighPart) > b)
            return true;
        
        if (LOWORD(version.HighPart) == b)
        {
            if (HIWORD(version.LowPart) > c)
                return true;

            if (HIWORD(version.LowPart) == c)
                if (LOWORD(version.LowPart) >= d)
                    return true;
        }
    }

    return false;
}

int checkHWCompatibilityForH264(int width, int height, int videoLevel,
                                int refFrameCount)
{
    int noLevel51Support = 1;
    int tooMuchRefFrames = 0;
    int maxRefFrames = 0;
    if (videoLevel >= 0)
    {
        int vendor = CHardwareEnv::get()->GetVideoCardVendor();
        int device = CHardwareEnv::get()->GetVideoCardDeviceID();
        LARGE_INTEGER videoDriverVersion;
        videoDriverVersion.QuadPart =
            CHardwareEnv::get()->GetVideoCardDriverVersion();

        const int maxRefFramesDPB41 = std::min(11, 8388608 / (width * height));
        maxRefFrames = maxRefFramesDPB41; // default value is calculate
        if (CHardwareEnv::PCI_VENDOR_NVIDIA == vendor)
        {
            // nVidia cards support level 5.1 since drivers v6.14.11.7800 for
            // XP and drivers v7.15.11.7800 for Vista/7
            if (win_util::GetWinVersion() >= win_util::WINVERSION_VISTA)
            {
                if (hasDriverVersionReached(videoDriverVersion, 7, 15, 11,
                                            7800))
                {
                    noLevel51Support = 0;

                    // max ref frames is 16 for HD and 11 otherwise
                    if (width >= 1280)
                        maxRefFrames = 16;
                    else
                        maxRefFrames = 11;
                }
            }
            else
            {
                if (hasDriverVersionReached(videoDriverVersion, 6, 14, 11,
                                            7800))
                {
                    noLevel51Support = 0;

                    // max ref frames is 14
                    maxRefFrames = 14;
                }
            }
        }
        else if (CHardwareEnv::PCI_VENDOR_S3_GRAPHICS == vendor)
        {
            noLevel51Support = 0;
        }
        else if (CHardwareEnv::PCI_VENDOR_ATI == vendor)
        {
            // HD4xxx and HD5xxx ATI cards support level 5.1 since drivers
            // v8.14.1.6105 (Catalyst 10.4)
            if ((0x68 == (device >> 8)) || (0x94 == (device >> 8)))
            {
                if (hasDriverVersionReached(videoDriverVersion, 8, 14, 1, 6105))
                {
                    noLevel51Support = 0;
                    maxRefFrames = 16;
                }
            }
        }

        // Check maximum allowed number reference frames.
        if (refFrameCount > maxRefFrames)
            tooMuchRefFrames = 1;
    }

    int hasVideoLevelReached51 = (videoLevel >= 51) ? 1 : 0;
    return (hasVideoLevelReached51 * noLevel51Support *
        DXVA_UNSUPPORTED_LEVEL) + (tooMuchRefFrames * DXVA_TOO_MUCH_REF_FRAMES);
}
}

CUnknown* CH264DecoderFilter::CreateInstance(IUnknown* aggregator, HRESULT *r)
{
    return new CH264DecoderFilter(aggregator, r);
}

CH264DecoderFilter::~CH264DecoderFilter()
{
}

HRESULT CH264DecoderFilter::CheckInputType(const CMediaType* inputType)
{
    if (!inputType)
        return E_POINTER;

    if (MEDIATYPE_Video != *inputType->Type())
        return VFW_E_TYPE_NOT_ACCEPTED;

    if (CFFMPEG::get()->IsSubTypeSupported(*inputType))
        return S_OK;

    return VFW_E_TYPE_NOT_ACCEPTED;
}

HRESULT CH264DecoderFilter::CheckTransform(const CMediaType* inputType, 
                                           const CMediaType* outputType)
{
    HRESULT r = CheckInputType(inputType);
    if (FAILED(r))
        return r;

    if (!outputType)
        return E_POINTER;

    if (MEDIATYPE_Video != *outputType->Type())
        return VFW_E_TYPE_NOT_ACCEPTED;

    if ((MEDIASUBTYPE_YV12 == *inputType->Subtype()) ||
        (MEDIASUBTYPE_I420 == *inputType->Subtype()) ||
        (MEDIASUBTYPE_IYUV == *inputType->Subtype()))
    {
        if ((MEDIASUBTYPE_YV12 != *outputType->Subtype()) &&
            (MEDIASUBTYPE_I420 != *outputType->Subtype()) &&
            (MEDIASUBTYPE_IYUV != *outputType->Subtype()) &&
            (MEDIASUBTYPE_YUY2 != *outputType->Subtype()))
            return VFW_E_TYPE_NOT_ACCEPTED;
    }
    else if (MEDIASUBTYPE_YUY2 == *inputType->Subtype())
    {
        if (MEDIASUBTYPE_YUY2 != *outputType->Subtype())
            return VFW_E_TYPE_NOT_ACCEPTED;
    }

    return S_OK;
}

HRESULT CH264DecoderFilter::DecideBufferSize(IMemAllocator * allocator, 
                                             ALLOCATOR_PROPERTIES* prop)
{
    BITMAPINFOHEADER header;
    if (!ExtractBitmapInfoFromMediaType(m_pOutput->CurrentMediaType(), &header))
        return E_FAIL;

    if (!prop)
        return E_POINTER;

    ALLOCATOR_PROPERTIES requested = *prop;
    if (requested.cbAlign < 1) 
        requested.cbAlign = 1;

    if (requested.cBuffers < 1) 
        requested.cBuffers = 1;

    requested.cbBuffer = header.biSizeImage;
    requested.cbPrefix = 0;

    ALLOCATOR_PROPERTIES actual;
    HRESULT r = allocator->SetProperties(&requested, &actual);
    if (FAILED(r)) 
        return r;

    return (requested.cBuffers > actual.cBuffers) ||
        (requested.cbBuffer > actual.cbBuffer) ? E_FAIL : S_OK;
}

HRESULT CH264DecoderFilter::GetMediaType(int position, CMediaType* mediaType)
{
    if (position < 0)
        return E_INVALIDARG;

    if (!mediaType)
        return E_POINTER;

    if (position >= static_cast<int>(m_mediaTypes.size()))
        return VFW_S_NO_MORE_ITEMS;

    *mediaType = *m_mediaTypes[position];
    return S_OK;
}

HRESULT CH264DecoderFilter::SetMediaType(PIN_DIRECTION dir,
                                         const CMediaType* mediaType)
{
    if (PINDIR_INPUT == dir)
    {
        if (!mediaType)
            return E_POINTER;

        // Rebuild output media types.
        m_mediaTypes.clear();

        // Get dimension info.
        int width;
        int height;
        int aspectX;
        int aspectY;
        if (!ExtractDimensionFromMediaType(*mediaType, &width, &height,
                                           &aspectX, &aspectY))
            return VFW_E_TYPE_NOT_ACCEPTED;

        // Get bitmap info.
        BITMAPINFOHEADER bitmapHeader;
        if (!ExtractBitmapInfoFromMediaType(*mediaType, &bitmapHeader))
            return VFW_E_TYPE_NOT_ACCEPTED;

        bitmapHeader.biWidth = width;
        bitmapHeader.biHeight = height;
        bitmapHeader.biSizeImage =
            bitmapHeader.biWidth * bitmapHeader.biHeight *
            bitmapHeader.biBitCount >> 3;

        VIDEOINFOHEADER* inputFormat =
            reinterpret_cast<VIDEOINFOHEADER*>(mediaType->Format());
        if (!inputFormat)
            return E_UNEXPECTED;

        m_averageTimePerFrame = inputFormat->AvgTimePerFrame;

        // Type 1: FORMAT_VideoInfo
        VIDEOINFOHEADER header = {0};
        header.bmiHeader = bitmapHeader;
        header.bmiHeader.biXPelsPerMeter = width * aspectY;
        header.bmiHeader.biYPelsPerMeter = height * aspectX;
        header.AvgTimePerFrame = inputFormat->AvgTimePerFrame;
        header.dwBitRate = inputFormat->dwBitRate;
        header.dwBitErrorRate = inputFormat->dwBitErrorRate;

        // Type 2: FORMAT_VideoInfo2
        VIDEOINFOHEADER2 header2 = {0};
        header2.bmiHeader = bitmapHeader;
        header2.dwPictAspectRatioX = aspectX;
        header2.dwPictAspectRatioY = aspectY;
        header2.dwInterlaceFlags =
            AMINTERLACE_IsInterlaced | AMINTERLACE_DisplayModeBobOrWeave;
        header2.AvgTimePerFrame = inputFormat->AvgTimePerFrame;
        header2.dwBitRate = inputFormat->dwBitRate;
        header2.dwBitErrorRate = inputFormat->dwBitErrorRate;

        // Copy source and target rectangles from input pin.
        if (inputFormat->rcSource.right && inputFormat->rcSource.bottom)
        {
            header.rcSource = inputFormat->rcSource;
            header.rcTarget = inputFormat->rcTarget;
            header2.rcSource = inputFormat->rcSource;
            header2.rcTarget = inputFormat->rcTarget;
        }
        else
        {
            header.rcSource.right = width;
            header.rcTarget.right = width;
            header.rcSource.bottom = height;
            header.rcTarget.bottom = height;
            header2.rcSource.right = width;
            header2.rcTarget.right = width;
            header2.rcSource.bottom = height;
            header2.rcTarget.bottom = height;
        }

        for (int i = 0; i < arraysize(supportedFormats); ++i)
        {
            shared_ptr<CMediaType> myType(new CMediaType);
            myType->SetType(&MEDIATYPE_Video);
            myType->SetSubtype(&supportedFormats[i].SubType);
            myType->SetFormatType(&FORMAT_VideoInfo);

            header.bmiHeader.biBitCount = supportedFormats[i].BitCount;
            header.bmiHeader.biPlanes = supportedFormats[i].PlaneCount;
            header.bmiHeader.biCompression = supportedFormats[i].FourCC;
            myType->SetFormat(reinterpret_cast<BYTE*>(&header), sizeof(header));

            m_mediaTypes.push_back(myType);

            shared_ptr<CMediaType> myType2(new CMediaType(*myType));
            myType2->SetFormatType(&FORMAT_VideoInfo2);

            header2.bmiHeader.biBitCount = supportedFormats[i].BitCount;
            header2.bmiHeader.biPlanes = supportedFormats[i].PlaneCount;
            header2.bmiHeader.biCompression = supportedFormats[i].FourCC;
            myType2->SetFormat(reinterpret_cast<BYTE*>(&header2),
                               sizeof(header2));

            m_mediaTypes.push_back(myType2);
        }
    }

    return S_OK;
}

HRESULT CH264DecoderFilter::CompleteConnect(PIN_DIRECTION dir, IPin* receivePin)
{
    if (PINDIR_INPUT == dir)
    {
        m_preDecode = CFFMPEG::get()->CreateCodec(m_pInput->CurrentMediaType());
        if (!m_preDecode)
            return VFW_E_TYPE_NOT_ACCEPTED;
    }
    else if (PINDIR_OUTPUT == dir)
    {
        if (m_decoder) // DXVA1 has been activated.
        {
            if (!m_decoder->Init(m_pixelFormat, m_averageTimePerFrame))
                m_decoder.reset();
        }
        
        if (!m_decoder) // Not support DXVA1.
            m_decoder.reset(new CH264SWDecoder(m_preDecode.get()));
    }

    return CTransformFilter::CompleteConnect(dir, receivePin);
}

HRESULT CH264DecoderFilter::BreakConnect(PIN_DIRECTION dir)
{
    if (PINDIR_INPUT == dir)
    {
        m_decoder.reset();
        m_preDecode.reset();
    }

    return S_OK;
}

HRESULT CH264DecoderFilter::NewSegment(REFERENCE_TIME start,
                                       REFERENCE_TIME stop, double rate)
{
    {
        AutoLock lock(m_decodeAccess);
        m_preDecode->FlushBuffers();
        if (m_decoder)
            m_decoder->Flush();
    }

    return CTransformFilter::NewSegment(start, stop, rate);
}

HRESULT CH264DecoderFilter::Receive(IMediaSample* inSample)
{
    AM_SAMPLE2_PROPERTIES* const props = m_pInput->SampleProps();
    if (props->dwStreamId != AM_STREAM_MEDIA)
        return m_pOutput->Deliver(inSample);

    assert(m_decoder);
    if (!m_decoder)
        return E_UNEXPECTED;

    BYTE* data;
    HRESULT r = inSample->GetPointer(&data);
    if (FAILED(r))
        return r;

    const int dataLength = inSample->GetActualDataLength();
    const int minDataSize = dataLength + CFFMPEG::GetInputBufferPaddingSize();
    if (inSample->GetSize() < minDataSize)
    {
        assert(false);
        // Reconfigure sample size.
    }

    // Make sure the padding bytes are initialized to 0.
    memset(data + dataLength, 0, CFFMPEG::GetInputBufferPaddingSize());

    REFERENCE_TIME start;
    REFERENCE_TIME stop;
    r = inSample->GetTime(&start, &stop);
    if (FAILED(r))
        return r;

    if ((stop <= start) && (stop != std::numeric_limits<int64>::min()))
        stop = start + m_averageTimePerFrame;

    m_preDecode->UpdateTime(start, stop);

    const int8* dataStart = reinterpret_cast<const int8*>(data);
    int dataRemaining = dataLength;
    while (dataRemaining > 0)
    {
        intrusive_ptr<IMediaSample> outSample;
        r = InitializeOutputSample(
            inSample, reinterpret_cast<IMediaSample**>(&outSample));
        if (FAILED(r))
            return r;

        int usedBytes = 0;
        {
            AutoLock lock(m_decodeAccess);
            r = m_decoder->Decode(dataStart, dataRemaining, start, stop,
                outSample.get(), &usedBytes);
            if (S_FALSE == r)
                return S_OK;

            if (FAILED(r))
                return r;

            r = m_decoder->DisplayNextFrame(outSample.get());
        }
        if (E_NOTIMPL == r)
            r = m_pOutput->Deliver(outSample.get());

        if (FAILED(r))
            return r;

        dataRemaining -= usedBytes;
        dataStart += usedBytes;
    }

    return r;
}

HRESULT CH264DecoderFilter::ActivateDXVA1(IAMVideoAccelerator* accel,
                                          const GUID* decoderID,
                                          const AMVAUncompDataInfo& uncompInfo,
                                          int surfaceCount)
{
    if (!m_preDecode)
        return E_FAIL;

    if (!accel || !decoderID)
        return E_POINTER;

    if (m_decoder && (m_decoder->GetDecoderID() == *decoderID))
        return S_OK;

    m_decoder.reset();
    int campatible = checkHWCompatibilityForH264(
        m_preDecode->GetWidth(), m_preDecode->GetHeight(),
        m_preDecode->GetVideoLevel(), m_preDecode->GetRefFrameCount());
    if (DXVA_UNSUPPORTED_LEVEL == campatible)
        return E_FAIL;

    m_decoder.reset(new CH264DXVA1Decoder(*decoderID, m_preDecode.get(), accel,
                                          surfaceCount));
    return S_OK;
}

bool CH264DecoderFilter::IsFormatSupported(const GUID& formatID)
{
    for (int i = 0; i < arraysize(supportedFormats); ++i)
        if (formatID == supportedFormats[i].SubType)
            return true;

    return false;
}

HRESULT CH264DecoderFilter::ConfirmDXVA1UncompFormat(IAMVideoAccelerator* accel,
                                                     const GUID* decoderID,
    DDPIXELFORMAT* pixelFormat)
{
    assert(accel);
    assert(pixelFormat);
    DWORD formatCount = 0;
    HRESULT r = accel->GetUncompFormatsSupported(decoderID, &formatCount, NULL);
    if (FAILED(r))
        return r;

    if (formatCount < 0)
        return E_FAIL;

    scoped_array<DDPIXELFORMAT> formats(new DDPIXELFORMAT[formatCount]);
    r = accel->GetUncompFormatsSupported(decoderID, &formatCount,
        formats.get());
    if (FAILED(r))
        return r;

    for (DWORD i = 0; i < formatCount; ++i)
    {
        if (formats[i].dwFourCC != MAKEFOURCC('N', 'V', '1', '2'))
            continue;

        memcpy(pixelFormat, &formats[i], sizeof(*pixelFormat));
        return S_OK;
    }

    return E_FAIL;
}

void CH264DecoderFilter::SetDXVA1PixelFormat(const DDPIXELFORMAT& pixelFormat)
{
    memcpy(&m_pixelFormat, &pixelFormat, sizeof(m_pixelFormat));
}

CH264DecoderFilter::CH264DecoderFilter(IUnknown* aggregator, HRESULT* r)
    : CTransformFilter(L"H264DecodeFilter", aggregator, CLSID_NULL)
    , m_mediaTypes()
    , m_preDecode()
    , m_pixelFormat()
    , m_decodeAccess()
    , m_decoder()
    , m_averageTimePerFrame(1)
{
    memset(&m_pixelFormat, 0, sizeof(m_pixelFormat));

    if (m_pInput)
        delete m_pInput;

    m_pInput = new CTransformInputPin(inputPinName, this, r, inputPinName);

    if (m_pOutput)
        delete m_pOutput;

    m_pOutput = new CH264DecoderOutputPin(this, r);
    if (r && FAILED(*r))
        return;
}