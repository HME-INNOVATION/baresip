// Class Definition
#include "NexeoMqttSubscriber.h"

// System and Library Includes
#include <re.h>
#include <baresip.h>
#include "json.hpp"
#include <list>
#include <regex>
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
static const std::regex kRegexGroupJoin("/join_group$");
static const std::regex kRegexGroupLeave("/leave_group$");
static const std::regex kRegexGroupsQuery("/get_groups$");
static const std::regex kRegexGroupsAssign("/set_groups$");

// ---------------------------------------------------------------------------
// Constructor.
// ---------------------------------------------------------------------------
NexeoMqttSubscriber::NexeoMqttSubscriber(
    NexeoMqttConnection& aMqttConnection)
:   mMqttConnection(aMqttConnection)
{
    std::string connectionId =
        std::string("baresip mqtt_subscribe ") +
        std::to_string(getpid());

    mZmsAgent = std::make_shared<zms::LinuxAgent>(connectionId);
    if (mZmsAgent->init())
    {
        throw std::runtime_error("Could not create ZMS agent");
    }

    mMqttConnection.setSubscriber(this);
}

// ---------------------------------------------------------------------------
// Destructor.
// ---------------------------------------------------------------------------
NexeoMqttSubscriber::~NexeoMqttSubscriber()
{
    mMqttConnection.setSubscriber(nullptr);
}

// ---------------------------------------------------------------------------
// MQTT message receive handler.
// ---------------------------------------------------------------------------
void NexeoMqttSubscriber::rxMessage(
    const std::string& aTopic,
    const std::string& aMessage)
{
    debug(
        "nexeo_mqtt: Got message: '%s' '%s'\n",
        aTopic.c_str(),
        aMessage.c_str());

    auto messageData = nlohmann::json::parse(aMessage);

    // TODO: optimize perf (more general regex + map + switch)
    if (std::regex_search(aTopic, kRegexGroupJoin))
    {
        groupJoin(messageData);
    }
    else if (std::regex_search(aTopic, kRegexGroupLeave))
    {
        groupLeave(messageData);
    }
    else if (std::regex_search(aTopic, kRegexGroupsQuery))
    {
        groupsQuery(messageData);
    }
    else if (std::regex_search(aTopic, kRegexGroupsAssign))
    {
        groupsAssign(messageData);
    }
}

// ---------------------------------------------------------------------------
// Message handler: single headset group join.
// ---------------------------------------------------------------------------
void NexeoMqttSubscriber::groupJoin(
    const nlohmann::json& aData)
{
    if (!aData.is_object() ||
        !aData.contains("headset_id") ||
        !aData["headset_id"].is_number_unsigned() ||
        !aData.contains("group_id") ||
        !aData["group_id"].is_number_unsigned())
    {
        warning("nexeo_mqtt: groupJoin received invalid message\n");
        return;
    }

    auto headset_id = aData["headset_id"].template get<unsigned int>();
    auto group_id = aData["group_id"].template get<unsigned int>();

    // TODO: declare constants somewhere
    if (headset_id > 100 || group_id > 10)
    {
        warning("nexeo_mqtt: groupJoin received invalid data\n");
        return;
    }

    debug(
        "nexeo_mqtt: groupJoin: headset %d group %d\n",
        headset_id,
        group_id);

    // TODO: create message structure
    zms::Message message;
    message.type = 1260; // MSG_BOSS_GROUP_JOIN
    message.data = std::string(2 * sizeof(uint8_t), 0);
    message.data[0] = headset_id;
    message.data[1] = group_id;
    message.index = 2 * sizeof(uint8_t);

    auto ret = mZmsAgent->send(message);
    if (ret != zms::SUCCESS)
    {
        warning("nexeo_mqtt: groupJoin: unable to send message: %d\n", ret);
    }
}

// ---------------------------------------------------------------------------
// Message handler: single headset group leave.
// ---------------------------------------------------------------------------
void NexeoMqttSubscriber::groupLeave(
    const nlohmann::json& aData)
{
    if (!aData.is_object() ||
        !aData.contains("headset_id") ||
        !aData["headset_id"].is_number_unsigned())
    {
        warning("nexeo_mqtt: groupLeave received invalid message\n");
        return;
    }

    auto headset_id = aData["headset_id"].template get<unsigned int>();

    // TODO: declare constants somewhere
    if (headset_id > 100)
    {
        warning("nexeo_mqtt: groupLeave received invalid data\n");
        return;
    }

    debug("nexeo_mqtt: groupLeave: headset %d\n", headset_id);

    // TODO: create message structure
    zms::Message message;
    message.type = 1261; // MSG_BOSS_GROUP_LEAVE
    message.data = std::string(1 * sizeof(uint8_t), 0);
    message.data[0] = headset_id;
    message.index = 1 * sizeof(uint8_t);

    auto ret = mZmsAgent->send(message);
    if (ret != zms::SUCCESS)
    {
        warning("nexeo_mqtt: groupLeave: unable to send message: %d\n", ret);
    }
}

// ---------------------------------------------------------------------------
// Message handler: groups data query.
// ---------------------------------------------------------------------------
void NexeoMqttSubscriber::groupsQuery(
    const nlohmann::json& aData)
{
    // Message payload is not currently used.
    (void) aData;

    debug("nexeo_mqtt: groupsQuery\n");

    zms::Message message;
    message.type = 1262; // MSG_BOSS_GROUPS_QUERY
    message.index = 0;

    auto ret = mZmsAgent->send(message);
    if (ret != zms::SUCCESS)
    {
        warning("nexeo_mqtt: groupsQuery: unable to send message: %d\n", ret);
    }
}

// ---------------------------------------------------------------------------
// Message handler: bulk group assignment.
// ---------------------------------------------------------------------------
void NexeoMqttSubscriber::groupsAssign(
    const nlohmann::json& aData)
{
    if (!aData.is_array())
    {
        warning("nexeo_mqtt: groupsAssign received invalid message\n");
        return;
    }

    debug("nexeo_mqtt: groupsAssign\n");

    // TODO: create message structure
    zms::Message message;
    message.type = 1264; // MSG_BOSS_GROUPS_ASSIGN
    message.index = 0;
    message.data = std::string(1 + 2 * aData.size(), 0);

    size_t offset = 1;
    for (auto& item : aData.items())
    {
        if (!item.value().is_object() ||
            !item.value().contains("headset_id") ||
            !item.value()["headset_id"].is_number_unsigned() ||
            !item.value().contains("group_id") ||
            !item.value()["group_id"].is_number_unsigned())
        {
            warning(
                "nexeo_mqtt: groupsAssign skipping invalid array item: %s\n",
                item.value().dump(-1, ' ', true).c_str());
            continue;
        }

        auto headset_id =
            item.value()["headset_id"].template get<unsigned int>();
        auto group_id =
            item.value()["group_id"].template get<unsigned int>();

        // TODO: declare constants somewhere
        if (headset_id > 100 || group_id > 10)
        {
            warning("nexeo_mqtt: groupsAssign received invalid data\n");
            continue;
        }

        // Store the group assignment.
        message.data[offset++] = headset_id;
        message.data[offset++] = group_id;
    }

    // Store the assignment count.
    message.data[0] = (offset - 1) / 2;

    auto ret = mZmsAgent->send(message);
    if (ret != zms::SUCCESS)
    {
        warning("nexeo_mqtt: groupsAssign: unable to send message: %d\n", ret);
    }
}

