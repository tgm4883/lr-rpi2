#include "privatedecoder_omx.h"

#include <algorithm>
#include <cassert>
#include <cstddef>

#include <IL/OMX_Video.h>
#ifdef USING_BROADCOM
#include <IL/OMX_Broadcom.h>
#endif

#include <QMutexLocker>

extern "C" {
#include "libavutil/pixdesc.h"
#include "libavcodec/avcodec.h"
}

#include "myth_imgconvert.h"
#include "mythlogging.h"
#include "omxcontext.h"
using namespace omxcontext;


#define LOC QString("DOMX:%1 ").arg(m_videc.Id())

// Stringize a macro
#define _STR(s) #s
#define STR(s) _STR(s)


/*
 * Types
 */


/*
 * Module data
 */
QString const PrivateDecoderOMX::s_name("openmax");


/*
 * Prototypes
 */
static const char *H264Profile2String(int profile);


/*
 * Functions
 */

// Convert PTS <> OMX ticks
static inline OMX_TICKS Pts2Ticks(AVStream *stream, int64_t pts)
{
    if (pts == int64_t(AV_NOPTS_VALUE))
        return S64_TO_TICKS(0);

    return S64_TO_TICKS( int64_t(
            (av_q2d(stream->time_base) * pts)* OMX_TICKS_PER_SECOND) );
}

static inline int64_t Ticks2Pts(AVStream *stream, OMX_TICKS ticks)
{
    return int64_t( (TICKS_TO_S64(ticks) /
                    av_q2d(stream->time_base)) / OMX_TICKS_PER_SECOND );
}

// static
void PrivateDecoderOMX::GetDecoders(render_opts &opts)
{
    opts.decoders->append(s_name);
    (*opts.equiv_decoders)[s_name].append("nuppel");
    (*opts.equiv_decoders)[s_name].append("ffmpeg");
    (*opts.equiv_decoders)[s_name].append("dummy");
}

PrivateDecoderOMX::PrivateDecoderOMX() : m_videc("video_decode", *this),
    m_filter(0), m_bStartTime(false),
#ifdef USING_BROADCOM
    m_eMode(OMX_InterlaceFieldsInterleavedUpperFirst), m_bRepeatFirstField(false),
#endif
    m_lock(QMutex::Recursive), m_bSettingsChanged(false),
    m_bSettingsHaveChanged(false)
{
    if (OMX_ErrorNone != m_videc.Init(OMX_IndexParamVideoInit))
        return;

    if (!m_videc.IsValid())
        return;

    // Show default port definitions and video formats supported
    for (unsigned port = 0; port < m_videc.Ports(); ++port)
    {
        m_videc.ShowPortDef(port, LOG_DEBUG);
        if (0) m_videc.ShowFormats(port, LOG_DEBUG);
    }
}

// virtual
PrivateDecoderOMX::~PrivateDecoderOMX()
{
    // Must shutdown the decoder now before our state becomes invalid.
    // When the decoder dtor is called our state has already been destroyed.
    m_videc.Shutdown();

    if (m_filter)
        av_bitstream_filter_close(m_filter);
}

// virtual
QString PrivateDecoderOMX::GetName(void)
{
    return s_name;
}

