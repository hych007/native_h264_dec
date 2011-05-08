#include "h264_decoder.h"

#include <limits>

#include <initguid.h>
#include <dxva2api.h>

#define EXCLUSIVE_TRACE_ENABLE
#include "ffmpeg.h"
#include "h264_detail.h"
#include "common/hardware_env.h"
#include "common/debug_util.h"
#include "common/intrusive_ptr_helper.h"
#include "chromium/base/platform_thread.h"

using std::vector;
using boost::shared_ptr;
using boost::intrusive_ptr;

namespace
{
const int compBufferCount = 18;
const int maxSlices = 16;

#define TRY_EXECUTE(x)\
{\
    const int maxRetry = 50;\
    int retry = 0;\
    do\
    {\
        r = x;\
        if ((SUCCEEDED(r)) || (r != E_PENDING))\
             break;\
\
        PlatformThread::YieldCurrentThread();\
    } while (++retry < maxRetry);\
}

int compTypeToBufType(int DXVA2CompType)
{
    if (DXVA2CompType <= DXVA2_BitStreamDateBufferType)
        return DXVA2CompType + 1;

    switch (DXVA2CompType)
    {
        case DXVA2_MotionVectorBuffer:
            return DXVA_MOTION_VECTOR_BUFFER;
        case DXVA2_FilmGrainBuffer:
            return DXVA_FILM_GRAIN_BUFFER;
        default:
            assert(false);
            return DXVA_COMPBUFFER_TYPE_THAT_IS_NOT_USED;
    }
}
}

enum KNALUType
{
    NALU_TYPE_SLICE = 1,
    NALU_TYPE_DPA = 2,
    NALU_TYPE_DPB = 3,
    NALU_TYPE_DPC = 4,
    NALU_TYPE_IDR = 5,
    NALU_TYPE_SEI = 6,
    NALU_TYPE_SPS = 7,
    NALU_TYPE_PPS = 8,
    NALU_TYPE_AUD = 9,
    NALU_TYPE_EOSEQ = 10,
    NALU_TYPE_EOSTREAM = 11,
    NALU_TYPE_FILL = 12
};

class CH264NALU
{
private :
    int forbidden_bit;          // should be always FALSE
    int nal_reference_idc;      // NALU_PRIORITY_xxxx
    KNALUType nal_unit_type;    // NALU_TYPE_xxxx    

    int m_nNALStartPos;         // NALU start (including startcode / size)
    int m_nNALDataPos;          // Useful part
    unsigned m_nDataLen;        // Length of the NAL unit (Excluding the start code, which does not belong to the NALU)

    const BYTE* m_pBuffer;
    int m_nCurPos;
    int m_nNextRTP;
    int m_nSize;
    int m_nNALSize;

    bool MoveToNextStartcode();

public :
    KNALUType GetType() const { return nal_unit_type; };
    bool IsRefFrame() const { return (nal_reference_idc != 0); };

    int GetDataLength() const { return m_nCurPos - m_nNALDataPos; };
    const BYTE* GetDataBuffer() { return m_pBuffer + m_nNALDataPos; };
    int GetRoundedDataLength() const
    {
        int nSize = m_nCurPos - m_nNALDataPos;
        return nSize + 128 - (nSize % 128);
    }

    int GetLength() const { return m_nCurPos - m_nNALStartPos; };
    const BYTE* GetNALBuffer() { return m_pBuffer + m_nNALStartPos; };
    bool IsEOF() const { return m_nCurPos >= m_nSize; };

    void SetBuffer (const void* pBuffer, int nSize, int nNALSize);
    bool ReadNext();
    int GetRawDataSize() const { return m_nSize; }
    const void* GetRawDataBuffer() const { return m_pBuffer; }
};

void CH264NALU::SetBuffer(const void* pBuffer, int nSize, int nNALSize)
{
    m_pBuffer = reinterpret_cast<const BYTE*>(pBuffer);
    m_nSize = nSize;
    m_nNALSize = nNALSize;
    m_nCurPos = 0;
    m_nNextRTP = 0;

    m_nNALStartPos = 0;
    m_nNALDataPos = 0;
}

