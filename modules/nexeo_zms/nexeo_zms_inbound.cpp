/**
 * @file nexeo_zms_inbound.cpp
 * Nexeo ZMS audio, inbound (ZMS/DSP -> baresip).
 */

// Class Definition
#include "nexeo_zms_inbound.h"

// System and Library Includes
#include <stdlib.h>
#include <unistd.h>
#include <gst/gst.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include <regex>

#include "ZMS.hpp"
#include "ZMS_MsgTypes.h"
#include <list>
#include <tuple>
#include <zoltar_sids.h>
#include <string>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static const std::list<std::tuple<int, int>> kMessageSubscriptions =
{
    { zms::MSG_SYS_PHASE_COMPLETE, ZMS_WILDCARD },
    { zms::MSG_NEW_SPEAKER_MIC, ZMS_WILDCARD },
    { zms::MSG_UPDATED_SPEAKER_MIC, ZMS_WILDCARD },
    { zms::MSG_UPDATED_TRIGGER_TBL, ZMS_WILDCARD },
    { zms::MSG_AUD_LANE1_OUTBOUND, ZMS_WILDCARD },
    { zms::MSG_AUD_LANE2_OUTBOUND, ZMS_WILDCARD },
    { zms::MSG_AUD_AOT_VOICE_L1, ZMS_WILDCARD },
    { zms::MSG_AUD_AOT_VOICE_L2, ZMS_WILDCARD },
    { zms::MSG_OT_TO_CT_START, ZMS_WILDCARD },
    { zms::MSG_OT_TO_CT_STOP, ZMS_WILDCARD },
    { zms::MSG_SID_UPDATED, ZMS_WILDCARD },
    { zms::MSG_AUDIO_VAIO_STOP_SPKR, ZMS_WILDCARD },
    { zms::MSG_APP_LOGGING_LVL_UPDATED, ZMS_WILDCARD },
    { zms::MSG_SID_UPDATED, SID_AOT_ENABLED },
    { zms::MSG_SID_UPDATED, SID_VAIO_ENABLED }
};

// ---------------------------------------------------------------------------
// Creates the audio device.
// ---------------------------------------------------------------------------
int NexeoZmsInbound::create(
    struct ausrc_st** stp,
    const struct ausrc* as,
    struct ausrc_prm* prm,
    const char* device,
    ausrc_read_h* rh,
    ausrc_error_h* errh,
    void* arg)
{
    if (!stp)
    {
        return EINVAL;
    }

    NexeoZmsInbound** st = static_cast<NexeoZmsInbound**>(
        mem_alloc(
            sizeof(NexeoZmsInbound*),
            [](void* arg)
            {
                delete *static_cast<NexeoZmsInbound**>(arg);
            }));
    if (!st)
    {
        return ENOMEM;
    }

    try
    {
        *st = new NexeoZmsInbound(as, prm, device, rh, errh, arg);
    }
    catch (int err)
    {
        delete *st;
        mem_deref(st);
        return err;
    }

    *stp = reinterpret_cast<struct ausrc_st*>(st);
    return 0;
}