// pure virtual
bool PrivateDecoderOMX::Init(const QString &decoder, PlayerFlags flags,
    AVCodecContext *avctx)
{
    LOG(VB_PLAYBACK, LOG_DEBUG, LOC + __func__ + QString(
            "(decoder=%1 flags=%2) - begin").arg(decoder).arg(flags));

    if (decoder != s_name || !(flags & kDecodeAllowEXT) || !avctx)
        return false;

    if (getenv("NO_OPENMAX"))
    {
        LOG(VB_PLAYBACK, LOG_NOTICE, LOC + "OpenMAX decoder disabled");
        return false;
    }

    if (!m_videc.IsValid())
    {
        LOG(VB_GENERAL, LOG_ERR, LOC + "No video decoder");
        return false;
    }

    if (m_videc.Ports() < 2)
    {
        LOG(VB_GENERAL, LOG_ERR, LOC + "No video decoder ports");
        return false;
    }

    OMX_VIDEO_CODINGTYPE type = OMX_VIDEO_CodingUnused;
    switch (avctx->codec_id)
    {
      case AV_CODEC_ID_MPEG1VIDEO:
      case AV_CODEC_ID_MPEG2VIDEO:
        type = OMX_VIDEO_CodingMPEG2;
        break;
      case AV_CODEC_ID_H263:
      case AV_CODEC_ID_H263P:
      case AV_CODEC_ID_H263I:
        type = OMX_VIDEO_CodingH263;
        break;
      case AV_CODEC_ID_RV10:
      case AV_CODEC_ID_RV20:
      case AV_CODEC_ID_RV30:
      case AV_CODEC_ID_RV40:
        type = OMX_VIDEO_CodingRV;
        break;
      case AV_CODEC_ID_MJPEG:
      case AV_CODEC_ID_MJPEGB:
        type = OMX_VIDEO_CodingMJPEG;
        break;
      case AV_CODEC_ID_MPEG4:
        type = OMX_VIDEO_CodingMPEG4;
        break;
      case AV_CODEC_ID_WMV1:
      case AV_CODEC_ID_WMV2:
      case AV_CODEC_ID_WMV3:
        type = OMX_VIDEO_CodingWMV;
        break;
      case AV_CODEC_ID_H264:
        LOG(VB_PLAYBACK, LOG_DEBUG, LOC + QString("Codec H264 %1")
            .arg(H264Profile2String(avctx->profile)) );
        type = OMX_VIDEO_CodingAVC;
        break;
      case AV_CODEC_ID_THEORA:
        type = OMX_VIDEO_CodingTheora;
        break;
      case AV_CODEC_ID_VP3:
      case AV_CODEC_ID_VP5:
      case AV_CODEC_ID_VP6:
      case AV_CODEC_ID_VP6F:
      case AV_CODEC_ID_VP6A:
      //case AV_CODEC_ID_VP8:
      //case AV_CODEC_ID_VP9:
        type = OMX_VIDEO_CodingVP6;
        break;
      case AV_CODEC_ID_MVC1:
      case AV_CODEC_ID_MVC2:
        type = OMX_VIDEO_CodingMVC;
        break;
      default:
        LOG(VB_PLAYBACK, LOG_ERR, LOC + QString("Codec %1 not supported")
                .arg(ff_codec_id_string(avctx->codec_id)));
        return false;
    }

    // Set input format
    LOG(VB_PLAYBACK, LOG_INFO, LOC + QString("Codec %1 => %2")
            .arg(ff_codec_id_string(avctx->codec_id)).arg(Coding2String(type)));

    OMX_VIDEO_PARAM_PORTFORMATTYPE fmt;
    OMX_DATA_INIT(fmt);
    fmt.nPortIndex = m_videc.Base();
    fmt.eCompressionFormat = type; // OMX_VIDEO_CodingAutoDetect gives error
    fmt.eColorFormat = OMX_COLOR_FormatUnused;
    OMX_ERRORTYPE e = m_videc.SetParameter(OMX_IndexParamVideoPortFormat, &fmt);
    if (e != OMX_ErrorNone)
    {
        LOG(VB_PLAYBACK, LOG_ERR, LOC + QString(
                "Set input IndexParamVideoPortFormat error %1")
            .arg(Error2String(e)));
        return false;
    }

    if (type == OMX_VIDEO_CodingAVC)
    {
        if (SetNalType(avctx) != OMX_ErrorNone)
        {
            if (avctx->extradata_size > 0 && avctx->extradata[0] == 1)
            {
                // Install s/w filter to convert mp4 to annex B
                if (!CreateFilter(avctx))
                    return false;
            }
        }
    }

    // Check output port default pixel format
    switch (m_videc.PortDef(1).format.video.eColorFormat)
    {
      case OMX_COLOR_FormatYUV420Planar:
      case OMX_COLOR_FormatYUV420PackedPlanar: // Broadcom default
        // AV_PIX_FMT_YUV420P;
        break;

      default:
        // Set output pixel format
        fmt.nPortIndex = m_videc.Base() + 1;
        fmt.eCompressionFormat = OMX_VIDEO_CodingUnused;
        fmt.eColorFormat = OMX_COLOR_FormatYUV420PackedPlanar;
        e = m_videc.SetParameter(OMX_IndexParamVideoPortFormat, &fmt);
        if (e != OMX_ErrorNone)
        {
            LOG(VB_PLAYBACK, LOG_ERR, LOC + QString(
                    "Set output IndexParamVideoPortFormat error %1")
                .arg(Error2String(e)));
            return false;
        }
        break;
    }

    // Ensure at least 2 output buffers
    OMX_PARAM_PORTDEFINITIONTYPE &def = m_videc.PortDef(1);
    if (def.nBufferCountActual < 2U ||
        def.nBufferCountActual < def.nBufferCountMin)
    {
        def.nBufferCountActual = std::max(2U, def.nBufferCountMin);
        e = m_videc.SetParameter(OMX_IndexParamPortDefinition, &def);
        if (e != OMX_ErrorNone)
        {
            LOG(VB_PLAYBACK, LOG_ERR, LOC + QString(
                    "Set output IndexParamPortDefinition error %1")
                .arg(Error2String(e)));
            return false;
        }
    }

    // Goto OMX_StateIdle & allocate all buffers
    // This generates an error if fmt.eCompressionFormat is not supported
    OMXComponentCB<PrivateDecoderOMX> cb(this, &PrivateDecoderOMX::AllocBuffersCB);
    e = m_videc.SetState(OMX_StateIdle, 500, &cb);
    if (e != OMX_ErrorNone)
    {
        // NB The RPi requires a license file for MPEG decoding.
        if (type == OMX_VIDEO_CodingMPEG2)
            LOG(VB_GENERAL, LOG_WARNING, LOC +
                "NB MPEG2 decoding requires a license file");
        return false;
    }

    m_bSettingsHaveChanged = false;

    // Goto OMX_StateExecuting
    e = m_videc.SetState(OMX_StateExecuting, 500);
    if (e != OMX_ErrorNone)
        return false;

    e = FillOutputBuffers();
    if (e != OMX_ErrorNone)
        return false;

    // The decoder is now ready for input
    LOG(VB_PLAYBACK, LOG_DEBUG, LOC + __func__ + " - end");
    return true;
}

