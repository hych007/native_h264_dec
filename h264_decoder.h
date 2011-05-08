#ifndef _H264_DECODER_H_
#define _H264_DECODER_H_

#include <vector>

#include <boost/intrusive_ptr.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/scoped_ptr.hpp>

#include <strmif.h>
#include <d3dx9.h>
#include <videoacc.h>
#include <dxva.h>

#include "chromium/base/basictypes.h"

class CCodecContext;
class CH264Decoder
{
public:
    CH264Decoder(const GUID& decoderID, CCodecContext* preDecode);
    virtual ~CH264Decoder();

    const GUID& GetDecoderID() const { return m_decoderID; }

    virtual bool Init(const DDPIXELFORMAT& pixelFormat,
                      int64 averageTimePerFrame) = 0;
    virtual HRESULT Decode(const void* data, int size, int64 start, int64 stop,
                           IMediaSample* outSample, int* bytesUsed) = 0;
    virtual HRESULT DisplayNextFrame(IMediaSample* sample) { return E_NOTIMPL; }
    virtual void Flush();

protected:
    struct TDeocdedPicDesc
    {
        bool RefPicture;    // True if reference picture
        bool InUse;         // Slot in use
        bool Displayed;     // True if picture have been presented
        int64 Start;
        int64 Stop;
        int FirstFieldType; // Top or bottom for the 1st field
        int SliceType;
        int CodecSpecific;
        int DisplayCount;
    };

    class CDecodedPic : public TDeocdedPicDesc
    {
    public:
        CDecodedPic();
        ~CDecodedPic();
        const boost::intrusive_ptr<IMediaSample>& GetSample() const
        {
            return m_sample;
        }
        void SetSample(IMediaSample* sample) { m_sample = sample; }
        void Reinit();

    private:
        boost::intrusive_ptr<IMediaSample> m_sample;
    };

    CCodecContext* getPreDecode() { return m_preDecode; }
    bool getFlushed() const { return m_flushed; }
    void setFlushed(bool flushed) { m_flushed = flushed; }
    int getFieldSurface() const { return m_fieldSurface; }
    void setFieldSurface(int surf) { m_fieldSurface = surf; }
    const boost::intrusive_ptr<IMediaSample>& getFieldSample() const
    {
        return m_fieldSample;
    }
    void setFieldSample(IMediaSample* s) { m_fieldSample = s; }
    int incrementDispCount() { return m_displayCount++; }

private:
    GUID m_decoderID;
    CCodecContext* m_preDecode;
    bool m_flushed;
    int m_fieldSurface;
    boost::intrusive_ptr<IMediaSample> m_fieldSample;
    int m_displayCount;
};

//------------------------------------------------------------------------------
class CVideoFrame;
class CSWScale;
class CH264SWDecoder : public CH264Decoder
{
public:
    explicit CH264SWDecoder(CCodecContext* preDecode);
    virtual ~CH264SWDecoder();

    virtual bool Init(const DDPIXELFORMAT& pixelFormat,
                      int64 averageTimePerFrame);
    virtual HRESULT Decode(const void* data, int size, int64 start, int64 stop,
                           IMediaSample* outSample, int* bytesUsed);

private:
    boost::scoped_ptr<CVideoFrame> m_frame;
    boost::scoped_ptr<CSWScale> m_scale;
};

//------------------------------------------------------------------------------
class CH264DXVA1Decoder : public CH264Decoder
{
public:
    CH264DXVA1Decoder(const GUID& decoderID, CCodecContext* preDecode,
                      IAMVideoAccelerator* accel, int picEntryCount);
    virtual ~CH264DXVA1Decoder();

    virtual bool Init(const DDPIXELFORMAT& pixelFormat,
                      int64 averageTimePerFrame);
    virtual HRESULT Decode(const void* data, int size, int64 start, int64 stop,
                           IMediaSample* outSample, int* bytesUsed);
    virtual HRESULT DisplayNextFrame(IMediaSample* sample) { return S_OK; }
    virtual void Flush();

private:
    class CDXVABuffers
    {
    public:
        explicit CDXVABuffers(IAMVideoAccelerator* accel);
        ~CDXVABuffers();

        int GetSize() const { return m_bufInfo.size(); }
        HRESULT AllocExecBuffer(int compType, int bufIndex,
                                const void* nonBitStreamData, int size,
                                void** DXVABuffer);
        void ReviseLastDataSize(int size);
        void Clear();
        AMVABUFFERINFO* GetBufferInfo();
        DXVA_BufferDescription* GetBufferDesc();

    private:
        std::vector<AMVABUFFERINFO> m_bufInfo;
        std::vector<DXVA_BufferDescription> m_bufDesc;
        IAMVideoAccelerator* m_accel;
    };

    HRESULT getFreeSurfaceIndex(
        int* surfaceIndex, boost::intrusive_ptr<IMediaSample>* sampleToDeliver);
    HRESULT beginFrame(int surfaceIndex);
    HRESULT endFrame(int surfaceIndex);
    HRESULT execute();
    bool updateRefFrameSliceLong(int slice, int dataOffset, int sliceLength);
    bool updateRefFrameSliceShort(int slice, int dataOffset, int sliceLength);
    int buildBitStreamAndRefFrameSlice(const void* data, int size, void* dest);
    bool addToStandby(int surfaceIndex,
                      const boost::intrusive_ptr<IMediaSample>& sample,
                      bool isRefPicture, int64 start, int64 stop, bool isField,
                      int fieldType, int sliceType, int codecSpecific);
    void clearUnusedRefFrames();
    void removeRefFrame(int surfaceIndex);
    void freePictureSlot(int surfaceIndex);
    int findEarliestFrame();
    void setTypeSpecificFlags(const CDecodedPic& pic, IMediaSample* sample);
    HRESULT displayNextFrame(IMediaSample* sample);

    boost::intrusive_ptr<IAMVideoAccelerator> m_accel;
    DXVA_PicParams_H264 m_picParams;
    std::vector<DXVA_Slice_H264_Long> m_sliceLong;
    std::vector<DXVA_Slice_H264_Short> m_sliceShort;
    bool m_useLongSlice;
    std::vector<CDecodedPic> m_decodedPics;
    CDXVABuffers m_execBuffers;
    int m_outPOC;
    int64 m_outStart;
    int64 m_lastFrameTime;
    int64 m_estTimePerFrame;
};

//------------------------------------------------------------------------------
class CH264DXVA2Decoder : public CH264Decoder
{
public:
    virtual ~CH264DXVA2Decoder();

protected:
    friend class CH264Decoder;

    CH264DXVA2Decoder();
};

#endif  // _H264_DECODER_H_