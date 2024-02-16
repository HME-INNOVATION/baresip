/**
 * @file nexeo_ib_outbound.c
 * Nexeo IB audio, outbound (baresip -> RTP).
 */
#define _DEFAULT_SOURCE 1
#define _POSIX_C_SOURCE 199309L

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <gst/gst.h>
#include <unistd.h>
#include <re.h>
#include <rem.h>
#include <re_atomic.h>
#include <baresip.h>
#include "nexeo_ib.h"

#include <gobject/glib-types.h>
#include <gst/app/gstappsrc.h>
#include <gst/gstpipeline.h>

// ---------------------------------------------------------------------------
// IB Outbound Audio Device State
// ---------------------------------------------------------------------------
struct auplay_st
{
    // Playback parameters
    thrd_t thread;
    bool run;
    RE_ATOMIC bool needs_audio;
    bool eos;
    auplay_write_h* wh;
    void* arg;
    struct auplay_prm prm;
    size_t psize;
    size_t sampc;
    uint32_t ptime;
    int16_t* buf;
    int err;

    // Network parameters
    char* iface;
    char* ip;
    uint16_t port;

    // Gstreamer objects
    GstElement* pipeline;
    GstElement* appsrc;
    GstElement* capsfilt;
    GstElement* conv;
    GstElement* resample;
    GstElement* rtppay;
    GstElement* rtpbuf;
    GstElement* queue;
    GstElement* udpsink;
    uint64_t total_samples;
};

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Function Prototypes
// ---------------------------------------------------------------------------
static void ib_outbound_destructor(
    void* arg);
static int ib_outbound_setup(
    struct auplay_st* st);
static void ib_outbound_set_source_caps(
    struct auplay_st* st);
static GstBusSyncReply ib_outbound_bus_handler(
    GstBus* bus,
    GstMessage* msg,
    struct auplay_st* st);
static void ib_outbound_need_data(
    GstElement* pipeline,
    guint size,
    struct auplay_st* st);
static void ib_outbound_enough_data(
    GstElement* pipeline,
    guint size,
    struct auplay_st* st);
static int ib_outbound_thread(
    void* arg);

// ---------------------------------------------------------------------------
// Creates the audio device.
// ---------------------------------------------------------------------------
int ib_outbound_alloc(
    struct auplay_st** stp,
    const struct auplay* ap,
    struct auplay_prm* prm,
    const char* device,
    auplay_write_h* wh,
    void* arg)
{
    if (!stp || !ap || !prm || !wh)
    {
        return EINVAL;
    }

    if (!str_isset(device))
    {
        return EINVAL;
    }

    if (prm->fmt != AUFMT_S16LE)
    {
        warning("ib_outbound: unsupported sample format (%s)\n",
            aufmt_name(prm->fmt));
        return ENOTSUP;
    }

    struct auplay_st* st;
    st = mem_zalloc(sizeof(*st), ib_outbound_destructor);
    if (!st)
    {
        return ENOMEM;
    }

    st->wh = wh;
    st->arg = arg;

    int err = parse_device_interface(device, &st->iface);
    if (err)
    {
        goto out;
    }

    err = parse_device_ip(device, &st->ip);
    if (err)
    {
        goto out;
    }

    err = parse_device_port(device, &st->port);
    if (err)
    {
        goto out;
    }

    st->ptime = prm->ptime;
    if (!st->ptime)
    {
        st->ptime = 20;
    }

    if (!prm->srate)
    {
        prm->srate = 16000;
    }

    if (!prm->ch)
    {
        prm->ch = 1;
    }

    st->prm = *prm;
    st->sampc = prm->srate * prm->ch * st->ptime / 1000;
    st->psize = 2 * st->sampc;

    st->buf = mem_zalloc(st->psize, NULL);
    if (!st->buf)
    {
        err = ENOMEM;
        goto out;
    }

    err = ib_outbound_setup(st);
    if (err)
    {
        goto out;
    }

    re_atomic_rlx_set(&st->needs_audio, false);
    st->run = true;
    st->eos = false;

    gst_element_set_state(st->pipeline, GST_STATE_PLAYING);

    err = thread_create_name(
        &st->thread,
        "ib_outbound",
        ib_outbound_thread,
        st);
    if (err)
    {
        st->run = false;
        goto out;
    }

    if (!st->run)
    {
        err = st->err;
        goto out;
    }

 out:
    if (err)
    {
        mem_deref(st);
    }
    else
    {
        *stp = st;
    }

    return err;
}

// ---------------------------------------------------------------------------
// Destroys the audio device.
// ---------------------------------------------------------------------------
static void ib_outbound_destructor(void* arg)
{
    struct auplay_st* st = arg;

    re_atomic_rlx_set(&st->needs_audio, false);

    // Wait for termination of other thread.
    if (st->run)
    {
        st->run = false;
        debug("ib_outbound: stopping playback thread\n");
        thrd_join(st->thread, NULL);
    }

    gst_element_set_state(st->pipeline, GST_STATE_NULL);
    gst_object_unref(GST_OBJECT(st->pipeline));

    mem_deref(st->buf);
    mem_deref(st->iface);
    mem_deref(st->ip);
}