// Setup H264 decoder for NAL type
OMX_ERRORTYPE PrivateDecoderOMX::SetNalType(AVCodecContext *avctx)
{
#ifdef USING_BROADCOM
    OMX_NALSTREAMFORMATTYPE fmt;
    OMX_DATA_INIT(fmt);
    fmt.nPortIndex = m_videc.Base();

    OMX_ERRORTYPE e = m_videc.GetParameter(
        OMX_INDEXTYPE(OMX_IndexParamNalStreamFormatSupported), &fmt);
    if (e != OMX_ErrorNone)
    {
        LOG(VB_PLAYBACK, LOG_ERR, LOC + QString(
                "Get ParamNalStreamFormatSupported error %1")
            .arg(Error2String(e)));
        if (avctx->extradata_size == 0 || avctx->extradata[0] != 1)
            return OMX_ErrorNone; // Annex B, no filter needed
        return e;
    }

    static bool s_bReported;
    if (!s_bReported)
    {
        s_bReported = true;
        QStringList list;
        if (fmt.eNaluFormat & OMX_NaluFormatStartCodes)
            list << "StartCodes (Annex B)";
        if (fmt.eNaluFormat & OMX_NaluFormatOneNaluPerBuffer)
            list << "OneNaluPerBuffer";
        if (fmt.eNaluFormat & OMX_NaluFormatOneByteInterleaveLength)
            list << "OneByteInterleaveLength";
        if (fmt.eNaluFormat & OMX_NaluFormatTwoByteInterleaveLength)
            list << "TwoByteInterleaveLength";
        if (fmt.eNaluFormat & OMX_NaluFormatFourByteInterleaveLength)
            list << "FourByteInterleaveLength";
        LOG(VB_PLAYBACK, LOG_INFO, LOC + "NalStreamFormatSupported: " +
            list.join(", "));
    }

    OMX_NALUFORMATSTYPE type;
    QString naluFormat;
    if (avctx->extradata_size >= 5 && avctx->extradata[0] == 1)
    {
        // AVCC NALs (mp4 stream)
        int n = 1 + (avctx->extradata[4] & 0x3);
        switch (n)
        {
          case 1:
            type = OMX_NaluFormatOneByteInterleaveLength;
            naluFormat = "OneByteInterleaveLength";
            break;
          case 2:
            type = OMX_NaluFormatTwoByteInterleaveLength;
            naluFormat = "TwoByteInterleaveLength";
            break;
          case 4:
            type = OMX_NaluFormatFourByteInterleaveLength;
            naluFormat = "FourByteInterleaveLength";
            break;
          default:
            return OMX_ErrorUnsupportedSetting;
        }
    }
    else
    {
        type = OMX_NaluFormatStartCodes; // Annex B
        naluFormat = "StartCodes";
    }

    fmt.eNaluFormat = OMX_NALUFORMATSTYPE(fmt.eNaluFormat & type);
    if (!fmt.eNaluFormat)
    {
        LOG(VB_PLAYBACK, LOG_INFO, LOC + "Unsupported NAL stream format " +
            naluFormat);
        return OMX_ErrorUnsupportedSetting;
    }

    LOG(VB_PLAYBACK, LOG_DEBUG, LOC + "NAL stream format: " + naluFormat);

    e = m_videc.SetParameter(OMX_INDEXTYPE(OMX_IndexParamNalStreamFormatSelect), &fmt);
    if (e != OMX_ErrorNone)
        LOG(VB_PLAYBACK, LOG_ERR, LOC + QString(
                "Set ParamNalStreamFormatSelect(%1) error %2")
            .arg(naluFormat).arg(Error2String(e)));
    return e;
#else
    if (avctx->extradata_size == 0 || avctx->extradata[0] != 1)
        return OMX_ErrorNone; // Annex B, no filter needed

    return OMX_ErrorUnsupportedSetting;
#endif //USING_BROADCOM
}


// Create an h264_mp4toannexb conversion filter
bool PrivateDecoderOMX::CreateFilter(AVCodecContext *avctx)
{
    // Check NAL size
    if (avctx->extradata_size < 5)
    {
        LOG(VB_PLAYBACK, LOG_ERR, LOC + "AVCC extradata_size too small");
        return false;
    }

    int n = 1 + (avctx->extradata[4] & 0x3);
    switch (n)
    {
      case 1:
      case 2:
      case 4:
        break;
      default:
        LOG(VB_PLAYBACK, LOG_ERR, LOC + QString("Invalid NAL size (%1)")
            .arg(n));
        return false;
    }

    if (m_filter)
        return true;

    m_filter = av_bitstream_filter_init("h264_mp4toannexb");
    if (!m_filter)
    {
        LOG(VB_PLAYBACK, LOG_ERR, LOC +
            "Failed to create h264_mp4toannexb filter");
        return false;
    }

    // Test the filter
    static const uint8_t test[] = { 0U,0U,0U,2U,0U,0U };
    int outbuf_size = 0;
    uint8_t *outbuf = NULL;
    int res = av_bitstream_filter_filter(m_filter, avctx, NULL, &outbuf,
                                         &outbuf_size, test, sizeof test, 0);
    if (res < 0)
    {
        LOG(VB_PLAYBACK, LOG_ERR, LOC + "h264_mp4toannexb filter test failed");
        av_bitstream_filter_close(m_filter);
        m_filter = NULL;
        return false;
    }

    av_freep(&outbuf);
    LOG(VB_GENERAL, LOG_INFO, LOC + "Installed h264_mp4toannexb filter");
    return true;
}