bool CH264NALU::MoveToNextStartcode()
{
    int nBuffEnd =
        (m_nNextRTP > 0) ? std::min(m_nNextRTP, m_nSize-4) : m_nSize-4;

    for (int i = m_nCurPos; i < nBuffEnd; i++)
    {
        if ((*((DWORD*)(m_pBuffer+i)) & 0x00FFFFFF) == 0x00010000)
        {
            // Find next AnnexB Nal
            m_nCurPos = i;
            return true;
        }
    }

    if ((m_nNALSize != 0) && (m_nNextRTP < m_nSize))
    {
        m_nCurPos = m_nNextRTP;
        return true;
    }

    m_nCurPos = m_nSize;
    return false;
}

bool CH264NALU::ReadNext()
{
    if (m_nCurPos >= m_nSize)
        return false;

    if ((m_nNALSize != 0) && (m_nCurPos == m_nNextRTP))
    {
        // RTP Nalu type : (XX XX) XX XX NAL..., with XX XX XX XX or XX XX equal to NAL size
        m_nNALStartPos    = m_nCurPos;
        m_nNALDataPos    = m_nCurPos + m_nNALSize;
        int nTemp            = 0;
        for (int i=0; i<m_nNALSize; i++)
        {
            nTemp = (nTemp << 8) + m_pBuffer[m_nCurPos++];
        }
        m_nNextRTP += nTemp + m_nNALSize;
        MoveToNextStartcode();
    }
    else
    {
        // Remove trailing bits
        while (!m_pBuffer[m_nCurPos] &&
            ((*((DWORD*)(m_pBuffer+m_nCurPos)) & 0x00FFFFFF) != 0x00010000))
            m_nCurPos++;

        // AnnexB Nalu : 00 00 01 NAL...
        m_nNALStartPos    = m_nCurPos;
        m_nCurPos       += 3;
        m_nNALDataPos    = m_nCurPos;
        MoveToNextStartcode();
    }

    forbidden_bit = (m_pBuffer[m_nNALDataPos]>>7) & 1;
    nal_reference_idc = (m_pBuffer[m_nNALDataPos]>>5) & 3;
    nal_unit_type = (KNALUType)(m_pBuffer[m_nNALDataPos] & 0x1f);
    return true;
}

//------------------------------------------------------------------------------
CH264Decoder::CDecodedPic::CDecodedPic()
    : TDeocdedPicDesc()
    , m_sample()
{
    Reinit();
}

CH264Decoder::CDecodedPic::~CDecodedPic()
{
}

void CH264Decoder::CDecodedPic::Reinit()
{
    RefPicture = false;
    InUse = false;
    Displayed = false;
    Start = 0;
    Stop = 0;
    FirstFieldType = 0;
    SliceType = 0;
    CodecSpecific = -1;
    DisplayCount = 0;
    m_sample = NULL;
}

CH264Decoder::~CH264Decoder()
{
}

CH264Decoder::CH264Decoder(const GUID& decoderID, CCodecContext* preDecode)
    : m_decoderID(decoderID)
    , m_preDecode(preDecode)
    , m_flushed(false)
    , m_fieldSurface(-1)
    , m_fieldSample()
    , m_displayCount(1)
{
    assert(preDecode);
}

void CH264Decoder::Flush()
{
    m_flushed = true;
    m_fieldSurface = -1;
    m_fieldSample = NULL;
    m_displayCount = 1;
}

//------------------------------------------------------------------------------
CH264SWDecoder::CH264SWDecoder(CCodecContext* preDecode)
    : CH264Decoder(GUID_NULL, preDecode)
    , m_frame(new CVideoFrame)
    , m_scale(new CSWScale)
{
}

CH264SWDecoder::~CH264SWDecoder()
{
}