// ---------------------------------------------------------------------------
// Constructor.
// ---------------------------------------------------------------------------
NexeoZmsInbound::NexeoZmsInbound(
    const struct ausrc* as,
    struct ausrc_prm* prm,
    const char* device,
    ausrc_read_h* rh,
    ausrc_error_h* errh,
    void* arg)
{
    (void) errh;

    if (!as || !prm || !rh || !device)
    {
        throw EINVAL;
    }

    if (!str_isset(device))
    {
        throw EINVAL;
    }

    if (prm->fmt != AUFMT_S16LE)
    {
        warning("zms_inbound: unsupported sample format (%s)\n",
            aufmt_name((enum aufmt) prm->fmt));
        throw ENOTSUP;
    }

    m_rh = rh;
    m_arg = arg;
    parseDeviceMessage(device);

    m_ptime = prm->ptime;
    if (!m_ptime)
    {
        m_ptime = 20;
    }

    if (!prm->srate)
    {
        prm->srate = 16000;
    }

    if (!prm->ch)
    {
        prm->ch = 1;
    }

    m_prm = *prm;
    m_sampc = prm->srate * prm->ch * m_ptime / 1000;
    m_psize = 2 * m_sampc;

    m_buf = (int16_t*) mem_zalloc(m_psize, NULL);
    if (!m_buf)
    {
        throw ENOMEM;
    }

    int err = aubuf_alloc(&m_aubuf, 0, 0);
    if (err)
    {
        throw err;
    }

    m_run = true;

    // TODO: probably need this to be common somewhere
    // unless we can verify that ZMS can handle dup addresses
    zms::Address logicalAddr(
        zms::BS7000,
        1,
        zms::A15,
        ZMS_WILDCARD,
        zms::SPM_OUTAUD);

    mZmsAgent = std::make_shared<zms::LinuxAgent>(
        "baresip zms_inbound",
        &logicalAddr);
    err = mZmsAgent->init();
    if (err)
    {
        warning("zms_inbound: ZMS init failed: %d\n", err);
        throw err;
    }

    bool subscribed = false;
    for (const auto& sub : kMessageSubscriptions)
    {
        auto type = std::get<0>(sub);
        auto index = std::get<1>(sub);

        err = mZmsAgent->subscribe(type, index);
        if (err)
        {
            warning("zms_inbound: Message subscription (%d/%d) failed: %d\n", type, index, err);
            throw err;
        }
        else if (type == m_msg)
        {
            subscribed = true;
        }
    }

    if (!subscribed)
    {
        err = mZmsAgent->subscribe(m_msg, ZMS_WILDCARD);
        if (err)
        {
            warning("zms_inbound: Message subscription (%d/%d) failed: %d\n", m_msg, ZMS_WILDCARD, err);
            throw err;
        }
    }

    setupPipeline();

    mRxThread = std::make_shared<std::thread>(&NexeoZmsInbound::rxMessage, this);
    pthread_setname_np(mRxThread->native_handle(), "zms_inbound rx");
}

// ---------------------------------------------------------------------------
// Destructor.
// ---------------------------------------------------------------------------
NexeoZmsInbound::~NexeoZmsInbound()
{
    if (m_run)
    {
        m_run = false;
    }

    mRxThread->join();

    gst_element_set_state(m_pipeline, GST_STATE_NULL);
    gst_object_unref(GST_OBJECT(m_pipeline));

    mem_deref(m_aubuf);
    mem_deref(m_buf);
}

// ---------------------------------------------------------------------------
// Handles playback of data ready from the pipeline.
// ---------------------------------------------------------------------------
void NexeoZmsInbound::packetHandler(GstBuffer* buffer)
{
    if (!m_run)
    {
        return;
    }

    GstMapInfo info;
    if (!gst_buffer_map(buffer, &info, GST_MAP_READ))
    {
        warning("zms_inbound: gst_buffer_map failed\n");
        return;
    }

    struct auframe af;
    af.fmt = AUFMT_RAW;
    af.srate = 0;
    af.sampv = (uint8_t *) info.data;
    af.sampc = info.size;
    af.timestamp = 0;
    af.level = AULEVEL_UNDEF;

    int err = aubuf_write_auframe(m_aubuf, &af);
    if (err)
    {
        warning("zms_inbound: aubuf_write failed: %m\n", err);
        return;
    }

    gst_buffer_unmap(buffer, &info);

    // Continue to process incoming audio.
    while (m_run)
    {
        const struct timespec delay =
        {
            0,
            (long) m_prm.ptime * (1000000 / 2)
        };

        auframe_init(
            &af,
            AUFMT_S16LE,
            m_buf,
            m_sampc,
            m_prm.srate,
            m_prm.ch);

        // Timed read from audio-buffer.
        if (m_prm.ptime &&
            aubuf_get_samp(
                m_aubuf,
                m_prm.ptime,
                m_buf,
                m_sampc))
        {
            ;
        }
        else
        {
            // Immediate read from audio-buffer.
            if (!m_prm.ptime)
            {
                struct auframe af;
                af.fmt = AUFMT_S16LE,
                af.srate = 0,
                af.sampv = (uint8_t *) m_buf,
                af.sampc = m_sampc,
                af.timestamp = 0,
                af.level = AULEVEL_UNDEF;
                aubuf_read_auframe(m_aubuf, &af);
            }

            // Call read handler.
            if (m_rh)
            {
                m_rh(&af, m_arg);
            }
        }

        if (aubuf_cur_size(m_aubuf) < m_psize)
        {
            break;
        }

        (void) nanosleep(&delay, NULL);
    }
}

