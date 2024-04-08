/**
 * @file nexeo_zms_outbound.cpp
 * Nexeo ZMS audio, outbound (baresip -> ZMS/DSP).
 */

// Class Definition
#include "nexeo_zms_outbound.h"

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
#define INBOUND_FRAME_SIZE   960     // 30ms

// ---------------------------------------------------------------------------
// Creates the audio device.
// ---------------------------------------------------------------------------
int NexeoZmsOutbound::create(
    struct auplay_st** stp,
    const struct auplay* ap,
    struct auplay_prm* prm,
    const char* device,
    auplay_write_h* wh,
    void* arg)
{
    if (!stp)
    {
        return EINVAL;
    }

    NexeoZmsOutbound** st = static_cast<NexeoZmsOutbound**>(
        mem_alloc(
            sizeof(NexeoZmsOutbound*),
            [](void* arg)
            {
                delete *static_cast<NexeoZmsOutbound**>(arg);
            }));
    if (!st)
    {
        return ENOMEM;
    }

    try
    {
        *st = new NexeoZmsOutbound(ap, prm, device, wh, arg);
    }
    catch (int err)
    {
        delete *st;
        mem_deref(st);
        return err;
    }

    *stp = reinterpret_cast<struct auplay_st*>(st);
    return 0;
}

// ---------------------------------------------------------------------------
// Constructor.
// ---------------------------------------------------------------------------
NexeoZmsOutbound::NexeoZmsOutbound(
    const struct auplay* ap,
    struct auplay_prm* prm,
    const char* device,
    auplay_write_h* wh,
    void* arg)
{
    if (!ap || !prm || !wh || !device)
    {
        throw EINVAL;
    }

    if (!str_isset(device))
    {
        throw EINVAL;
    }

    if (prm->fmt != AUFMT_S16LE)
    {
        warning("zms_outbound: unsupported sample format (%s)\n",
            aufmt_name((enum aufmt) prm->fmt));
        throw ENOTSUP;
    }

    m_wh = wh;
    m_arg = arg;
    parseDeviceHeadset(device);

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

    m_needs_audio = false;
    m_run = true;

    std::string connectionId =
        std::string("baresip zms_outbound") +
        std::string(device);

    mZmsAgent = std::make_shared<zms::LinuxAgent>(connectionId);
    auto err = mZmsAgent->init();
    if (err)
    {
        warning("zms_outbound: ZMS init failed: %d\n", err);
        throw err;
    }

    setupPipeline();

    mDataThread = std::make_shared<std::thread>(&NexeoZmsOutbound::handleSourceData, this);
    pthread_setname_np(mDataThread->native_handle(), "zms_outbound srcdata");
}

// ---------------------------------------------------------------------------
// Destructor.
// ---------------------------------------------------------------------------
NexeoZmsOutbound::~NexeoZmsOutbound()
{
    m_run = false;
    m_needs_audio = false;

    gst_element_set_state(m_pipeline, GST_STATE_NULL);

    mDataThread->join();

    gst_object_unref(GST_OBJECT(m_pipeline));
    mem_deref(m_buf);
}


// ---------------------------------------------------------------------------
// Handles message transmission from data ready from the pipeline.
// ---------------------------------------------------------------------------
void NexeoZmsOutbound::sendMessage(GstBuffer* buffer)
{
    if (!m_run)
    {
        return;
    }

    auto datalen = gst_buffer_get_size(buffer);
    auto offset = m_buffer.size();
    m_buffer.resize(offset + datalen, 0); //grow the buffer to hold new data
    gst_buffer_extract(buffer, 0, m_buffer.data() + offset, datalen);

    if (m_buffer.size() < INBOUND_FRAME_SIZE)
    {
        debug("zms_outbound: trying to send message but buffer is too small: %d\n", m_buffer.size());
        return;
    }

    auto framesToSend = m_buffer.size() / INBOUND_FRAME_SIZE;
    framesToSend = ( framesToSend > 3 ) ? 3 : framesToSend;

    // Form a ZMS message from the buffer and send it
    zms::Message txMsg;
    txMsg.type = 401; // MSG_AUD_BOSS_HEADSET_TX
    txMsg.index = framesToSend * INBOUND_FRAME_SIZE + 4;

    // TODO: use GST queues to buffer data, prevent more copies
    //txMsg.data = std::string(txMsg.index, 0); // alloc a string buffer
    //gst_buffer_extract(buffer, 0, &txMsg.data.data()[4], txMsg.index);

    txMsg.data = std::string(txMsg.index, 0);
    txMsg.data[0] = m_ppid;
    memcpy(&txMsg.data[4], m_buffer.data(), txMsg.index - 4);

    auto retVal = mZmsAgent->send(txMsg);
    if (retVal != zms::SUCCESS)
    {
        warning(
            "zms_outbound: send (%d bytes) failed: %d\n",
            txMsg.index,
            retVal);
    }

    m_buffer.erase(m_buffer.begin(), m_buffer.begin() + txMsg.index - 4);
}

// ---------------------------------------------------------------------------
// Handles sink data from GST, sending ZMS messages.
// ---------------------------------------------------------------------------
void NexeoZmsOutbound::handleSinkData(
    GstElement* sink,
    GstBuffer* buffer,
    GstPad* pad,
    gpointer user_data)
{
    (void) sink;
    (void) pad;

    NexeoZmsOutbound* st = (NexeoZmsOutbound*) user_data;
    if (!st)
    {
        warning("zms_outbound: handleSinkData: invalid pointer!");
        return;
    }

    st->sendMessage(buffer);
}

