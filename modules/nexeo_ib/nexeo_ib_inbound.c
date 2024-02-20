/**
 * @file nexeo_ib_inbound.c
 * Nexeo IB audio, inbound (RTP -> baresip).
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
// IB Inbound Audio Device State
// ---------------------------------------------------------------------------
struct ausrc_st
{
    // Source parameters
    bool run;
    bool eos;
    ausrc_read_h* rh;
    ausrc_error_h* errh;
    void* arg;
    struct ausrc_prm prm;
    struct aubuf* aubuf;
    size_t psize;
    size_t sampc;
    uint32_t ptime;
    int16_t* buf;
    int err;
    struct tmr tmr;

    // Network parameters
    uint16_t port;

    // Gstreamer objects
    GstElement* pipeline;
    GstElement* udpsrc;
    GstElement* rtpdepay;
    GstElement* conv;
    GstElement* resample;
    GstElement* bin;
    GstElement* capsfilt;
    GstElement* sink;
};

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Function Prototypes
// ---------------------------------------------------------------------------
static void ib_inbound_destructor(
    void* arg);
static int ib_inbound_setup(
    struct ausrc_st* st);
static void ib_inbound_set_target_caps(
    struct ausrc_st* st);
static void ib_inbound_format_check(
    struct ausrc_st* st,
    GstStructure* s);
static void ib_inbound_play_packet(
    struct ausrc_st* st);
static void ib_inbound_packet_handler(
    struct ausrc_st* st,
    GstBuffer* buffer);
static void ib_inbound_sink_handoff(
    GstElement* sink,
    GstBuffer* buffer,
    GstPad* pad,
    gpointer user_data);
static GstBusSyncReply ib_inbound_bus_handler(
    GstBus* bus,
    GstMessage* msg,
    struct ausrc_st* st);
static void ib_inbound_timeout(
    void* arg);

// ---------------------------------------------------------------------------
// Creates the audio device.
// ---------------------------------------------------------------------------
int ib_inbound_alloc(
    struct ausrc_st** stp,
    const struct ausrc* as,
    struct ausrc_prm* prm,
    const char* device,
    ausrc_read_h* rh,
    ausrc_error_h* errh,
    void* arg)
{
    if (!stp || !as || !prm || !rh)
    {
        return EINVAL;
    }

    if (!str_isset(device))
    {
        return EINVAL;
    }

    if (prm->fmt != AUFMT_S16LE)
    {
        warning("ib_inbound: unsupported sample format (%s)\n",
            aufmt_name(prm->fmt));
        return ENOTSUP;
    }

    struct ausrc_st* st = mem_zalloc(sizeof(*st), ib_inbound_destructor);
    if (!st)
    {
        return ENOMEM;
    }

    st->rh = rh;
    st->arg = arg;

    int err = parse_device_port(device, &st->port);
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

    err = aubuf_alloc(&st->aubuf, 0, 0);
    if (err)
    {
        goto out;
    }

    err = ib_inbound_setup(st);
    if (err)
    {
        goto out;
    }

    st->run = true;
    st->eos = false;

    gst_element_set_state(st->pipeline, GST_STATE_PLAYING);

    if (!st->run)
    {
        err = st->err;
        goto out;
    }

    st->errh = errh;

    tmr_start(&st->tmr, st->ptime, ib_inbound_timeout, st);

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
static void ib_inbound_destructor(void* arg)
{
    struct ausrc_st* st = arg;

    if (st->run)
    {
        st->run = false;
    }

    tmr_cancel(&st->tmr);

    gst_element_set_state(st->pipeline, GST_STATE_NULL);
    gst_object_unref(GST_OBJECT(st->pipeline));

    mem_deref(st->aubuf);
    mem_deref(st->buf);
}

// ---------------------------------------------------------------------------
// Sets up the gst pipeline.
// ---------------------------------------------------------------------------
static int ib_inbound_setup(struct ausrc_st* st)
{
    // Create and configure all elements.
    st->pipeline = gst_pipeline_new("ib_inbound pipeline");
    if (!st->pipeline)
    {
        warning("ib_inbound: failed to create pipeline element\n");
        return ENOMEM;
    }

    st->udpsrc = gst_element_factory_make("udpsrc", "ib_inbound src");
    if (!st->udpsrc)
    {
        warning("ib_inbound: failed to create udpsrc element\n");
        return ENOMEM;
    }
    g_object_set(st->udpsrc, "port", st->port, NULL);

    GstCaps* rxCaps = gst_caps_new_simple(
        "application/x-rtp",
        "media", G_TYPE_STRING, "audio",
        "clock-rate", G_TYPE_INT, 16000,
        "encoding-name", G_TYPE_STRING, "L16",
        "encoding-params", G_TYPE_STRING, "1",
        "channels", G_TYPE_INT, 1,
        "payload", G_TYPE_INT, 96,
        NULL);

    st->rtpdepay = gst_element_factory_make("rtpL16depay", "ib_inbound rtpdepay");
    if (!st->rtpdepay)
    {
        warning("ib_inbound: failed to create rtpdepay element\n");
        return ENOMEM;
    }

    st->conv = gst_element_factory_make("audioconvert", "ib_inbound conv");
    if (!st->conv)
    {
        warning("ib_inbound: failed to create conv element\n");
        return ENOMEM;
    }

    st->resample = gst_element_factory_make("audioresample", "ib_inbound resample");
    if (!st->resample)
    {
        warning("ib_inbound: failed to create resample element\n");
        return ENOMEM;
    }

    st->bin = gst_bin_new("ib_inbound bin");
    if (!st->bin)
    {
        warning("ib_inbound: failed to create bin\n");
        return ENOMEM;
    }

    st->capsfilt = gst_element_factory_make("capsfilter", "ib_inbound capsfilt");
    if (!st->capsfilt)
    {
        warning("ib_inbound: failed to create capsfilter element\n");
        return ENOMEM;
    }
    ib_inbound_set_target_caps(st);

    st->sink = gst_element_factory_make("fakesink", "ib_inbound sink");
    if (!st->sink)
    {
        warning("ib_inbound: failed to create sink element\n");
        return ENOMEM;
    }
    g_object_set(
        st->sink,
        "async", false,
        NULL);
    g_signal_connect(
        st->sink,
        "handoff",
        G_CALLBACK(ib_inbound_sink_handoff),
        st);
    g_object_set(
	G_OBJECT(st->sink),
        "signal-handoffs", TRUE,
        "async", FALSE,
       	NULL);

    // Put all elements in a bin.
    gst_bin_add_many(
        GST_BIN(st->bin),
        st->capsfilt,
        st->sink,
        NULL);
    gst_bin_add_many(
        GST_BIN(st->pipeline),
        st->udpsrc,
        st->rtpdepay,
        st->conv,
        st->resample,
        st->bin,
        NULL);

    // Link all the pipeline elements.
    gboolean result = gst_element_link_many(st->capsfilt, st->sink, NULL);
    if (result == FALSE)
    {
        warning("ib_inbound: failed to link capsfilt -> sink\n");
        return ENOMEM;
    }

    GstPad* pad = gst_element_get_static_pad(st->capsfilt, "sink");
    gst_element_add_pad(st->bin, gst_ghost_pad_new("sink", pad));
    gst_object_unref(GST_OBJECT(pad));

    result = gst_element_link_filtered(st->udpsrc, st->rtpdepay, rxCaps);
    if (result == FALSE) {
        warning("ib_inbound: failed to link udpsrc -> rtpdepay\n");
        return ENOMEM;
    }

    result = gst_element_link_many(
        st->rtpdepay,
        st->conv,
        st->resample,
        st->bin,
        NULL);
    if (result == FALSE)
    {
        warning("ib_inbound: failed to link source elements\n");
        return ENOMEM;
    }

    // Set up bus callbacks.
    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(st->pipeline));
    gst_bus_enable_sync_message_emission(bus);
    gst_bus_set_sync_handler(
        bus, (GstBusSyncHandler) ib_inbound_bus_handler,
        st, NULL);
    gst_object_unref(bus);

    return 0;
}

// ---------------------------------------------------------------------------
// Filter the target data on the capabilities desired.
// ---------------------------------------------------------------------------
static void ib_inbound_set_target_caps(struct ausrc_st* st)
{
    GstCaps* caps = gst_caps_new_simple(
        "audio/x-raw",
        "format",   G_TYPE_STRING,  "S16LE",
        "rate",     G_TYPE_INT,     st->prm.srate,
        "channels", G_TYPE_INT,     st->prm.ch,
        NULL);

    g_object_set(G_OBJECT(st->capsfilt), "caps", caps, NULL);
    gst_caps_unref(caps);
}

// ---------------------------------------------------------------------------
// Handle gst bus messages.
// ---------------------------------------------------------------------------
static GstBusSyncReply ib_inbound_bus_handler(
    GstBus* bus,
    GstMessage* msg,
    struct ausrc_st* st)
{
    (void) bus;

    GError* err;
    gchar* d;

    switch (GST_MESSAGE_TYPE(msg))
    {
        case GST_MESSAGE_EOS:
        {
            debug("ib_inbound: GST_MESSAGE_EOS\n");
            st->run = false;
            st->eos = true;
            break;
        }

        case GST_MESSAGE_ERROR:
        {
            gst_message_parse_error(msg, &err, &d);

            warning("ib_inbound: GST_MESSAGE_ERROR: %d(%m) message=\"%s\"\n",
                err->code,
                err->code,
                err->message);
            warning("ib_inbound: Debug: %s\n", d);

            g_free(d);
            st->err = err->code;

            // Call error handler.
            if (st->errh)
            {
                st->errh(err->code, err->message, st->arg);
            }

            g_error_free(err);
            st->run = false;
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
// Checks that the data matches the expected pipeline format.
// ---------------------------------------------------------------------------
static void ib_inbound_format_check(struct ausrc_st* st, GstStructure* s)
{
    int rate, channels;

    if (!st || !s)
    {
        return;
    }

    gst_structure_get_int(s, "rate", &rate);
    gst_structure_get_int(s, "channels", &channels);

    if ((int)st->prm.srate != rate)
    {
        warning("ib_inbound: expected %u Hz (got %u Hz)\n", st->prm.srate,
            rate);
    }
    if (st->prm.ch != channels)
    {
        warning("ib_inbound: expected %d channels (got %d)\n",
            st->prm.ch, channels);
    }
}

// ---------------------------------------------------------------------------
// Pushes data to be played.
// ---------------------------------------------------------------------------
static void ib_inbound_play_packet(struct ausrc_st* st)
{
    struct auframe af;
    auframe_init(
        &af,
        AUFMT_S16LE,
        st->buf,
        st->sampc,
        st->prm.srate,
        st->prm.ch);

    // Timed read from audio-buffer.
    if (st->prm.ptime &&
        aubuf_get_samp(
            st->aubuf,
            st->prm.ptime,
            st->buf,
            st->sampc))
    {
        return;
    }

    // Immediate read from audio-buffer.
    if (!st->prm.ptime)
    {
        aubuf_read_samp(st->aubuf, st->buf, st->sampc);
    }

    // Call read handler.
    if (st->rh)
    {
        st->rh(&af, st->arg);
    }
}

// ---------------------------------------------------------------------------
// Handles playback of data ready from the pipeline.
// ---------------------------------------------------------------------------
static void ib_inbound_packet_handler(struct ausrc_st* st, GstBuffer* buffer)
{
    if (!st->run)
    {
        return;
    }

    GstMapInfo info;
    if (!gst_buffer_map(buffer, &info, GST_MAP_READ))
    {
        warning("ib_inbound: gst_buffer_map failed\n");
        return;
    }

    int err  = aubuf_write(st->aubuf, info.data, info.size);
    if (err)
    {
        warning("ib_inbound: aubuf_write failed: %m\n", err);
    }

    gst_buffer_unmap(buffer, &info);

    // Continue to process incoming audio.
    while (st->run)
    {
        const struct timespec delay =
        {
            0,
            st->prm.ptime * 1000000 / 2
        };

        ib_inbound_play_packet(st);

        if (aubuf_cur_size(st->aubuf) < st->psize)
        {
            break;
        }

        (void) nanosleep(&delay, NULL);
    }
}

// ---------------------------------------------------------------------------
// Callback to hand off audio from the pipeline for playback.
// ---------------------------------------------------------------------------
static void ib_inbound_sink_handoff(
    GstElement* sink,
    GstBuffer* buffer,
    GstPad* pad,
    gpointer user_data)
{
    (void)sink;

    struct ausrc_st* st = user_data;
    GstCaps* caps = gst_pad_get_current_caps(pad);
    ib_inbound_format_check(st, gst_caps_get_structure(caps, 0));
    gst_caps_unref(caps);

    ib_inbound_packet_handler(st, buffer);
}

// ---------------------------------------------------------------------------
// Timeout handler for pipeline errors.
// ---------------------------------------------------------------------------
static void ib_inbound_timeout(void* arg)
{
    struct ausrc_st* st = arg;
    tmr_start(&st->tmr, st->ptime ? st->ptime : 40, ib_inbound_timeout, st);

    // Check if the source is still running.
    if (!st->run)
    {
        tmr_cancel(&st->tmr);

        if (st->eos)
        {
            /* error handler must be called from re_main thread */
            if (st->errh)
            {
                st->errh(0, "end of file", st->arg);
            }
        }
    }
}