// ---------------------------------------------------------------------------
// Handles sink data from GST, pushing to baresip.
// ---------------------------------------------------------------------------
void NexeoZmsInbound::handleData(
    GstElement* sink,
    GstBuffer* buffer,
    GstPad* pad,
    gpointer user_data)
{
    (void) sink;
    (void) pad;

    NexeoZmsInbound* st = (NexeoZmsInbound*) user_data;
    st->packetHandler(buffer);
}

// ---------------------------------------------------------------------------
// Creates and configures the pipeline elements.
// ---------------------------------------------------------------------------
void NexeoZmsInbound::setupPipeline()
{
    // Create and configure all elements.
    m_pipeline = gst_pipeline_new("zms pipeline");
    m_appsrc = gst_element_factory_make("appsrc", "zms src");
    m_capsfilt = gst_element_factory_make("capsfilter", "zms capsfilt");
    m_conv = gst_element_factory_make("audioconvert", "zms conv");
    m_resample = gst_element_factory_make("audioresample", "zms resample");
    m_queue = gst_element_factory_make("queue", "zms queue");
    m_sink = gst_element_factory_make("fakesink", "zms sink");

    g_object_set(
        m_appsrc,
        "stream-type", GST_APP_STREAM_TYPE_STREAM,
        "is-live", TRUE,
        "format", GST_FORMAT_TIME,
        NULL);

    GstCaps* caps = gst_caps_new_simple(
        "audio/x-raw",
        "format",    G_TYPE_STRING,  "S16LE",
        "layout",    G_TYPE_STRING,  "interleaved",
        "rate",      G_TYPE_INT,     16000,
        "channels",  G_TYPE_INT,     1,
        NULL);
    g_object_set(G_OBJECT(m_capsfilt), "caps", caps, NULL);
    gst_caps_unref(caps);

    g_object_set(
        m_queue,
        "max-size-buffers", 1,
        NULL);

    g_object_set(
        m_sink,
        "async", false,
        NULL);
    g_signal_connect(
        m_sink,
        "handoff",
        G_CALLBACK(NexeoZmsInbound::handleData),
        this);
    g_object_set(
        G_OBJECT(m_sink),
        "signal-handoffs", TRUE,
        "async", FALSE,
        NULL);

    // Put all elements in a bin.
    gst_bin_add_many(
        GST_BIN(m_pipeline),
        m_appsrc,
        m_capsfilt,
        m_conv,
        m_resample,
        m_queue,
        m_sink,
        NULL);

    // Link all the pipeline elements.
    gboolean result = gst_element_link_many(
        m_appsrc,
        m_capsfilt,
        m_conv,
        m_resample,
        m_queue,
        m_sink,
        NULL);

    if (result == FALSE)
    {
        throw result;
    }

    gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
}