// OMX_StateIdle callback
OMX_ERRORTYPE PrivateDecoderOMX::AllocBuffersCB()
{
    assert(m_ibufs_sema.available() == 0);
    assert(m_ibufs.isEmpty());
    assert(m_obufs_sema.available() == 0);
    assert(m_obufs.isEmpty());

    // Allocate input buffers
    int index = 0;
    const OMX_PARAM_PORTDEFINITIONTYPE &def = m_videc.PortDef(index);
    OMX_U32 uBufs = def.nBufferCountActual;
    LOG(VB_PLAYBACK, LOG_DEBUG, LOC + QString(
            "Allocate %1 of %2 byte input buffer(s)")
        .arg(uBufs).arg(def.nBufferSize));
    while (uBufs--)
    {
        OMX_BUFFERHEADERTYPE *hdr;
        OMX_ERRORTYPE e = OMX_AllocateBuffer(m_videc.Handle(), &hdr,
            m_videc.Base() + index, this, def.nBufferSize);
        if (e != OMX_ErrorNone)
        {
            LOG(VB_PLAYBACK, LOG_ERR, LOC + QString(
                "OMX_AllocateBuffer error %1").arg(Error2String(e)) );
            return e;
        }
        if (hdr->nSize != sizeof(OMX_BUFFERHEADERTYPE))
        {
            LOG(VB_PLAYBACK, LOG_ERR, LOC + "OMX_AllocateBuffer header mismatch");
            OMX_FreeBuffer(m_videc.Handle(), m_videc.Base() + index, hdr);
            return OMX_ErrorVersionMismatch;
        }
        if (hdr->nVersion.nVersion != OMX_VERSION)
        {
            LOG(VB_PLAYBACK, LOG_ERR, LOC + "OMX_AllocateBuffer version mismatch");
            OMX_FreeBuffer(m_videc.Handle(), m_videc.Base() + index, hdr);
            return OMX_ErrorVersionMismatch;
        }
        hdr->nFilledLen = 0;
        hdr->nOffset = 0;
        m_lock.lock();
        m_ibufs.append(hdr);
        m_lock.unlock();
        m_ibufs_sema.release();
    }

    // Allocate output buffers
    return AllocOutputBuffersCB();
}

// Start filling the output buffers
OMX_ERRORTYPE PrivateDecoderOMX::FillOutputBuffers()
{
    while (m_obufs_sema.tryAcquire())
    {
        m_lock.lock();
        assert(!m_obufs.isEmpty());
        OMX_BUFFERHEADERTYPE *hdr = m_obufs.takeFirst();
        m_lock.unlock();

        hdr->nFlags = 0;
        hdr->nFilledLen = 0;
        OMX_ERRORTYPE e = OMX_FillThisBuffer(m_videc.Handle(), hdr);
        if (e != OMX_ErrorNone)
        {
            LOG(VB_PLAYBACK, LOG_ERR, LOC + QString(
                "OMX_FillThisBuffer error %1").arg(Error2String(e)) );
            m_lock.lock();
            m_obufs.append(hdr);
            m_lock.unlock();
            m_obufs_sema.release();
            return e;
        }
    }

    return OMX_ErrorNone;
}

// Deallocate output buffers
// OMX_CommandPortDisable callback
OMX_ERRORTYPE PrivateDecoderOMX::FreeOutputBuffersCB()
{
    const int index = 1;
    while (m_obufs_sema.tryAcquire(1, !m_videc.IsCommandComplete() ? 100 : 0) )
    {
        m_lock.lock();
        assert(!m_obufs.isEmpty());
        OMX_BUFFERHEADERTYPE *hdr = m_obufs.takeFirst();
        m_lock.unlock();

        OMX_ERRORTYPE e = OMX_FreeBuffer(m_videc.Handle(), m_videc.Base() + index, hdr);
        if (e != OMX_ErrorNone)
            LOG(VB_PLAYBACK, LOG_ERR, LOC + QString(
                    "OMX_FreeBuffer 0x%1 error %2")
                .arg(quintptr(hdr),0,16).arg(Error2String(e)));
    }
    assert(m_obufs.isEmpty());
    assert(m_obufs_sema.available() == 0);
    return OMX_ErrorNone;
}

// Allocate output buffers
// OMX_CommandPortEnable callback
OMX_ERRORTYPE PrivateDecoderOMX::AllocOutputBuffersCB()
{
    assert(m_obufs_sema.available() == 0);
    assert(m_obufs.isEmpty());

    const int index = 1;
    const OMX_PARAM_PORTDEFINITIONTYPE *pdef = &m_videc.PortDef(index);
    OMX_U32 uBufs = pdef->nBufferCountActual;
    LOG(VB_PLAYBACK, LOG_DEBUG, LOC + QString(
            "Allocate %1 of %2 byte output buffer(s)")
        .arg(uBufs).arg(pdef->nBufferSize));
    while (uBufs--)
    {
        OMX_BUFFERHEADERTYPE *hdr;
        OMX_ERRORTYPE e = OMX_AllocateBuffer(m_videc.Handle(), &hdr,
            m_videc.Base() + index, this, pdef->nBufferSize);
        if (e != OMX_ErrorNone)
        {
            LOG(VB_PLAYBACK, LOG_ERR, LOC + QString(
                "OMX_AllocateBuffer error %1").arg(Error2String(e)) );
            return e;
        }
        if (hdr->nSize != sizeof(OMX_BUFFERHEADERTYPE))
        {
            LOG(VB_PLAYBACK, LOG_ERR, LOC + "OMX_AllocateBuffer header mismatch");
            OMX_FreeBuffer(m_videc.Handle(), m_videc.Base() + index, hdr);
            return OMX_ErrorVersionMismatch;
        }
        if (hdr->nVersion.nVersion != OMX_VERSION)
        {
            LOG(VB_PLAYBACK, LOG_ERR, LOC + "OMX_AllocateBuffer version mismatch");
            OMX_FreeBuffer(m_videc.Handle(), m_videc.Base() + index, hdr);
            return OMX_ErrorVersionMismatch;
        }
        hdr->nFilledLen = 0;
        hdr->nOffset = 0;
        m_obufs.append(hdr);
        m_obufs_sema.release();
    }

    return OMX_ErrorNone;
}

