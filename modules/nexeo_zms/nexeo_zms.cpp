/**
 * @file nexeo_zms.cpp
 * Nexeo ZMS audio module.
 */
#define _DEFAULT_SOURCE 1
#define _POSIX_C_SOURCE 199309L

#include <stdlib.h>
#include <unistd.h>
#include <re.h>
#include <rem.h>
#undef restrict
#include <gst/gst.h>
#include <baresip.h>

#include "ZMS.hpp"

#include "nexeo_zms_inbound.h"
#include "nexeo_zms_outbound.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static struct ausrc* ausrc;
static struct auplay* auplay;

// ---------------------------------------------------------------------------
// Initializes the module.
// ---------------------------------------------------------------------------
static int module_zms_init(void)
{
    gst_init(0, NULL);
    zms::initializeZMQ();

    int err = ausrc_register(
        &ausrc,
        baresip_ausrcl(),
        "nexeo_zms",
        NexeoZmsInbound::create);

    err |= auplay_register(
        &auplay,
        baresip_auplayl(),
        "nexeo_zms",
        NexeoZmsOutbound::create);

    return err;
}

// ---------------------------------------------------------------------------
// De-initializes the module.
// ---------------------------------------------------------------------------
static int module_zms_close(void)
{
    ausrc = (struct ausrc*) mem_deref(ausrc);
    auplay = (struct auplay*) mem_deref(auplay);

    // NB: Don't deinit GST, it's handled automatically.

    return 0;
}

// ---------------------------------------------------------------------------
// Module definitions.
// ---------------------------------------------------------------------------
extern "C" EXPORT_SYM const struct mod_export DECL_EXPORTS(nexeo_zms) =
{
    "nexeo_zms",
    "sound",
    module_zms_init,
    module_zms_close
};

