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

class CCodecContext;

interface __declspec(uuid("AE7EC2A2-1913-4a80-8DD6-DF1497ABA494"))
IDXVA2Sample : public IUnknown
{
    virtual int __stdcall GetDXSurfaceId() = 0;
};

class CDXVA2Allocator;
class CDXVA2Sample : public CMediaSample, public IMFGetService,
                     public IDXVA2Sample
{

public:
    CDXVA2Sample(CDXVA2Allocator* alloc, HRESULT* r);
    virtual ~CDXVA2Sample();

    // Note: CMediaSample does not derive from CUnknown, so we cannot use the
    // DECLARE_IUNKNOWN macro that is used by most of the filter classes.
    virtual HRESULT __stdcall QueryInterface(const IID& ID, void** o);
    virtual ULONG __stdcall AddRef();
    virtual ULONG __stdcall Release();

    // IMFGetService
    virtual HRESULT __stdcall GetService(const GUID& service, const IID& ID,
                                         void** o);

    // IMPCDXVA2Sample
    virtual int __stdcall GetDXSurfaceId() { return m_surfaceID; }

    // Override GetPointer because this class does not manage a system memory
    // buffer. The EVR uses the MR_BUFFER_SERVICE service to get the Direct3D
    // surface.
    virtual HRESULT __stdcall GetPointer(BYTE** buffer) { return E_NOTIMPL; }

private:
    friend class CDXVA2Allocator;

    // Sets the pointer to the Direct3D surface.
    void setSurface(int surfaceID, IDirect3DSurface9* surface);

    boost::intrusive_ptr<IDirect3DSurface9> m_surface;
    int m_surfaceID;
};

//------------------------------------------------------------------------------
class CH264DecoderFilter;
class CDXVA2Allocator : public CBaseAllocator
{
public:
    CDXVA2Allocator(CH264DecoderFilter* decoder, HRESULT* r);
    virtual ~CDXVA2Allocator();

protected:
    HRESULT Alloc();
    void Free();

private:
    CH264DecoderFilter* m_decoder;
    IDirect3DSurface9** m_surfaces;
    int m_surfaceCount;
};

//------------------------------------------------------------------------------
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

    // CBaseOutputPin
    virtual HRESULT InitAllocator(IMemAllocator** alloc);

private:
    CH264DecoderFilter* m_decoder;
    boost::intrusive_ptr<CDXVA2Allocator> m_allocator;
    int m_DXVA1SurfCount;
    GUID m_DXVA1DecoderID;
    DDPIXELFORMAT m_uncompPixelFormat;
};

//------------------------------------------------------------------------------
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
    HRESULT CreateDXVA2Decoder(const GUID& decoderID);
    bool IsFormatSupported(const GUID& formatID);
    HRESULT ConfirmDXVA1UncompFormat(IAMVideoAccelerator* accel,
                                     const GUID* decoderID,
                                     DDPIXELFORMAT* pixelFormat);
    HRESULT ConfirmDXVA2UncompFormat(
        IDirectXVideoDecoderService* decoderService, const GUID* decoderID,
        DXVA2_ConfigPictureDecode* selectedConfig);
    void SetDXVA1PixelFormat(const DDPIXELFORMAT& pixelFormat);

    boost::intrusive_ptr<IDirect3DDeviceManager9> Get3DDevManager()
    {
        return m_devManager;
    }

    DXVA2_VideoDesc m_videoDesc;
    HANDLE m_devHandle;
    void FlushDXVADecoder() {}
    bool UseDXVA2() { return false; }

protected:
    CH264DecoderFilter(IUnknown* aggregator, HRESULT* r);

private:
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

    // Put it into a first-release position.
    boost::shared_ptr<CH264Decoder> m_decoder;
};

#endif  // _H264_DECODER_FILTER_H_