bool CH264SWDecoder::Init(const DDPIXELFORMAT& pixelFormat,
                          const CCodecContext* cont, int64 averageTimePerFrame)
{
    getPreDecode()->SetThreadNumber(
        CHardwareEnv::get()->GetNumOfLogicalProcessors());
    return true;
}

HRESULT CH264SWDecoder::Decode(const void* data, int size, int64 start,
                               int64 stop, IMediaSample* outSample,
                               int* bytesUsed)
{
    if (!m_scale->Init(*getPreDecode(), outSample))
        return E_FAIL;

    int usedBytes = getPreDecode()->Decode(m_frame.get(), data, size);
    if (!m_frame->IsComplete()) // Not enough data to build a frame.
        return S_OK;

    BYTE* buf;
    HRESULT r = outSample->GetPointer(&buf);
    if (FAILED(r))
        return r;

    if (!m_scale->Convert(*m_frame, buf))
        return E_FAIL;

    *bytesUsed = usedBytes;
    return S_OK;
}

//------------------------------------------------------------------------------
CH264DXVA1Decoder::CDXVABuffers::CDXVABuffers(IAMVideoAccelerator* accel)
    : m_bufInfo()
    , m_bufDesc()
    , m_accel(accel)
{
    assert(accel);
}

CH264DXVA1Decoder::CDXVABuffers::~CDXVABuffers()
{
    Clear();
}

HRESULT CH264DXVA1Decoder::CDXVABuffers::AllocExecBuffer(
    int compType, int bufIndex, const void* nonBitStreamData, int size,
    void** DXVABuffer)
{
    int bufType = compTypeToBufType(compType);

    void* allocated;
    LONG stride;
    HRESULT r = m_accel->GetBuffer(bufType, bufIndex, FALSE, &allocated,
                                   &stride);
    assert(SUCCEEDED(r));
    if (SUCCEEDED(r))
    {
        assert((compType != DXVA2_BitStreamDateBufferType) || DXVABuffer);
        if (compType != DXVA2_BitStreamDateBufferType)
            memcpy(allocated, nonBitStreamData, size);
        else if (DXVABuffer)
            *DXVABuffer = allocated;

        AMVABUFFERINFO info = {0};
        info.dwTypeIndex = bufType;
        info.dwDataSize = size;
        m_bufInfo.push_back(info);

        DXVA_BufferDescription desc = {0};
        desc.dwTypeIndex = bufType;
        desc.dwDataSize = size;
        m_bufDesc.push_back(desc);
        return true;
    }

    return r;
}

void CH264DXVA1Decoder::CDXVABuffers::ReviseLastDataSize(int size)
{
    vector<AMVABUFFERINFO>::reverse_iterator i = m_bufInfo.rbegin();
    if (i != m_bufInfo.rend())
        i->dwDataSize = size;

    vector<DXVA_BufferDescription>::reverse_iterator j = m_bufDesc.rbegin();
    if (j != m_bufDesc.rend())
        j->dwDataSize = size;
}

void CH264DXVA1Decoder::CDXVABuffers::Clear()
{
    for (int i = 0; i < static_cast<int>(m_bufInfo.size()); ++i)
    {
        HRESULT r = m_accel->ReleaseBuffer(m_bufInfo[i].dwTypeIndex,
                                           m_bufInfo[i].dwBufferIndex);
        assert(SUCCEEDED(r));
    }

    m_bufInfo.clear();
    m_bufDesc.clear();
}

AMVABUFFERINFO* CH264DXVA1Decoder::CDXVABuffers::GetBufferInfo()
{
    return &m_bufInfo[0];
}

DXVA_BufferDescription* CH264DXVA1Decoder::CDXVABuffers::GetBufferDesc()
{
    return &m_bufDesc[0];
}