// ---------------------------------------------------------------------------
// Sets up the gst pipeline.
// ---------------------------------------------------------------------------
static int ib_outbound_setup(struct auplay_st* st)
{
    // Create and configure all elements.
    st->pipeline = gst_pipeline_new("ib_outbound pipeline");
    if (!st->pipeline)
    {
        warning("ib_outbound: failed to create pipeline element\n");
        return ENOMEM;
    }

    st->appsrc = gst_element_factory_make("appsrc", "ib_outbound src");
    if (!st->appsrc)
    {
        warning("ib_outbound: failed to create appsrc element\n");
        return ENOMEM;
    }
    g_object_set(
        st->appsrc,
        "stream-type", GST_APP_STREAM_TYPE_STREAM,
        "is-live", TRUE,
        "format", GST_FORMAT_TIME,
        NULL);
    g_signal_connect(
        st->appsrc,
        "need-data",
        G_CALLBACK(ib_outbound_need_data),
        st);
    g_signal_connect(
        st->appsrc,
        "enough-data",
        G_CALLBACK(ib_outbound_enough_data),
        st);

    st->capsfilt = gst_element_factory_make("capsfilter", "ib_outbound capsfilt");
    if (!st->capsfilt)
    {
        warning("ib_outbound: failed to create capsfilter element\n");
        return ENOMEM;
    }
    ib_outbound_set_source_caps(st);

    st->conv = gst_element_factory_make("audioconvert", "ib_outbound conv");
    if (!st->conv)
    {
        warning("ib_outbound: failed to create conv element\n");
        return ENOMEM;
    }

    st->resample = gst_element_factory_make("audioresample", "ib_outbound resample");
    if (!st->resample)
    {
        warning("ib_outbound: failed to create resample element\n");
        return ENOMEM;
    }

    st->rtppay = gst_element_factory_make("rtpL16pay", "ib_outbound rtppay");
    if (!st->rtppay)
    {
        warning("ib_outbound: failed to create rtppay element\n");
        return ENOMEM;
    }

    GstCaps* txCaps = gst_caps_new_simple(
        "application/x-rtp",
        "media", G_TYPE_STRING, "audio",
        "clock-rate", G_TYPE_INT, 16000,
        "encoding-name", G_TYPE_STRING, "L16",
        "encoding-params", G_TYPE_STRING, "1",
        "channels", G_TYPE_INT, 1,
        "payload", G_TYPE_INT, 96,
        NULL);

    st->rtpbuf = gst_element_factory_make("rtpjitterbuffer", "ib_outbound rtpbuf");
    if (!st->rtpbuf)
    {
        warning("ib_outbound: failed to create rtpbuf element\n");
        return ENOMEM;
    }
    g_object_set(
        st->rtpbuf,
        "latency", 100,
        "drop-on-latency", FALSE,
        NULL);

    st->queue = gst_element_factory_make("queue", "ib_outbound queue");
    if (!st->queue)
    {
        warning("ib_outbound: failed to create queue element\n");
        return ENOMEM;
    }
    g_object_set(
        st->queue,
        "max-size-buffers", 1,
        NULL);

    st->udpsink = gst_element_factory_make("udpsink", "ib_outbound udpsink");
    if (!st->udpsink)
    {
        warning("ib_outbound: failed to create udpsink element\n");
        return ENOMEM;
    }
    g_object_set(
        st->udpsink,
        "force-ipv4", TRUE,
        "sync", TRUE,
        "multicast-iface", st->iface,
        "host", st->ip,
        "port", st->port,
        NULL);

    // Put all elements in a bin.
    gst_bin_add_many(
        GST_BIN(st->pipeline),
        st->appsrc,
        st->capsfilt,
        st->conv,
        st->resample,
        st->rtppay,
        st->rtpbuf,
        st->queue,
        st->udpsink,
        NULL);

    // Link all the pipeline elements.
    gboolean result = gst_element_link_many(
        st->appsrc,
        st->capsfilt,
        st->conv,
        st->resample,
        st->rtppay,
        NULL);
    if (result == FALSE)
    {
        warning("ib_outbound: failed to link source elements\n");
        return ENOMEM;
    }

    result = gst_element_link_filtered(st->rtppay, st->rtpbuf, txCaps);
    if (result == FALSE)
    {
        warning("ib_outbound: failed to link rtppay -> rtpbuf\n");
        return ENOMEM;
    }

    result = gst_element_link_many(st->rtpbuf, st->queue, st->udpsink, NULL);
    if (result == FALSE)
    {
        warning("ib_outbound: failed to link sink elements\n");
        return ENOMEM;
    }

    // Set up bus callbacks.
    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(st->pipeline));
    gst_bus_enable_sync_message_emission(bus);
    gst_bus_set_sync_handler(
        bus, (GstBusSyncHandler) ib_outbound_bus_handler,
        st, NULL);
    gst_object_unref(bus);

    return 0;
}

