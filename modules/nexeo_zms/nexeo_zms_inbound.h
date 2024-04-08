/**
 * @file nexeo_zms_inbound.h
 * Nexeo ZMS audio, inbound (ZMS/DSP -> baresip).
 */

#pragma once

#include <stdlib.h>
#include <unistd.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include <memory>
#include <ZMS.hpp>
#include <thread>

#undef restrict
#include <gst/gst.h>
extern "C"
{
#include <gobject/glib-types.h>
#include <gst/app/gstappsrc.h>
#include <gst/gstpipeline.h>
}

class NexeoZmsInbound
{
public:
    static int create(
        struct ausrc_st** stp,
        const struct ausrc* as,
        struct ausrc_prm* prm,
        const char* device,
        ausrc_read_h* rh,
        ausrc_error_h* errh,
        void* arg);

private:
    NexeoZmsInbound(
        const struct ausrc* as,
        struct ausrc_prm* prm,
        const char* device,
        ausrc_read_h* rh,
        ausrc_error_h* errh,
        void* arg);
    ~NexeoZmsInbound();

    void setupPipeline();
    void rxMessage();
    void playPacket();
    void packetHandler(GstBuffer* buffer);
    static void handleData(
        GstElement* sink,
        GstBuffer* buffer,
        GstPad* pad,
        gpointer user_data);
    void parseDeviceHeadset(
        const char* device);

private:
    bool m_run;
    ausrc_read_h* m_rh;
    void* m_arg;
    struct ausrc_prm m_prm;
    struct aubuf* m_aubuf;
    size_t m_psize;
    size_t m_sampc;
    uint32_t m_ptime;
    int16_t* m_buf;
    int m_ppid;
    std::shared_ptr<zms::LinuxAgent> mZmsAgent;
    std::shared_ptr<std::thread> mRxThread;

    // Gstreamer objects
    GstElement* m_pipeline;
    GstElement* m_appsrc;
    GstElement* m_capsfilt;
    GstElement* m_conv;
    GstElement* m_resample;
    GstElement* m_queue;
    GstElement* m_sink;

    uint64_t m_totalSamples;
};

