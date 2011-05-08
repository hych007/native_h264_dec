#ifndef _FFMPEG_H_
#define _FFMPEG_H_

#include <boost/shared_ptr.hpp>
#include <boost/scoped_array.hpp>

#include "chromium/base/singleton.h"

struct IMediaSample;
class CMediaType;
class CVideoFrame;
class CCodecContext;
class CSWScale
{
public:
    CSWScale();
    ~CSWScale();

    bool Init(const CCodecContext& codec, IMediaSample* sample);
    bool Convert(const CVideoFrame& frame, void* buf);

private:
    boost::shared_ptr<void> m_cont;
    int m_width;
    int m_height;
};

//------------------------------------------------------------------------------
struct AVFrame;
class CVideoFrame
{
public:
    CVideoFrame();
    ~CVideoFrame();

    bool IsComplete() const { return m_isComplete; }
    void SetComplete(bool complete) { m_isComplete = complete; }
    bool GetTime(int64* start, int64* stop);
    void SetTypeSpecificFlags(IMediaSample* sample);

private:
    friend class CCodecContext;
    friend class CSWScale;
    AVFrame* getFrame();

    boost::shared_ptr<AVFrame> m_frame;
    bool m_isComplete;
};

//------------------------------------------------------------------------------
struct AVCodec;
struct AVCodecContext;
class CCodecContext
{
public:
    static void ReviseTypeSpecFlags(int firstFieldType, int picType,
                                    DWORD* flags);

    CCodecContext();
    ~CCodecContext();

    bool Init(AVCodec* c, const CMediaType& mediaType);
    int GetVideoLevel() const;
    int GetRefFrameCount() const;
    int GetWidth() const;
    int GetHeight() const;
    int GetNALLength() const;
    bool IsRefFrameInUse(int frameNum) const;
    void SetThreadNumber(int n);
    void SetSliceLong(void* sliceLong);
    void UpdateTime(int64 start, int64 stop);
    void PreDecodeBuffer(const void* data, int size, int* framePOC, int* outPOC,
                         int64* startTime);
    const void* GetPrivateData() const;
    int Decode(CVideoFrame* frame, const void* buf, int size);
    void FlushBuffers();

private:
    friend class CSWScale;

    static void handleUserData(AVCodecContext* c, const void* buf, int bufSize);

    AVCodecContext* getCodecContext();
    void allocExtraData(const CMediaType& mediaType);

    boost::shared_ptr<AVCodecContext> m_cont;
    boost::scoped_array<int8> m_extraData;
};

//------------------------------------------------------------------------------
class CFFMPEG : public Singleton<CFFMPEG>
{
public:
    static bool IsSubTypeSupported(const CMediaType& mediaType);
    static int GetInputBufferPaddingSize();
    static boost::shared_ptr<CCodecContext> CreateCodec(const CMediaType& mediaType);

    CFFMPEG();
    ~CFFMPEG();

private:
    static void logCallback(void* p, int level, const char* format, va_list v);
};

#endif  // _FFMPEG_H_