// ---------------------------------------------------------------------------
// Filter the source data on the capabilities desired.
// ---------------------------------------------------------------------------
static void ib_outbound_set_source_caps(struct auplay_st* st)
{
    GstCaps* caps = gst_caps_new_simple(
        "audio/x-raw",
        "format",    G_TYPE_STRING,  "S16LE",
        "layout",    G_TYPE_STRING,  "interleaved",
        "rate",      G_TYPE_INT,     16000,
        "channels",  G_TYPE_INT,     1,
        NULL);

    g_object_set(G_OBJECT(st->capsfilt), "caps", caps, NULL);
    gst_caps_unref(caps);
}

// ---------------------------------------------------------------------------
// Handle gst bus messages.
// ---------------------------------------------------------------------------
static GstBusSyncReply ib_outbound_bus_handler(
    GstBus* bus,
    GstMessage* msg,
    struct auplay_st* st)
{
    (void) bus;

    GError* err;
    gchar* d;

    switch (GST_MESSAGE_TYPE(msg))
    {
        case GST_MESSAGE_EOS:
        {
            debug("ib_outbound: GST_MESSAGE_EOS\n");
            st->run = false;
            re_atomic_rlx_set(&st->needs_audio, false);
            st->eos = true;
            break;
        }

        case GST_MESSAGE_ERROR:
        {
            gst_message_parse_error(msg, &err, &d);

            warning("ib_outbound: GST_MESSAGE_ERROR: %d(%m) message=\"%s\"\n",
                err->code,
                err->code,
                err->message);
            warning("ib_outbound: Debug: %s\n", d);

            g_free(d);
            st->err = err->code;
            g_error_free(err);
            st->run = false;
            re_atomic_rlx_set(&st->needs_audio, false);
            break;
        }

        default:
        {
            break;
        }
    }

    gst_message_unref(msg);
    return GST_BUS_DROP;
}

// ---------------------------------------------------------------------------
// Handler indicating the pipeline needs data.
// ---------------------------------------------------------------------------
static void ib_outbound_need_data(
    GstElement* pipeline,
    guint size,
    struct auplay_st* st)
{
    (void) pipeline;
    (void) size;

    debug("ib_outbound: pipeline needs data\n");
    re_atomic_rlx_set(&st->needs_audio, true);
}

// ---------------------------------------------------------------------------
// Handler indicating the pipeline has enough data.
// ---------------------------------------------------------------------------
static void ib_outbound_enough_data(
    GstElement* pipeline,
    guint size,
    struct auplay_st* st)
{
    (void) pipeline;
    (void) size;

    debug("ib_outbound: pipeline has enough data\n");
    re_atomic_rlx_set(&st->needs_audio, false);
}

// ---------------------------------------------------------------------------
// Worker thread for pushing data into the gst pipeline.
// ---------------------------------------------------------------------------
static int ib_outbound_thread(void* arg)
{
    struct auplay_st* st = arg;

    GstFlowReturn ret;
    GstBuffer* buf;
    uint64_t t;
    int dt;
    uint32_t ptime = st->prm.ptime;
    int sample_count = st->prm.srate * st->prm.ptime / 1000;
    bool prime = true;

    while (st->run)
    {
        // Track the first frame each time we resume pushing audio.  We need
        // to prime the buffer before pushing audio.
        // TODO: is this needed?
        prime = true;

        t = tmr_jiffies();
        while (re_atomic_rlx(&st->needs_audio))
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
                    st->total_samples,
                    GST_SECOND,
                    16000);
                GST_BUFFER_DURATION(buf) = gst_util_uint64_scale(
                    sample_count,
                    GST_SECOND,
                    16000);

                ret = gst_app_src_push_buffer((GstAppSrc*) st->appsrc, buf);
                if (ret != GST_FLOW_OK)
                {
                    warning("ib_outbound: push buffer failed: %d\n", ret);
                }
                else
                {
                    st->total_samples += sample_count;
                }
            }

            struct auframe af;
            auframe_init(
                &af,
                st->prm.fmt,
                st->buf,
                st->sampc,
                st->prm.srate,
                st->prm.ch);
            af.timestamp = t * 1000;

            // Get the frame from the source.
            st->wh(&af, st->arg);

            // Audio buffer is populated from the source audio frame.
            buf = gst_buffer_new_allocate(
                NULL,
                sample_count * 2,
                NULL);
            gst_buffer_fill(buf, 0, (char*) st->buf, sample_count * 2);

            GST_BUFFER_TIMESTAMP (buf) = gst_util_uint64_scale_int(
                st->total_samples,
                GST_SECOND,
                16000);
            GST_BUFFER_DURATION (buf) = gst_util_uint64_scale(
                sample_count,
                GST_SECOND,
                16000);

            ret = gst_app_src_push_buffer((GstAppSrc*) st->appsrc, buf);
            if (ret != GST_FLOW_OK)
            {
                warning("ib_outbound: push buffer failed: %d\n", ret);
            }
            else
            {
                st->total_samples += sample_count;
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

    return 0;
}