CH264DXVA1Decoder::CH264DXVA1Decoder(const GUID& decoderID,
                                     CCodecContext* preDecode,
                                     IAMVideoAccelerator* accel,
                                     int picEntryCount)
    : CH264Decoder(decoderID, preDecode)
    , m_accel(accel)
    , m_picParams()
    , m_sliceLong()
    , m_sliceShort()
    , m_useLongSlice(false)
    , m_decodedPics()
    , m_execBuffers(accel)
    , m_outPOC(-1)
    , m_outStart(std::numeric_limits<int64>::min())
    , m_lastFrameTime(0)
    , m_estTimePerFrame(1)
{
    assert(accel);
    memset(&m_picParams, 0, sizeof(m_picParams));

    DXVA_Slice_H264_Long emptySliceLong = {0};
    m_sliceLong.resize(maxSlices, emptySliceLong);

    DXVA_Slice_H264_Short emptySliceShort = {0};
    m_sliceShort.resize(maxSlices, emptySliceShort);

    const int vendor = CHardwareEnv::get()->GetVideoCardVendor();
    m_picParams.Reserved16Bits =
        (CHardwareEnv::PCI_VENDOR_INTEL == vendor) ? 0x534C : 0;
    m_picParams.MbsConsecutiveFlag = 1;
    m_picParams.ContinuationFlag = 1;
    m_picParams.Reserved8BitsA = 0;
    m_picParams.Reserved8BitsB = 0;

    // Improve accelerator performances
    m_picParams.MinLumaBipredSize8x8Flag = 1;
    m_picParams.StatusReportFeedbackNumber = 0; // Use to report status

    assert(arraysize(m_picParams.RefFrameList) == 16);
    for (int i =0; i < arraysize(m_picParams.RefFrameList); ++i)
    {
        m_picParams.RefFrameList[i].AssociatedFlag = 1;
        m_picParams.RefFrameList[i].bPicEntry = 255;
        m_picParams.RefFrameList[i].Index7Bits = 127;
    }

    m_decodedPics.resize(picEntryCount);
}

CH264DXVA1Decoder::~CH264DXVA1Decoder()
{
}

bool CH264DXVA1Decoder::Init(const DDPIXELFORMAT& pixelFormat,
                             const CCodecContext* cont,
                             int64 averageTimePerFrame)
{
    assert(m_accel);
    assert(cont);

    DXVA_ConfigPictureDecode configRequested;
    memset(&configRequested, 0, sizeof(configRequested));
    configRequested.guidConfigBitstreamEncryption = DXVA_NoEncrypt;
    configRequested.guidConfigMBcontrolEncryption = DXVA_NoEncrypt;
    configRequested.guidConfigResidDiffEncryption = DXVA_NoEncrypt;
    configRequested.bConfigBitstreamRaw = 2;

    writeDXVA_QueryOrReplyFunc(&configRequested.dwFunction,
                               DXVA_QUERYORREPLYFUNCFLAG_DECODER_PROBE_QUERY,
                               DXVA_PICTURE_DECODING_FUNCTION);
    DXVA_ConfigPictureDecode config;
    config.guidConfigBitstreamEncryption = DXVA_NoEncrypt;
    config.guidConfigMBcontrolEncryption = DXVA_NoEncrypt;
    config.guidConfigResidDiffEncryption = DXVA_NoEncrypt;
    config.bConfigBitstreamRaw = 2;
    HRESULT r = m_accel->Execute(configRequested.dwFunction, &configRequested,
                                 sizeof(DXVA_ConfigPictureDecode), &config,
                                 sizeof(config), 0, NULL);
    if (FAILED(r))
        return false;

    writeDXVA_QueryOrReplyFunc(&config.dwFunction,
                               DXVA_QUERYORREPLYFUNCFLAG_DECODER_LOCK_QUERY,
                               DXVA_PICTURE_DECODING_FUNCTION);
    r = m_accel->Execute (config.dwFunction, &config, sizeof(config),
                          &configRequested,
                          sizeof(DXVA_ConfigPictureDecode), 0, NULL);

    AMVAUncompDataInfo dataInfo;
    DWORD d = compBufferCount;
    dataInfo.dwUncompWidth = 720;
    dataInfo.dwUncompHeight = 480;
    memcpy(&dataInfo.ddUncompPixelFormat, &pixelFormat, sizeof(pixelFormat));
    AMVACompBufferInfo compBufInfo[compBufferCount];
    r = m_accel->GetCompBufferInfo(&GetDecoderID(), &dataInfo, &d, compBufInfo);
    if (FAILED(r))
        return false;

    getPreDecode()->SetSliceLong(&m_sliceLong[0]);
    m_useLongSlice = (config.bConfigBitstreamRaw != 2);
    m_estTimePerFrame = averageTimePerFrame;
    return true;
}

