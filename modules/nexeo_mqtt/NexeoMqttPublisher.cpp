// Class Definition
#include "NexeoMqttPublisher.h"

// System and Library Includes
#include <re.h>
#include <baresip.h>
#include <json.hpp>
#include <list>
#include <string>
#include <sys/types.h>
#include <tuple>
#include <unistd.h>
#include <ZMS.hpp>
#include <ZMS_MsgTypes.h>
#include <zoltar_sids.h>

// Local Includes
#include "NexeoMqttConnection.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static const std::list<std::tuple<int, int>> kMessageSubscriptions =
{
//    { zms::MSG_CREW_GRP_TALK_START, ZMS_WILDCARD },
//    { zms::MSG_CREW_GRP_TALK_STOP, ZMS_WILDCARD },
    { zms::MSG_OT_TO_CT_START, ZMS_WILDCARD },
    { zms::MSG_OT_TO_CT_STOP, ZMS_WILDCARD },
    { /* HEADSET BUTTON EVENTS */ 1275, ZMS_WILDCARD }
};

// ---------------------------------------------------------------------------
// Constructor.
// ---------------------------------------------------------------------------
NexeoMqttPublisher::NexeoMqttPublisher(
    NexeoMqttConnection& aMqttConnection)
:   mMqttConnection(aMqttConnection),
    mRun(true)
{
    std::string connectionId =
        std::string("baresip mqtt_publish ") +
        std::to_string(getpid());

    mZmsAgent = std::make_shared<zms::LinuxAgent>(connectionId);
    if (mZmsAgent->init())
    {
        throw std::runtime_error("Could not create ZMS agent");
    }

    for (const auto& sub : kMessageSubscriptions)
    {
        auto type = std::get<0>(sub);
        auto index = std::get<1>(sub);

        if (mZmsAgent->subscribe(type, index))
        {
            throw std::runtime_error("Could not subscribe to message");
        }
    }

    mRxThread = std::make_shared<std::thread>(
        &NexeoMqttPublisher::rxMessage,
        this);
    pthread_setname_np(mRxThread->native_handle(), "mqtt_publish rx");
}

// ---------------------------------------------------------------------------
// Destructor.
// ---------------------------------------------------------------------------
NexeoMqttPublisher::~NexeoMqttPublisher()
{
    if (mRun)
    {
        mRun = false;
    }

    mRxThread->join();
}

// ---------------------------------------------------------------------------
// ZMS Message RX thread function.
// ---------------------------------------------------------------------------
void NexeoMqttPublisher::rxMessage()
{
    zms::Message rxMsg;
    int retVal;

    while (mRun)
    {
        if ((retVal = mZmsAgent->recv(rxMsg, 10)) == zms::TIMEOUT)
        {
            retVal = zms::SUCCESS;
            continue;
        }
        else if (retVal != zms::SUCCESS )
        {
            warning("mqtt_publish: Failed to recv msg: %d\n", retVal);
            break;
        }

        // Handle the message.
        nlohmann::json payload;
        switch (rxMsg.type)
        {
            case zms::MSG_OT_TO_CT_START:
            {
                auto messageData = nlohmann::json::parse(rxMsg.data);
                payload["headset_id"] = messageData["headset_id"];
                payload["lane"] = rxMsg.index;
                break;
            }

            case zms::MSG_OT_TO_CT_STOP:
            {
                auto messageData = nlohmann::json::parse(rxMsg.data);
                payload["headset_id"] = messageData["headset_id"];
                payload["lane"] = nullptr;
                break;
            }

            /*
            case zms::MSG_CREW_GRP_TALK_START:
            case zms::MSG_CREW_GRP_TALK_STOP:
            {
                auto messageData = nlohmann::json::parse(rxMsg.data);
                payload = messageData;
                payload["msg"] = rxMsg.type;
                payload["index"] = rxMsg.index;
                break;
            }
            */

            case /* HEADSET BUTTON EVENTS */ 1275:
            {
                auto messageData = nlohmann::json::parse(rxMsg.data);
                payload["headset_id"] = messageData["headset_id"];
                payload["button"] = messageData["button"];
                break;
            }

            default:
            {
                break;
            }
        }

        if (!payload.empty())
        {
            mMqttConnection.publish(payload);
        }
    }
}