// ---------------------------------------------------------------------------
// Callback to indicate that the pipeline needs data.
// ---------------------------------------------------------------------------
void NexeoZmsOutbound::needData(
    GstElement* pipeline,
    guint size,
    NexeoZmsOutbound* st)
{
    (void) pipeline;
    (void) size;

    if (st->m_run)
    {
        st->m_needs_audio = true;
    }
}

// ---------------------------------------------------------------------------
// Callback to indicate that the pipeline has enough data.
// ---------------------------------------------------------------------------
void NexeoZmsOutbound::enoughData(
    GstElement* pipeline,
    guint size,
    NexeoZmsOutbound* st)
{
    (void) pipeline;
    (void) size;

    if (!st)
    {
        warning("zms_outbound: enoughData: invalid pointer!");
        return;
    }

    st->m_needs_audio = false;
}

// ---------------------------------------------------------------------------
// Creates and configures the pipeline elements.
// ---------------------------------------------------------------------------
void NexeoZmsOutbound::setupPipeline()
{
    // Create and configure all elements.
    // TODO: error checking
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
    g_signal_connect(
        m_appsrc,
        "need-data",
        G_CALLBACK(NexeoZmsOutbound::needData),
        this);
    g_signal_connect(
        m_appsrc,
        "enough-data",
        G_CALLBACK(NexeoZmsOutbound::enoughData),
        this);

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
        G_CALLBACK(NexeoZmsOutbound::handleSinkData),
        this);
    g_object_set(
        G_OBJECT(m_sink),
        "signal-handoffs", TRUE,
        "async", FALSE,
        NULL);

    m_buffer.reserve(6400); // enough for 200ms of audio data

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
// Handles source data from baresip, injecting it into the GST pipeline.
// ---------------------------------------------------------------------------
void NexeoZmsOutbound::handleSourceData()
{
    bool prime = true;
    uint64_t t;
    GstFlowReturn ret;
    GstBuffer* buf;
    int dt;
    uint32_t ptime = m_prm.ptime;
    int sample_count = m_prm.srate * m_prm.ptime / 1000;

    while (m_run)
    {
        prime = true;
        t = tmr_jiffies();

        while (m_needs_audio)
        {
            if (prime)
            {
                // Only prime once per inner loop.
                prime = false;

                // Priming buffer is all zeros.
                buf = gst_buffer_new_allocate(
                    NULL,
                    sample_count * 2,
                    NULL);
                gst_buffer_memset(buf, 0, 0, sample_count * 2);

                GST_BUFFER_TIMESTAMP(buf) = gst_util_uint64_scale_int(
                    m_totalSamples,
                    GST_SECOND,
                    16000);
                GST_BUFFER_DURATION(buf) = gst_util_uint64_scale(
                    sample_count,
                    GST_SECOND,
                    16000);

                ret = gst_app_src_push_buffer((GstAppSrc*) m_appsrc, buf);
                if (ret != GST_FLOW_OK)
                {
                    warning("zms_outbound: push buffer failed: %d\n", ret);
                }
                else
                {
                    m_totalSamples += sample_count;
                }
            }

            struct auframe af;
            auframe_init(
                &af,
                (aufmt) m_prm.fmt,
                m_buf,
                m_sampc,
                m_prm.srate,
                m_prm.ch);
            af.timestamp = t * 1000;

            // Get the frame from the source.
            m_wh(&af, m_arg);

            // Audio buffer is populated from the source audio frame.
            buf = gst_buffer_new_allocate(
                NULL,
                sample_count * 2,
                NULL);
            gst_buffer_fill(buf, 0, (char*) m_buf, sample_count * 2);

            GST_BUFFER_TIMESTAMP (buf) = gst_util_uint64_scale_int(
                m_totalSamples,
                GST_SECOND,
                16000);
            GST_BUFFER_DURATION (buf) = gst_util_uint64_scale(
                sample_count,
                GST_SECOND,
                16000);

            ret = gst_app_src_push_buffer((GstAppSrc*) m_appsrc, buf);
            if (ret != GST_FLOW_OK)
            {
                warning("zms_outbound: push buffer failed: %d\n", ret);
            }
            else
            {
                m_totalSamples += sample_count;
            }

            // Sleep while the current buffer plays.
            t += ptime;
            dt = (int) (t - tmr_jiffies());
            if (dt > 2)
            {
                sys_msleep(dt);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Parses a device definition for the headset id value.
// ---------------------------------------------------------------------------
void NexeoZmsOutbound::parseDeviceHeadset(const char* device)
{
    std::cmatch m;
    std::regex r("ppid=(\\d{1,2})");

    if (!std::regex_search(device, m, r))
    {
        throw EINVAL;
    }

    // TODO: validate that the headset id is acceptable (1-99 only!)
    auto in_ppid = std::stoi(m.str(1));
    if (in_ppid <= 0 || in_ppid > 99)
    {
        throw EINVAL;
    }
    else
    {
        m_ppid = in_ppid;
    }

    info("zms_outbound: found ppid '%d' from device '%s'\n", m_ppid, device);
}