bool flush_flag = false;
HRESULT CH264DXVA1Decoder::Decode(const void* data, int size, int64 start,
                                  int64 stop, IMediaSample* outSample,
                                  int* bytesUsed)
{
    assert(data);
    assert(bytesUsed);
    assert(getPreDecode());

    int framePOC;
    int outPOC;
    int64 startTime;
    getPreDecode()->PreDecodeBuffer(data, size, &framePOC, &outPOC, &startTime);
    TRACE(L"\n Predecode done. framePOC: %d, outPOC: %d, start: %.4f",
          framePOC, outPOC, startTime / 10000000.0f);

    // If parsing fail (probably no PPS/SPS), continue anyway it may arrived later (happen on truncated streams)
    int fieldType;
    int sliceType;
    if (FAILED(h264_detail::BuildPicParams(getPreDecode(), &m_picParams,
                                           &fieldType, &sliceType)))
        return S_FALSE;

    DXVA_Qmatrix_H264 scalingMatrix;
    if (FAILED(h264_detail::BuildScalingMatrix(getPreDecode(), &scalingMatrix)))
        return S_FALSE;

    // Wait I frame after a flush.
    if (getFlushed() && !m_picParams.IntraPicFlag)
        return S_FALSE;

    int surfaceIndex;
    intrusive_ptr<IMediaSample> sampleToDeliver;
    HRESULT r = getFreeSurfaceIndex(&surfaceIndex, &sampleToDeliver);
    if (FAILED(r))
        return r;

    if (flush_flag)
    {
        
    }

    h264_detail::SetCurrentPicIndex(surfaceIndex, &m_picParams, getPreDecode());

    TRACE_EXCL(L"\n Begin frame: %d", surfaceIndex);
    r = beginFrame(surfaceIndex);
    if (FAILED(r))
        return r;

    m_picParams.StatusReportFeedbackNumber++;

    // Send picture parameters
    r = m_execBuffers.AllocExecBuffer(DXVA2_PictureParametersBufferType, 0,
                                      &m_picParams, sizeof(m_picParams), NULL);
    if (FAILED(r))
        return r;

    r = execute();
    if (FAILED(r))
        return r;

    // Add bitstream, slice control and quantization matrix.
    void* DXVABuffer = NULL;
    r = m_execBuffers.AllocExecBuffer(DXVA2_BitStreamDateBufferType, 0, NULL, 0,
                                      &DXVABuffer);
    if (FAILED(r))
        return r;

    int slice = buildBitStreamAndRefFrameSlice(data, size, DXVABuffer);
    if (slice < 0)
        return S_FALSE;

    const void* execBuf = m_useLongSlice ?
        reinterpret_cast<const void*>(&m_sliceLong[0]) :
        reinterpret_cast<const void*>(&m_sliceShort[0]);
    int execBufSize =
        m_useLongSlice ?
            sizeof(m_sliceLong[0]) * slice :
            sizeof(m_sliceShort[0]) * slice;
    r = m_execBuffers.AllocExecBuffer(DXVA2_SliceControlBufferType, 0, execBuf,
                                      execBufSize, NULL);
    if (FAILED(r))
        return r;

    r = m_execBuffers.AllocExecBuffer(DXVA2_InverseQuantizationMatrixBufferType,
                                      0, &scalingMatrix, sizeof(scalingMatrix),
                                      NULL);
    if (FAILED(r))
        return r;

    // Decode bitstream
    r = execute();
    if (FAILED(r))
        return r;

    r = endFrame(surfaceIndex);

    bool added = addToStandby(surfaceIndex, sampleToDeliver,
                              m_picParams.RefPicFlag, start, stop,
                              m_picParams.field_pic_flag, fieldType, sliceType,
                              framePOC);
    h264_detail::UpdateRefFramesList(&m_picParams, getPreDecode());
    clearUnusedRefFrames();
    if (added)
    {
        r = displayNextFrame(outSample);
        if (outPOC != std::numeric_limits<int>::min())
        {
            m_outPOC = outPOC;
            m_outStart = startTime;
        }
    }

    setFlushed(false);
    *bytesUsed = size;
    return S_OK;
}

