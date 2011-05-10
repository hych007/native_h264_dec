#ifndef _H264_DECODER_FILTER_H_
#define _H264_DECODER_FILTER_H_

#include <vector>

#include <boost/shared_ptr.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/intrusive_ptr.hpp>
#include <streams.h>
#include <d3dx9.h>
#include <videoacc.h>
#include <dxva2api.h>
#include <mfidl.h>

#include "chromium/base/basictypes.h"
#include "chromium/base/lock.h"

class CH264DecoderFilter;
class CH264DecoderOutputPin : public CTransformOutputPin,
                              public IAMVideoAcceleratorNotify
{
public:
    CH264DecoderOutputPin(CH264DecoderFilter* decoder, HRESULT* r);
    ~CH264DecoderOutputPin();

    // IUnknown
    DECLARE_IUNKNOWN;
    virtual HRESULT __stdcall NonDelegatingQueryInterface(const IID& ID,
                                                          void** o);

    // IAMVideoAcceleratorNotify
    virtual HRESULT __stdcall GetUncompSurfacesInfo(
        const GUID* profileID, AMVAUncompBufferInfo* uncompBufInfo);
    virtual HRESULT __stdcall SetUncompSurfacesInfo(
        DWORD actualUncompSurfacesAllocated);
    virtual HRESULT __stdcall GetCreateVideoAcceleratorData(
        const GUID* profileID, DWORD* miscDataSize, void** miscData);

private:
    CH264DecoderFilter* m_decoder;
    int m_DXVA1SurfCount;
    GUID m_DXVA1DecoderID;
    DDPIXELFORMAT m_uncompPixelFormat;
};

//------------------------------------------------------------------------------
class CCodecContext;
class CH264Decoder;
class CH264DecoderFilter : public CTransformFilter
{
public:
    static CUnknown* __stdcall CreateInstance(IUnknown* aggregator, HRESULT *r);

    virtual ~CH264DecoderFilter();

    virtual HRESULT CheckInputType(const CMediaType* inputType);
    virtual HRESULT CheckTransform(const CMediaType* inputType, 
                                   const CMediaType* outType);
    virtual HRESULT DecideBufferSize(IMemAllocator* allocator, 
                                     ALLOCATOR_PROPERTIES* request);
    virtual HRESULT GetMediaType(int position, CMediaType* mediaType);
    virtual HRESULT SetMediaType(PIN_DIRECTION dir,
                                 const CMediaType* mediaType);
    virtual HRESULT CompleteConnect(PIN_DIRECTION dir, IPin* receivePin);
    virtual HRESULT BreakConnect(PIN_DIRECTION dir);
    virtual HRESULT NewSegment(REFERENCE_TIME start, REFERENCE_TIME stop,
                               double rate);
    virtual HRESULT Receive(IMediaSample* sample);

    HRESULT ActivateDXVA1(IAMVideoAccelerator* accel, const GUID* decoderID,
                          const AMVAUncompDataInfo& uncompInfo,
                          int surfaceCount);
    HRESULT ActivateDXVA2();
    HRESULT CreateDXVA2Decoder(const GUID& decoderID,
                               const DXVA2_VideoDesc& videoDesc);
    bool IsFormatSupported(const GUID& formatID);
    HRESULT ConfirmDXVA1UncompFormat(IAMVideoAccelerator* accel,
                                     const GUID* decoderID,
                                     DDPIXELFORMAT* pixelFormat);
    HRESULT ConfirmDXVA2UncompFormat(
        IDirectXVideoDecoderService* decoderService, const GUID* decoderID,
        DXVA2_ConfigPictureDecode* selectedConfig,
        DXVA2_VideoDesc* selectedFormat);
    void SetDXVA1PixelFormat(const DDPIXELFORMAT& pixelFormat);
    bool NeedCustomizeAllocator();
    IDirect3DSurface9* GetSurface(int n);
    void FlushDXVADecoder() {}

    boost::intrusive_ptr<IDirect3DDeviceManager9> Get3DDevManager()
    {
        return m_devManager;
    }

    HANDLE m_devHandle;

protected:
    CH264DecoderFilter(IUnknown* aggregator, HRESULT* r);

private:
    HRESULT configureEVRForDXVA2(IMFGetService* getService);

    std::vector<boost::shared_ptr<CMediaType> > m_mediaTypes;
    boost::shared_ptr<CCodecContext> m_preDecode;
    DDPIXELFORMAT m_pixelFormat;
    Lock m_decodeAccess;
    int64 m_averageTimePerFrame;

    // DXVA2 members.
    boost::intrusive_ptr<IDirect3DDeviceManager9> m_devManager;
    boost::shared_ptr<void> m_deviceHandle;
    boost::intrusive_ptr<IDirectXVideoDecoderService> m_decoderService;
    DXVA2_ConfigPictureDecode m_config;
    std::vector<boost::intrusive_ptr<IDirect3DSurface9> > m_surfaces;

    // Put it into a first-release position.
    boost::shared_ptr<CH264Decoder> m_decoder;
};

#endif  // _H264_DECODER_FILTER_H_