// pure virtual
bool PrivateDecoderOMX::Reset(void)
{
    if (!m_videc.IsValid())
        return false;

    LOG(VB_PLAYBACK, LOG_DEBUG, LOC + __func__ + " - begin");

    m_bStartTime = false;

    if (m_bSettingsHaveChanged)
    {
        // Flush input
        OMX_ERRORTYPE e;
        e = m_videc.SendCommand(OMX_CommandFlush, m_videc.Base(), 0, 50);
        if (e != OMX_ErrorNone)
            return false;

        // Flush output
        e = m_videc.SendCommand(OMX_CommandFlush, m_videc.Base() + 1, 0, 50);
        if (e != OMX_ErrorNone)
            return false;
    }

    // Re-submit the empty output buffers
    FillOutputBuffers();

    LOG(VB_PLAYBACK, LOG_DEBUG, LOC + __func__ + " - end");
    return true;
}

// pure virtual
int PrivateDecoderOMX::GetFrame(
    AVStream *stream,
    AVFrame *picture,
    int *got_picture_ptr,
    AVPacket *pkt)
{
    if (!m_videc.IsValid())
        return -1;

    if (!stream)
        return -1;

    // Check for a decoded frame
    int ret = GetBufferedFrame(stream, picture);
    if (ret < 0)
        return ret;
    if (ret > 0)
        *got_picture_ptr = 1;

    // Check for OMX_EventPortSettingsChanged notification
    if (SettingsChanged() != OMX_ErrorNone)
        return -1;

    // Submit a packet for decoding
    return (pkt && pkt->size) ? ProcessPacket(stream, pkt) : 0;
}

// Submit a packet for decoding
int PrivateDecoderOMX::ProcessPacket(AVStream *stream, AVPacket *pkt)
{
    uint8_t *buf = pkt->data, *free_buf = NULL;
    int size = pkt->size;
    int ret = pkt->size;

    // Convert h264_mp4toannexb
    if (m_filter)
    {
        AVCodecContext *avctx = stream->codec;
        int outbuf_size = 0;
        uint8_t *outbuf = NULL;
        int res = av_bitstream_filter_filter(m_filter, avctx, NULL, &outbuf,
                                             &outbuf_size, buf, size, 0);
        if (res <= 0)
        {
            static int count;
            if (++count == 1)
                LOG(VB_GENERAL, LOG_ERR, LOC +
                    QString("Failed to convert packet (%1)").arg(res));
            if (count > 200)
                count = 0;
        }

        if (outbuf && outbuf_size > 0)
        {
            size = outbuf_size;
            buf = free_buf = outbuf;
        }
    }

    while (size > 0)
    {
        if (!m_ibufs_sema.tryAcquire(1, 5))
        {
            LOG(VB_PLAYBACK, LOG_DEBUG, LOC + __func__ + " - no buffers");
            ret = 0;
            break;
        }
        m_lock.lock();
        assert(!m_ibufs.isEmpty());
        OMX_BUFFERHEADERTYPE *hdr = m_ibufs.takeFirst();
        m_lock.unlock();

        int free = int(hdr->nAllocLen) - int(hdr->nFilledLen + hdr->nOffset);
        int cnt = (free > size) ? size : free;
        memcpy(&hdr->pBuffer[hdr->nOffset + hdr->nFilledLen], buf, cnt);
        hdr->nFilledLen += cnt;
        buf += cnt;
        size -= cnt;
        free -= cnt;

        hdr->nTimeStamp = Pts2Ticks(stream, pkt->pts);
        if (!m_bStartTime && (pkt->flags & AV_PKT_FLAG_KEY))
        {
            m_bStartTime = true;
            hdr->nFlags = OMX_BUFFERFLAG_STARTTIME;
        }
        else
            hdr->nFlags = 0;

        LOG(VB_PLAYBACK | VB_TIMESTAMP, LOG_INFO, LOC + QString(
                "EmptyThisBuffer len=%1 tc=%2uS (pts %3)")
            .arg(hdr->nFilledLen).arg(TICKS_TO_S64(hdr->nTimeStamp))
            .arg(pkt->pts) );

        OMX_ERRORTYPE e = OMX_EmptyThisBuffer(m_videc.Handle(), hdr);
        if (e != OMX_ErrorNone)
        {
            LOG(VB_PLAYBACK, LOG_ERR, LOC + QString(
                "OMX_EmptyThisBuffer error %1").arg(Error2String(e)) );
            m_lock.lock();
            m_ibufs.append(hdr);
            m_lock.unlock();
            m_ibufs_sema.release();
            ret = -1;
            break;
        }
    }

    if (free_buf)
        av_freep(&free_buf);

    return ret;
}