void CH264DXVA1Decoder::Flush()
{
    for (int i = 0; i < static_cast<int>(m_decodedPics.size()); ++i)
        m_decodedPics[i].Reinit();

    m_outPOC = -1;
    m_lastFrameTime = 0;
    CH264Decoder::Flush();
}

HRESULT CH264DXVA1Decoder::getFreeSurfaceIndex(
    int* surfaceIndex, intrusive_ptr<IMediaSample>* sampleToDeliver)
{
    assert(surfaceIndex);
    assert(sampleToDeliver);

    if (getFieldSurface() != -1)
    {
        *surfaceIndex = getFieldSurface();
        *sampleToDeliver = getFieldSample();
        setFieldSample(NULL);
        return S_FALSE;
    }

    int findResult = -1;
    int minDisplay = std::numeric_limits<int>::max();
    for (int i = 0; i < static_cast<int>(m_decodedPics.size()); ++i)
    {
        if (!m_decodedPics[i].InUse &&
            (m_decodedPics[i].DisplayCount < minDisplay))
        {
            minDisplay = m_decodedPics[i].DisplayCount;
            findResult  = i;
        }
    }

    if (findResult != -1)
    {
        *surfaceIndex = findResult;
        return S_OK;
    }

    assert(false);
    Flush();
    return E_UNEXPECTED;
}

HRESULT CH264DXVA1Decoder::beginFrame(int surfaceIndex)
{
    AMVABeginFrameInfo info;
    info.dwDestSurfaceIndex = surfaceIndex;
    info.dwSizeInputData = sizeof(surfaceIndex);
    info.pInputData = &surfaceIndex;
    info.dwSizeOutputData = 0;
    info.pOutputData = NULL;

    HRESULT r;
    for (int i = 0; i < 20; ++i)
    {
        TRY_EXECUTE(m_accel->BeginFrame(&info));
        if (SUCCEEDED(r))
            TRY_EXECUTE(m_accel->QueryRenderStatus(0xFFFFFFFF, 0, 0));

        if (SUCCEEDED(r))
            break;

        // Don't use PlatformThread::YieldCurrentThread() here, or the frames
        // will probably get interleaved.
        PlatformThread::Sleep(1);
    }

    return r;
}

HRESULT CH264DXVA1Decoder::endFrame(int surfaceIndex)
{
    AMVAEndFrameInfo endFrameInfo;
    endFrameInfo.dwSizeMiscData = sizeof(surfaceIndex);
    endFrameInfo.pMiscData = &surfaceIndex;
    return m_accel->EndFrame(&endFrameInfo);
}

HRESULT CH264DXVA1Decoder::execute()
{
    DWORD func = 0x01000000;
    int32 result;
    HRESULT r = m_accel->Execute(
        func, m_execBuffers.GetBufferDesc(),
        sizeof(DXVA_BufferDescription) * m_execBuffers.GetSize(),
        &result, sizeof(result), m_execBuffers.GetSize(),
        m_execBuffers.GetBufferInfo());
    assert(SUCCEEDED(r));

    m_execBuffers.Clear();
    return r;
}

