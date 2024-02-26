/**
 * @file nexeo_zms.cpp
 * Nexeo MQTT module.
 */
#define _DEFAULT_SOURCE 1
#define _POSIX_C_SOURCE 199309L

#include <stdlib.h>
#include <unistd.h>
#include <re.h>
#include <baresip.h>

#include <ZMS.hpp>

#include "NexeoMqttConnection.h"
#include "NexeoMqttPublisher.h"

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static NexeoMqttConnection* gConnection = nullptr;
static NexeoMqttPublisher* gPublisher = nullptr;

// ---------------------------------------------------------------------------
// Initializes the module.
// ---------------------------------------------------------------------------
static int module_nexeo_mqtt_init(void)
{
    zms::initializeZMQ();

    try
    {
        gConnection = new NexeoMqttConnection();
        gPublisher = new NexeoMqttPublisher(*gConnection);
    }
    catch (std::exception& e)
    {
        warning("nexeo_mqtt: init failed: %s\n", e.what());
        return ENOMEM;
    }

    return 0;
}

// ---------------------------------------------------------------------------
// De-initializes the module.
// ---------------------------------------------------------------------------
static int module_nexeo_mqtt_close(void)
{
    delete gPublisher;
    delete gConnection;

    return 0;
}

// ---------------------------------------------------------------------------
// Module definitions.
// ---------------------------------------------------------------------------
extern "C" EXPORT_SYM const struct mod_export DECL_EXPORTS(nexeo_mqtt) =
{
    "nexeo_mqtt",
    "application",
    module_nexeo_mqtt_init,
    module_nexeo_mqtt_close
};