// Dequeue decoded buffer
int PrivateDecoderOMX::GetBufferedFrame(AVStream *stream, AVFrame *picture)
{
    if (!picture)
        return -1;

    AVCodecContext *avctx = stream->codec;
    if (!avctx)
        return -1;

    if (!m_obufs_sema.tryAcquire())
        return 0;

    m_lock.lock();
    assert(!m_obufs.isEmpty());
    OMX_BUFFERHEADERTYPE *hdr = m_obufs.takeFirst();
    m_lock.unlock();

    if (hdr->nFlags & ~OMX_BUFFERFLAG_ENDOFFRAME)
        LOG(VB_PLAYBACK, LOG_DEBUG, LOC +
            QString("Decoded frame flags=%1").arg(HeaderFlags(hdr->nFlags)) );

    if (avctx->pix_fmt < 0)
        avctx->pix_fmt = PIX_FMT_YUV420P; // == FMT_YV12

    int ret = 1; // Assume success
    if (hdr->nFilledLen == 0)
    {
        // This happens after an EventBufferFlag EOS
        ret = 0;
    }
    else if (avctx->get_buffer(avctx, picture) < 0)
    {
        LOG(VB_GENERAL, LOG_ERR, LOC +
            "Decoded frame available but no video buffers");
        ret = 0;
    }
    else
    {
        VideoFrame vf;
        VideoFrameType const frametype = FMT_YV12;
        PixelFormat in_fmt  = AV_PIX_FMT_NONE;
        int pitches[3]; // Y, U, & V pitches
        int offsets[3]; // Y, U, & V offsets
        unsigned char *buf = 0;

        const OMX_VIDEO_PORTDEFINITIONTYPE &vdef = m_videc.PortDef(1).format.video;
        switch (vdef.eColorFormat)
        {
          case OMX_COLOR_FormatYUV420Planar:
          case OMX_COLOR_FormatYUV420PackedPlanar: // Broadcom default
            pitches[0] = vdef.nStride;
            pitches[1] = pitches[2] = vdef.nStride >> 1;
            offsets[0] = 0;
            offsets[1] = vdef.nStride * vdef.nSliceHeight;
            offsets[2] = offsets[1] + (offsets[1] >> 2);
            in_fmt = AV_PIX_FMT_YUV420P;
            break;

          default:
            LOG(VB_GENERAL, LOG_ERR, LOC + QString(
                    "Unsupported frame type: %1")
                .arg(Format2String(vdef.eColorFormat)) );
            break;
        }

        if (in_fmt == avctx->pix_fmt)
        {
            init(&vf, frametype, &hdr->pBuffer[hdr->nOffset],
                vdef.nFrameWidth, vdef.nFrameHeight, hdr->nFilledLen,
                pitches, offsets);
        }
        else if (in_fmt != AV_PIX_FMT_NONE)
        {
            int in_width   = vdef.nFrameWidth;
            int in_height  = vdef.nFrameHeight;
            int out_width  = avctx->width;
            int out_height = avctx->height;
            int size       = ((bitsperpixel(frametype) * out_width) / 8) * out_height;
            uint8_t* src   = &hdr->pBuffer[hdr->nOffset];

            buf = new unsigned char[size];
            init(&vf, frametype, buf, out_width, out_height, size);

            AVPicture img_in, img_out;
            avpicture_fill(&img_out, (uint8_t *)vf.buf, avctx->pix_fmt,
                out_width, out_height);
            avpicture_fill(&img_in, src, in_fmt, in_width, in_height);
            img_in.data[0] = src;
            img_in.data[1] = src + offsets[1];
            img_in.data[2] = src + offsets[2];
            img_in.linesize[0] = pitches[0];
            img_in.linesize[1] = pitches[1];
            img_in.linesize[2] = pitches[2];

            LOG(VB_PLAYBACK, LOG_DEBUG, LOC + QString("Converting %1 to %2")
                .arg(av_pix_fmt_desc_get(in_fmt)->name)
                .arg(av_pix_fmt_desc_get(avctx->pix_fmt)->name) );
            myth_sws_img_convert(&img_out, avctx->pix_fmt, &img_in, in_fmt,
                in_width, in_height);
        }
        else
        {
            ret = 0;
        }

        if (ret)
        {
#ifdef USING_BROADCOM
            switch (m_eMode)
            {
              case OMX_InterlaceProgressive:
                vf.interlaced_frame = 0;
                vf.top_field_first = 0;
                break;
              case OMX_InterlaceFieldSingleUpperFirst:
                /* The data is interlaced, fields sent
                 * separately in temporal order, with upper field first */
                vf.interlaced_frame = 1;
                vf.top_field_first = 1;
                break;
              case OMX_InterlaceFieldSingleLowerFirst:
                vf.interlaced_frame = 1;
                vf.top_field_first = 0;
                break;
              case OMX_InterlaceFieldsInterleavedUpperFirst:
                /* The data is interlaced, two fields sent together line
                 * interleaved, with the upper field temporally earlier */
                vf.interlaced_frame = 1;
                vf.top_field_first = 1;
                break;
              case OMX_InterlaceFieldsInterleavedLowerFirst:
                vf.interlaced_frame = 1;
                vf.top_field_first = 0;
                break;
              case OMX_InterlaceMixed:
                /* The stream may contain a mixture of progressive
                 * and interlaced frames */
                vf.interlaced_frame = 1;
                break;
            }
            vf.repeat_pict = m_bRepeatFirstField;
#endif // USING_BROADCOM

            copy((VideoFrame*)picture->opaque, &vf);

            picture->repeat_pict = vf.repeat_pict;
            picture->interlaced_frame = vf.interlaced_frame;
            picture->top_field_first = vf.top_field_first;
            picture->reordered_opaque = Ticks2Pts(stream, hdr->nTimeStamp);
            LOG(VB_PLAYBACK | VB_TIMESTAMP, LOG_INFO, LOC + QString(
                    "Decoder output tc %1uS (pts %2)")
                .arg(TICKS_TO_S64(hdr->nTimeStamp))
                .arg(picture->reordered_opaque) );
        }

        delete[] buf;
    }

    hdr->nFlags = 0;
    hdr->nFilledLen = 0;
    OMX_ERRORTYPE e = OMX_FillThisBuffer(m_videc.Handle(), hdr);
    if (e != OMX_ErrorNone)
        LOG(VB_PLAYBACK, LOG_ERR, LOC + QString(
            "OMX_FillThisBuffer reQ error %1").arg(Error2String(e)) );

    return ret;
}