bool CH264DXVA1Decoder::updateRefFrameSliceLong(int slice, int dataOffset,
                                                int sliceLength)
{
    if (slice >= static_cast<int>(m_sliceLong.size()))
        return false;

    m_sliceLong[slice].BSNALunitDataLocation = dataOffset;
    m_sliceLong[slice].SliceBytesInBuffer = sliceLength;
    m_sliceLong[slice].slice_id = slice;
    h264_detail::UpdateRefFrameSliceLong(&m_picParams, getPreDecode(),
                                         &m_sliceLong[slice]);
    if (slice)
    {
        m_sliceLong[slice].NumMbsForSlice =
            m_sliceLong[slice].first_mb_in_slice -
            m_sliceLong[slice - 1].first_mb_in_slice;

        m_sliceLong[slice - 1].NumMbsForSlice =
            m_sliceLong[slice].NumMbsForSlice;
    }
    return true;
}

bool CH264DXVA1Decoder::updateRefFrameSliceShort(int slice, int dataOffset,
                                                 int sliceLength)
{
    if (slice >= static_cast<int>(m_sliceShort.size()))
        return false;

    m_sliceShort[slice].BSNALunitDataLocation = dataOffset;
    m_sliceShort[slice].SliceBytesInBuffer = sliceLength;
    return true;
}

int CH264DXVA1Decoder::buildBitStreamAndRefFrameSlice(const void* data,
                                                      int size, void* dest)
{
    assert(data);
    assert(dest);

    CH264NALU block;
    block.SetBuffer(data, size, getPreDecode()->GetNALLength());

    bool (CH264DXVA1Decoder::*updateFunc)(int, int, int) =
        m_useLongSlice ?
            &CH264DXVA1Decoder::updateRefFrameSliceLong :
            &CH264DXVA1Decoder::updateRefFrameSliceShort;

    int8* destCursor = reinterpret_cast<int8*>(dest);
    int dataOffset = 0;
    int slice = 0;
    while (block.ReadNext())
    {
        if ((NALU_TYPE_SLICE == block.GetType()) ||
            (NALU_TYPE_IDR == block.GetType()))
        {
            // For AVC1, put startcode 0x000001
            destCursor[0] = 0;
            destCursor[1] = 0;
            destCursor[2] = 1;

            // Copy NALU
            memcpy(destCursor + 3, block.GetDataBuffer(),
                   block.GetDataLength());

            // Update slice control buffer
            int NALLength = block.GetDataLength() + 3;
            if (!(this->*updateFunc)(slice, dataOffset, NALLength))
                break;

            dataOffset += NALLength;
            destCursor += NALLength;
            slice++;
        }
    }

    // Complete with zero padding (buffer size should be a multiple of 128)
    int padding  = 128 - (dataOffset % 128);
    memset(destCursor, 0, padding);
    m_sliceLong[slice - 1].SliceBytesInBuffer += padding;
    m_sliceShort[slice - 1].SliceBytesInBuffer += padding;
    m_execBuffers.ReviseLastDataSize(dataOffset + padding);
    return slice;
}

bool CH264DXVA1Decoder::addToStandby(int surfaceIndex,
                                     const intrusive_ptr<IMediaSample>& sample,
                                     bool isRefPicture, int64 start, int64 stop,
                                     bool isField, int fieldType, int sliceType,
                                     int codecSpecific)
{
    CDecodedPic& ref = m_decodedPics[surfaceIndex];
    if (isField && (-1 == getFieldSurface()))
    {
        setFieldSurface(surfaceIndex);
        setFieldSample(sample.get());
        ref.FirstFieldType = fieldType;
        ref.Start = start;
        ref.Stop = stop;
        ref.CodecSpecific = codecSpecific;
        return false;
    }

    assert(!ref.GetSample());
    assert(!ref.InUse);
    assert(surfaceIndex < static_cast<int>(m_decodedPics.size()));

    ref.RefPicture = isRefPicture;
    ref.InUse = true;
    ref.Displayed = false;
    ref.SliceType = sliceType;
    ref.SetSample(sample.get());

    if (!isField)
    {
        ref.Start = start;
        ref.Stop = stop;
        ref.FirstFieldType = fieldType;
        ref.CodecSpecific = codecSpecific;
    }

    setFieldSurface(-1);
    return true;
}

