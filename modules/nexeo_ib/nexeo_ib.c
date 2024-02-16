/**
 * @file nexeo_ib.c
 * Nexeo IB audio module.
 */
#define _DEFAULT_SOURCE 1
#define _POSIX_C_SOURCE 199309L

#include <stdlib.h>
#include <unistd.h>
#include <gst/gst.h>
#include <re.h>
#include <baresip.h>
#include "nexeo_ib.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static const char* kRegexNetworkInterface = "iface=([a-zA-Z0-9.\\-]+)";
static const char* kRegexIpAddress = "ip=((?:[0-9]{1,3}\\.){3}[0-9]{1,3})";
static const char* kRegexPort = "port=([0-9]{1,5})";

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static struct ausrc*  ausrc;
static struct auplay* auplay;

// ---------------------------------------------------------------------------
// Initializes the module.
// ---------------------------------------------------------------------------
static int module_ib_init(void)
{
    gst_init(0, NULL);

    int err = ausrc_register(
        &ausrc,
        baresip_ausrcl(),
        "nexeo_ib",
        ib_inbound_alloc);

    err |= auplay_register(
        &auplay,
        baresip_auplayl(),
        "nexeo_ib",
        ib_outbound_alloc);

    return err;
}

// ---------------------------------------------------------------------------
// De-initializes the module.
// ---------------------------------------------------------------------------
static int module_ib_close(void)
{
    ausrc = mem_deref(ausrc);
    auplay = mem_deref(auplay);

    // NB: Don't deinit GST, it's handled automatically.

    return 0;
}

// ---------------------------------------------------------------------------
// Parses a device definition for the network interface value.
// ---------------------------------------------------------------------------
int parse_device_interface(const char* device, char** interface)
{
    int err = 0;

    gchar** match = g_regex_split_simple(
        kRegexNetworkInterface,
        device,
        0,
        G_REGEX_MATCH_NOTEMPTY);

    if (match[1] != NULL && match[2] != NULL && strlen(match[1]))
    {
        err = str_dup(interface, match[1]);
        debug("ib: found interface '%s' from device '%s'\n", *interface, device);
    }
    else
    {
        *interface = NULL;
    }

    g_strfreev(match);
    return err;
}

// ---------------------------------------------------------------------------
// Parses a device definition for the IP address value.
// ---------------------------------------------------------------------------
int parse_device_ip(const char* device, char** ip)
{
    int err = 0;

    gchar** match = g_regex_split_simple(
        kRegexIpAddress,
        device,
        0,
        G_REGEX_MATCH_NOTEMPTY);

    if (match[1] != NULL && match[2] != NULL && strlen(match[1]))
    {
        err = str_dup(ip, match[1]);
        debug("ib: found IP '%s' from device '%s'\n", *ip, device);
    }
    else
    {
        *ip = NULL;
    }

    g_strfreev(match);
    return err;
}

// ---------------------------------------------------------------------------
// Parses a device definition for the port value.
// ---------------------------------------------------------------------------
int parse_device_port(const char* device, uint16_t* port)
{
    int err = 0;

    gchar** match = g_regex_split_simple(
        kRegexPort,
        device,
        0,
        G_REGEX_MATCH_NOTEMPTY);

    if (match[1] != NULL && match[2] != NULL && strlen(match[1]))
    {
        int in_port = atoi(match[1]);
        if (in_port <= 0 || in_port > 65535)
        {
            err = EINVAL;
        }
        else
        {
            *port = in_port;
            debug("ib: found port '%d' from device '%s'\n", *port, device);
        }
    }

    g_strfreev(match);
    return err;
}

// ---------------------------------------------------------------------------
// Module definitions.
// ---------------------------------------------------------------------------
EXPORT_SYM const struct mod_export DECL_EXPORTS(nexeo_ib) =
{
    "nexeo_ib",
    "sound",
    module_ib_init,
    module_ib_close
};

