/**
 * @file nexeo_zms_outbound.h
 * Nexeo ZMS audio, outbound (baresip -> ZMS/DSP).
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
#include <atomic>

#undef restrict
#include <gst/gst.h>
extern "C"
{
#include <gobject/glib-types.h>
#include <gst/app/gstappsrc.h>
#include <gst/gstpipeline.h>
}

class NexeoZmsOutbound
{
public:
    static int create(
        struct auplay_st** stp,
        const struct auplay* ap,
        struct auplay_prm* prm,
        const char* device,
        auplay_write_h* wh,
        void* arg);

private:
    NexeoZmsOutbound(
        const struct auplay* ap,
        struct auplay_prm* prm,
        const char* device,
        auplay_write_h* wh,
        void* arg);
    ~NexeoZmsOutbound();

    void setupPipeline();
    void handleSourceData();
    void sendMessage(
        GstBuffer* buffer);
    static void handleSinkData(
        GstElement* sink,
        GstBuffer* buffer,
        GstPad* pad,
        gpointer user_data);
    static void needData(
        GstElement* pipeline,
        guint size,
        NexeoZmsOutbound* st);
    static void enoughData(
        GstElement* pipeline,
        guint size,
        NexeoZmsOutbound* st);
    int parseDeviceHeadset(
        const char* device);
    int parseDeviceGroup(
        const char* device);

private:
    bool m_run;
    auplay_write_h* m_wh;
    void* m_arg;
    struct auplay_prm m_prm;
    size_t m_psize;
    size_t m_sampc;
    uint32_t m_ptime;
    int16_t* m_buf;
    int m_ppid;
    int m_group;
    std::shared_ptr<zms::LinuxAgent> mZmsAgent;
    std::shared_ptr<std::thread> mDataThread;
    std::atomic<bool> m_needs_audio;

    // Gstreamer objects
    GstElement* m_pipeline;
    GstElement* m_appsrc;
    GstElement* m_capsfilt;
    GstElement* m_conv;
    GstElement* m_resample;
    GstElement* m_queue;
    GstElement* m_sink;

    std::vector<uint8_t> m_buffer; // temporary, this is a bad way to buffer
    uint64_t m_totalSamples;
};