void CH264DXVA1Decoder::clearUnusedRefFrames()
{
    for (int i = 0; i < static_cast<int>(m_decodedPics.size()); ++i)
        if (m_decodedPics[i].RefPicture && m_decodedPics[i].Displayed)
            if (!getPreDecode()->IsRefFrameInUse(i))
                removeRefFrame(i);
}

void CH264DXVA1Decoder::removeRefFrame(int surfaceIndex)
{
    m_decodedPics[surfaceIndex].RefPicture = false;
    if (m_decodedPics[surfaceIndex].Displayed)
        freePictureSlot(surfaceIndex);
}

void CH264DXVA1Decoder::freePictureSlot(int surfaceIndex)
{
    CDecodedPic& ref = m_decodedPics[surfaceIndex];
    ref.DisplayCount = incrementDispCount();
    ref.InUse = false;
    ref.Displayed = false;
    ref.CodecSpecific = -1;
    ref.SetSample(NULL);
}

int CH264DXVA1Decoder::findEarliestFrame()
{
    int index = -1;
    int64 earliest = std::numeric_limits<int64>::max();
    for (int i = 0; i < static_cast<int>(m_decodedPics.size()); ++i)
    {
        if (m_decodedPics[i].InUse && !m_decodedPics[i].Displayed)
        {
            if ((m_decodedPics[i].CodecSpecific == m_outPOC) &&
                (m_decodedPics[i].Start < earliest))
            {
                index  = i;
                earliest = m_decodedPics[i].Start;
            }
        }
    }

    if (index >= 0)
    {
        if (std::numeric_limits<int64>::min() == m_outStart)
        {
            // If start time not set (no PTS for example), guess presentation
            // time.
            m_outStart = m_lastFrameTime;
        }
        m_decodedPics[index].Start = m_outStart;
        m_decodedPics[index].Stop = m_outStart + m_estTimePerFrame;
        m_lastFrameTime = m_outStart + m_estTimePerFrame;
    }

    return index;
}

void CH264DXVA1Decoder::setTypeSpecificFlags(const CDecodedPic& pic,
                                             IMediaSample* sample)
{
    assert(sample);
    intrusive_ptr<IMediaSample2> sample2;
    HRESULT r = sample->QueryInterface(IID_IMediaSample2,
                                       reinterpret_cast<void**>(&sample2));
    if (FAILED(r))
        return;

    AM_SAMPLE2_PROPERTIES props;
    if(SUCCEEDED(sample2->GetProperties(sizeof(props),
                                        reinterpret_cast<BYTE*>(&props))))
    {
        props.dwTypeSpecificFlags &= ~0x7F;
        CCodecContext::ReviseTypeSpecFlags(pic.FirstFieldType, pic.SliceType,
                                           &props.dwTypeSpecificFlags);

        sample2->SetProperties(sizeof(props),
                               reinterpret_cast<BYTE*>(&props));
    }
}

HRESULT CH264DXVA1Decoder::displayNextFrame(IMediaSample* sample)
{
    int earliest = findEarliestFrame();
    if (earliest < 0)
        return S_FALSE;

    HRESULT r = S_FALSE;
    CDecodedPic& picRef = m_decodedPics[earliest];
    if (picRef.Start >= 0)
    {
        // For DXVA1, query a media sample at the last time (only one in the
        // allocator)
        sample->SetTime(&picRef.Start, &picRef.Stop);
        sample->SetMediaTime(NULL, NULL);
        setTypeSpecificFlags(picRef, sample);
        if (SUCCEEDED(r))
            r = m_accel->DisplayFrame(earliest, sample);
    }

    picRef.Displayed = true;
    if (!picRef.RefPicture)
        freePictureSlot(earliest);

    return r;
}