// ---------------------------------------------------------------------------
// ZMS Message RX thread function.
// ---------------------------------------------------------------------------
void NexeoZmsInbound::rxMessage()
{
    zms::Message rxMsg;
    int retVal;

    while (m_run)
    {
        if ((retVal = mZmsAgent->recv(rxMsg, 10)) == zms::TIMEOUT)
        {
            retVal = zms::SUCCESS;
            continue;
        }
        else if (retVal != zms::SUCCESS )
        {
            warning("zms_inbound: Failed to recv msg: %d\n", retVal);
            break;
        }

        // If we got the target type message, pull the audio out.
        if (rxMsg.type == m_msg)
        {
            GstFlowReturn ret;
            GstBuffer* buf;

            buf = gst_buffer_new_allocate(
                NULL,
                rxMsg.index,
                NULL);
            gst_buffer_fill(buf, 0, (char*) rxMsg.data.data(), rxMsg.index);

            GST_BUFFER_TIMESTAMP (buf) = gst_util_uint64_scale_int(
                m_totalSamples,
                GST_SECOND,
                16000);
            GST_BUFFER_DURATION (buf) = gst_util_uint64_scale(
                rxMsg.index / 2,
                GST_SECOND,
                16000);

            ret = gst_app_src_push_buffer((GstAppSrc*) m_appsrc, buf);
            if (ret != GST_FLOW_OK)
            {
                warning("zms_inbound: push buffer failed: %d\n", ret);
            }
            else
            {
                m_totalSamples += rxMsg.index / 2;
            }

            continue;

#if 0
            //warning("zms_inbound: zms::MSG_AUD_LANE1_OUTBOUND\n");

            // copy the message data into an aubuf
            // this assumes the data is in the right format already
            // if not, we'd need to resample / conv / etc.
            //
            //
            //

            struct auframe af;
            af.fmt = AUFMT_RAW;
            af.srate = 0;
            af.sampv = (uint8_t *) rxMsg.data.data();
            af.sampc = rxMsg.index;
            af.timestamp = 0;
            af.level = AULEVEL_UNDEF;

            int err = aubuf_write_auframe(m_aubuf, &af);
            if (err)
            {
                warning("zms_inbound: aubuf_write failed: %m\n", err);
                break;
            }

            /*
            const struct timespec delay =
            {
                0,
                (long) m_prm.ptime * (1000000 / 2)
            };
            */

            playPacket();

            if (aubuf_cur_size(m_aubuf) < m_psize)
            {
                break;
            }

            //(void) nanosleep(&delay, NULL);
#endif //0

        }

        //warning("zms_inbound: got message %d, %d bytes\n", rxMsg.type, rxMsg.index);

        // Handle other message types.
        // TODO: if this is common type logic it can't be done here; then
        // subscribe and handle these messages somewhere else.
        switch (rxMsg.type)
        {
            default:
            {
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Reads inbound data and pushes it to baresip.
// TODO: not currently used as data is pushed into GST pipeline.
// ---------------------------------------------------------------------------
void NexeoZmsInbound::playPacket()
{
    struct auframe af;
    auframe_init(
        &af,
        AUFMT_S16LE,
        m_buf,
        m_sampc,
        m_prm.srate,
        m_prm.ch);

    // Timed read from audio-buffer.
    if (m_prm.ptime &&
        aubuf_get_samp(
            m_aubuf,
            m_prm.ptime,
            m_buf,
            m_sampc))
    {
        return;
    }

    // Immediate read from audio-buffer.
    if (!m_prm.ptime)
    {
        struct auframe af;
        af.fmt = AUFMT_S16LE,
        af.srate = 0,
        af.sampv = (uint8_t *) m_buf,
        af.sampc = m_sampc,
        af.timestamp = 0,
        af.level = AULEVEL_UNDEF;
        aubuf_read_auframe(m_aubuf, &af);
    }

    // Call read handler.
    if (m_rh)
    {
        m_rh(&af, m_arg);
    }
}

// ---------------------------------------------------------------------------
// Parses a device definition for the message id value.
// ---------------------------------------------------------------------------
void NexeoZmsInbound::parseDeviceMessage(const char* device)
{
    std::cmatch m;
    std::regex r("msg=(\\d{1,5})");

    if (!std::regex_search(device, m, r))
    {
        throw EINVAL;
    }

    // TODO: validate that the message id is acceptable (audio messages only!)
    auto in_msg = std::stoi(m.str(1));
    if (in_msg <= 0 || in_msg > zms::MSG_NUM_MAX)
    {
        throw EINVAL;
    }
    else
    {
        m_msg = in_msg;
    }

    info("zms_inbound: found msg id '%d' from device '%s'\n", m_msg, device);
}