// Handle the port settings changed notification
OMX_ERRORTYPE PrivateDecoderOMX::SettingsChanged()
{
    QMutexLocker lock(&m_lock);

    // Check for errors notified in EventCB
    OMX_ERRORTYPE e = m_videc.LastError();
    if (e != OMX_ErrorNone)
    {
        LOG(VB_PLAYBACK, LOG_ERR, LOC + QString("SettingsChanged error %1")
            .arg(Error2String(e)));
        return e;
    }

    if (!m_bSettingsChanged)
        return OMX_ErrorNone;

    lock.unlock();

    LOG(VB_PLAYBACK, LOG_INFO, LOC + __func__ + " - begin");

    const int index = 1; // Output port index

    // Disable output port and free output buffers
    OMXComponentCB<PrivateDecoderOMX> cb(this, &PrivateDecoderOMX::FreeOutputBuffersCB);
    e = m_videc.PortDisable(index, 500, &cb);
    if (e != OMX_ErrorNone)
        return e;

    assert(m_obufs_sema.available() == 0);
    assert(m_obufs.isEmpty());

    // Update the output port definition to get revised buffer no. and size
    e = m_videc.GetPortDef(index);
    if (e != OMX_ErrorNone)
        return e;

    // Log the new port definition
    m_videc.ShowPortDef(index, LOG_INFO);

    // Set output format
    // Broadcom doc says OMX_COLOR_Format16bitRGB565 also supported
    OMX_VIDEO_PARAM_PORTFORMATTYPE fmt;
    OMX_DATA_INIT(fmt);
    fmt.nPortIndex = m_videc.Base() + index;
    fmt.eCompressionFormat = OMX_VIDEO_CodingUnused;
    fmt.eColorFormat = OMX_COLOR_FormatYUV420PackedPlanar; // default
    e = m_videc.SetParameter(OMX_IndexParamVideoPortFormat, &fmt);
    if (e != OMX_ErrorNone)
        LOG(VB_PLAYBACK, LOG_ERR, LOC + QString(
                "SetParameter IndexParamVideoPortFormat error %1")
            .arg(Error2String(e)));

#ifdef USING_BROADCOM
    OMX_CONFIG_INTERLACETYPE inter;
    e = GetInterlace(inter, index);
    if (e == OMX_ErrorNone)
    {
        LOG(VB_PLAYBACK, LOG_INFO, LOC + QString("%1%2")
            .arg(Interlace2String(inter.eMode))
            .arg(inter.bRepeatFirstField ? " rpt 1st" : "") );

        m_bRepeatFirstField = inter.bRepeatFirstField;
        m_eMode = inter.eMode;
#if 0 // Can't change interlacing setting, at least not on RPi
        switch (inter.eMode)
        {
          case OMX_InterlaceProgressive:
            break;

          case OMX_InterlaceFieldSingleUpperFirst:
          case OMX_InterlaceFieldSingleLowerFirst:
            break;

          case OMX_InterlaceFieldsInterleavedUpperFirst:
            /* The data is interlaced, two fields sent together line
             * interleaved, with the upper field temporally earlier */
            inter.eMode = OMX_InterlaceFieldSingleUpperFirst;
            break;
          case OMX_InterlaceFieldsInterleavedLowerFirst:
            inter.eMode = OMX_InterlaceFieldSingleLowerFirst;
            break;

          case OMX_InterlaceMixed:
            inter.eMode = OMX_InterlaceFieldSingleUpperFirst;
            break;
        }

        if (m_eMode != inter.eMode)
        {
            e = m_videc.SetConfig(OMX_IndexConfigCommonInterlace, &inter);
            if (e == OMX_ErrorNone)
                m_eMode = inter.eMode;
            else
                LOG(VB_PLAYBACK, LOG_ERR, LOC + QString(
                        "Set ConfigCommonInterlace error %1")
                    .arg(Error2String(e)));
        }
#endif
    }
#endif // USING_BROADCOM

    if (VERBOSE_LEVEL_CHECK(VB_PLAYBACK, LOG_INFO))
    {
        OMX_CONFIG_POINTTYPE aspect;
        e = GetAspect(aspect, index);
        if (e == OMX_ErrorNone)
            LOG(VB_PLAYBACK, LOG_INFO, LOC + QString("Pixel aspect x/y = %1/%2")
                .arg(aspect.nX).arg(aspect.nY) );
    }

    // Re-enable output port and allocate new output buffers
    OMXComponentCB<PrivateDecoderOMX> cb2(this, &PrivateDecoderOMX::AllocOutputBuffersCB);
    e = m_videc.PortEnable(index, 500, &cb2);
    if (e != OMX_ErrorNone)
        return e;

    // Submit the new output buffers
    e = FillOutputBuffers();
    if (e != OMX_ErrorNone)
        return e;

    lock.relock();
    m_bSettingsChanged = false;
    m_bSettingsHaveChanged = true;
    lock.unlock();

    LOG(VB_PLAYBACK, LOG_INFO, LOC + __func__ + " - end");
    return OMX_ErrorNone;
}

// virtual
bool PrivateDecoderOMX::HasBufferedFrames(void)
{
    QMutexLocker lock(&m_lock);
    return !m_obufs.isEmpty();
}

OMX_ERRORTYPE PrivateDecoderOMX::GetAspect(
    OMX_CONFIG_POINTTYPE &point, int index) const
{
    OMX_DATA_INIT(point);
    point.nPortIndex = m_videc.Base() + index;
    OMX_ERRORTYPE e = m_videc.GetParameter(OMX_IndexParamBrcmPixelAspectRatio,
                                            &point);
    if (e != OMX_ErrorNone)
        LOG(VB_PLAYBACK, LOG_ERR, LOC + QString(
                "IndexParamBrcmPixelAspectRatio error %1").arg(Error2String(e)));
    return e;
}

#ifdef USING_BROADCOM
OMX_ERRORTYPE PrivateDecoderOMX::GetInterlace(
    OMX_CONFIG_INTERLACETYPE &interlace, int index) const
{
    OMX_DATA_INIT(interlace);
    interlace.nPortIndex = m_videc.Base() + index;
    OMX_ERRORTYPE e = m_videc.GetConfig(OMX_IndexConfigCommonInterlace, &interlace);
    if (e != OMX_ErrorNone)
        LOG(VB_PLAYBACK, LOG_ERR, LOC + QString(
                "IndexConfigCommonInterlace error %1").arg(Error2String(e)));
    return e;
}
#endif // USING_BROADCOM

// virtual
OMX_ERRORTYPE PrivateDecoderOMX::Event(OMXComponent &cmpnt, OMX_EVENTTYPE eEvent,
    OMX_U32 nData1, OMX_U32 nData2, OMX_PTR pEventData)
{
    switch(eEvent)
    {
      case OMX_EventPortSettingsChanged:
        if (nData1 != m_videc.Base() + 1)
            break;

        if (m_lock.tryLock(500))
        {
            m_bSettingsChanged = true;
            m_lock.unlock();
        }
        else
        {
            LOG(VB_GENERAL, LOG_CRIT, LOC +
                "EventPortSettingsChanged deadlock");
        }
        return OMX_ErrorNone;

      default:
        break;
    }

    return OMXComponentCtx::Event(cmpnt, eEvent, nData1, nData2, pEventData);
}

// virtual
OMX_ERRORTYPE PrivateDecoderOMX::EmptyBufferDone(
    OMXComponent&, OMX_BUFFERHEADERTYPE *hdr)
{
    assert(hdr->nSize == sizeof(OMX_BUFFERHEADERTYPE));
    assert(hdr->nVersion.nVersion == OMX_VERSION);
    hdr->nFilledLen = 0;
    if (m_lock.tryLock(1000))
    {
        m_ibufs.append(hdr);
        m_lock.unlock();
        m_ibufs_sema.release();
    }
    else
        LOG(VB_GENERAL, LOG_CRIT, LOC + "EmptyBufferDone deadlock");
    return OMX_ErrorNone;
}

// virtual
OMX_ERRORTYPE PrivateDecoderOMX::FillBufferDone(
    OMXComponent&, OMX_BUFFERHEADERTYPE *hdr)
{
    assert(hdr->nSize == sizeof(OMX_BUFFERHEADERTYPE));
    assert(hdr->nVersion.nVersion == OMX_VERSION);
    if (m_lock.tryLock(1000))
    {
        m_obufs.append(hdr);
        m_lock.unlock();
        m_obufs_sema.release();
    }
    else
        LOG(VB_GENERAL, LOG_CRIT, LOC + "FillBufferDone deadlock");
    return OMX_ErrorNone;
}

// virtual
void PrivateDecoderOMX::ReleaseBuffers(OMXComponent &)
{
    // Free all output buffers
    FreeOutputBuffersCB();

    // Free all input buffers
    while (m_ibufs_sema.tryAcquire())
    {
        m_lock.lock();
        assert(!m_ibufs.isEmpty());
        OMX_BUFFERHEADERTYPE *hdr = m_ibufs.takeFirst();
        m_lock.unlock();

        OMX_ERRORTYPE e = OMX_FreeBuffer(m_videc.Handle(), m_videc.Base(), hdr);
        if (e != OMX_ErrorNone)
            LOG(VB_PLAYBACK, LOG_ERR, LOC + QString(
                    "OMX_FreeBuffer 0x%1 error %2")
                .arg(quintptr(hdr),0,16).arg(Error2String(e)));
    }
}

#define CASE2STR(f) case f: return STR(f)
#define CASE2STR_(f) case f: return #f

static const char *H264Profile2String(int profile)
{
    switch (profile)
    {
        CASE2STR_(FF_PROFILE_H264_BASELINE);
        CASE2STR_(FF_PROFILE_H264_MAIN);
        CASE2STR_(FF_PROFILE_H264_EXTENDED);
        CASE2STR_(FF_PROFILE_H264_HIGH);
        CASE2STR_(FF_PROFILE_H264_HIGH_10);
        CASE2STR_(FF_PROFILE_H264_HIGH_10_INTRA);
        CASE2STR_(FF_PROFILE_H264_HIGH_422);
        CASE2STR_(FF_PROFILE_H264_HIGH_422_INTRA);
        CASE2STR_(FF_PROFILE_H264_HIGH_444_PREDICTIVE);
        CASE2STR_(FF_PROFILE_H264_HIGH_444_INTRA);
        CASE2STR_(FF_PROFILE_H264_CAVLC_444);
    }
    static char buf[32];
    return strcpy(buf, qPrintable(QString("Profile 0x%1").arg(profile)));
}
/* EOF */
