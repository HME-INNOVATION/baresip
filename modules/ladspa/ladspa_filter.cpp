// -----------------------------------------------------------------------------
// LADSPA filter plugin
// -----------------------------------------------------------------------------

// Class Interface
#include "ladspa_filter.h"

// System and Library Includes
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include <boost/algorithm/string.hpp>
#include <boost/core/ignore_unused.hpp>
#include <boost/dll/import.hpp>
#include <boost/filesystem.hpp>
#include <boost/function.hpp>
#include <ladspa.h>
#include <map>
#include <regex>
#include <string>
#include <vector>

// Globals
static boost::function<decltype(ladspa_descriptor)> gDescriptorsById;
static const LADSPA_Descriptor* gDescriptor = nullptr;


// -----------------------------------------------------------------------------
// Destructor handler for the LADSPA state struct.
// -----------------------------------------------------------------------------
static void ladspaStateDestructor(void* arg)
{
    auto st = static_cast<LadspaState*>(arg);
    delete st->state;
    list_unlink(&st->af.le);
}


// -----------------------------------------------------------------------------
// Free function to retrieve a config item as a float.
// -----------------------------------------------------------------------------
static int getConfigValue(const std::string& aConfigName, float& aValue)
{
    struct conf* conf = conf_cur();

    if (!conf || aConfigName.empty())
    {
        return EINVAL;
    }

    struct pl opt;
    int err = conf_get(conf, aConfigName.c_str(), &opt);
    if (err)
    {
        return err;
    }

    aValue = pl_float(&opt);

    debug("ladspa: configured '%s' = %f\n", aConfigName.c_str(), aValue);
    return 0;
}


// -----------------------------------------------------------------------------
// Free function to map port names to baresip config file entries.
// -----------------------------------------------------------------------------
static std::string portNameToConfigName(const std::string& aPortName)
{
    // NB: The logic here closely follows that of GST, with the exception
    // that the prefixes are different, and we use underscores instead of
    // dashes for property names.
    // See: https://github.com/GStreamer/gst-plugins-bad/blob/master/ext/ladspa/gstladspautils.c#L288 (gst_ladspa_object_class_get_param_name)

    // Start forming the config name with our plugin prefix and the
    // descriptor's label.
    std::string configName("ladspa_");
    configName += gDescriptor->Label;
    configName += "_";

    // Split the string on brackets and parentheses.
    std::vector<std::string> tokens;
    boost::split(tokens, aPortName, boost::is_any_of("[]()"));

    // Start forming the config name by stripping out text from
    // inside brackets and parentheses.
    for (size_t i = 0; i < tokens.size(); i++)
    {
        if (!(i % 2))
        {
            configName += tokens[i];
        }
    }

    // Trim off whitespace.
    boost::algorithm::trim(configName);

    // Lowercase the string.
    boost::algorithm::to_lower(configName);

    // Replace non-alphanumeric characters.
    std::regex alphanum(R"([^a-zA-Z0-9])");
    configName = std::regex_replace(configName, alphanum, "_");

    debug(
        "ladspa: port '%s' configurable with '%s'\n",
        aPortName.c_str(),
        configName.c_str());
    return configName;
}


// -----------------------------------------------------------------------------
// Factory method to create a LADSPA state object.
// -----------------------------------------------------------------------------
int LadspaFilter::create(
    struct aufilt_enc_st** stp,
    void** ctx,
    const struct aufilt* af,
    struct aufilt_prm* prm,
    const struct audio* au)
{
    boost::ignore_unused(ctx, af, au);
    if (!stp || !prm)
    {
        return EINVAL;
    }

    // Check the audio format.  LADPSA only supports floats for audio data, but
    // we can convert 16-bit values to floats using auconv functions.
    if (prm->fmt != AUFMT_S16LE && prm->fmt != AUFMT_FLOAT)
    {
        warning(
            "ladspa: unsupported sample format (%s)\n",
            aufmt_name((enum aufmt)prm->fmt));
        return ENOTSUP;
    }

    // Check if we have already been initialized.
    if (*stp)
    {
        return 0;
    }

    // Create a state object.
    auto st = static_cast<LadspaState*>(mem_zalloc(
        sizeof(LadspaState),
        ladspaStateDestructor));
    if (!st)
    {
        return ENOMEM;
    }

    // Instantiate the state filter object.
    try
    {
        st->state = new LadspaFilter(prm);
    }
    catch (int err)
    {
        delete st->state;
        mem_deref(st);
        return err;
    }
    catch (...)
    {
        delete st->state;
        mem_deref(st);
        return ENOMEM;
    }

    // Assign the state object and return successfully.
    *stp = reinterpret_cast<struct aufilt_enc_st*>(st);
    return 0;
}


// -----------------------------------------------------------------------------
// LADSPA filter constructor.
// -----------------------------------------------------------------------------
LadspaFilter::LadspaFilter(struct aufilt_prm* prm)
{
    debug("ladspa: filter constructor\n");

    boost::ignore_unused(prm);

    // Set up the LADSPA plugin.
    // TODO: Don't want to use a fixed sample rate here; but our plugin requires
    // 48 kHz, so it's currently hardcoded.
    mHandle = gDescriptor->instantiate(gDescriptor, 48000);
    if (!mHandle)
    {
        warning("ladspa: instantiate failed\n");
        throw ENOMEM;
    }

    // Check and configure the plugin's ports.
    for (unsigned long port = 0; port < gDescriptor->PortCount; port++)
    {
        auto portDescriptor = gDescriptor->PortDescriptors[port];

        // Get the indices of the audio ports.
        if (LADSPA_IS_PORT_AUDIO(portDescriptor))
        {
            if (LADSPA_IS_PORT_INPUT(portDescriptor))
            {
                mAudioPortsIn.push_back(port);
            }
            else
            {
                mAudioPortsOut.push_back(port);
            }
        }

        // Configure the control ports.
        else if (LADSPA_IS_PORT_CONTROL(portDescriptor))
        {
            // Control input ports.
            // NB: The values of these may be affected by the baresip
            // configuration file.  The corresponding config item names
            // are generated automatically from the port names.
            if (LADSPA_IS_PORT_INPUT(portDescriptor))
            {
                mControlPortsIn[port] = LadspaControlPort
                {
                    .mName = gDescriptor->PortNames[port],
                    .mConfigName = portNameToConfigName(gDescriptor->PortNames[port]),
                    .mValue = 0,
                    .mRange = gDescriptor->PortRangeHints[port]
                };

                // Populate the port's configuration, if available.
                // TODO: should check the value against the hint range.
                getConfigValue(
                    mControlPortsIn[port].mConfigName,
                    mControlPortsIn[port].mValue);

                gDescriptor->connect_port(
                    mHandle,
                    port,
                    &mControlPortsIn[port].mValue);
            }

            // Control output ports.
            else
            {
                mControlPortsOut[port] = LadspaControlPort
                {
                    .mName = gDescriptor->PortNames[port],
                    .mConfigName = "",
                    .mValue = 0,
                    .mRange = gDescriptor->PortRangeHints[port]
                };

                gDescriptor->connect_port(
                    mHandle,
                    port,
                    &mControlPortsOut[port].mValue);
            }
        }
    }

    // Check if this plugin provides audio ports.
    if (mAudioPortsIn.empty() || mAudioPortsOut.empty())
    {
        warning(
            "ladspa: descriptor (%s) does not provide required audio ports\n",
            gDescriptor->Label);
        throw ENOTSUP;
    }

    // Plugin setup is ready; active it to process audio.
    gDescriptor->activate(mHandle);
}


// -----------------------------------------------------------------------------
// LADSPA filter destructor.
// -----------------------------------------------------------------------------
LadspaFilter::~LadspaFilter()
{
    debug("ladspa: filter destructor\n");

    if (mHandle && gDescriptor->deactivate)
    {
        gDescriptor->deactivate(mHandle);
    }

    if (mHandle)
    {
        gDescriptor->cleanup(mHandle);
    }

    mHandle = nullptr;
}


// -----------------------------------------------------------------------------
// LADSPA filter encode static function.
// -----------------------------------------------------------------------------
int LadspaFilter::encode(struct aufilt_enc_st* stp, struct auframe* af)
{
    if (!stp || !af)
    {
        return EINVAL;
    }

    auto st = reinterpret_cast<LadspaState*>(stp);
    return st->state->encode(af);
}


// -----------------------------------------------------------------------------
// LADSPA filter encode converting function.
// -----------------------------------------------------------------------------
int LadspaFilter::encode(struct auframe* af)
{
    int err = 0;
    float* flt;
    switch (af->fmt)
    {
        case AUFMT_S16LE:
            // Convert from S16 to FLOAT
            flt = (float*) mem_alloc(af->sampc * sizeof(float), NULL);
            if (!flt)
            {
                return ENOMEM;
            }

            auconv_from_s16(
                AUFMT_FLOAT,
                flt,
                (int16_t*) af->sampv,
                af->sampc);

            // Process audio
            err = encode(flt, af->sampc);

            // Convert from FLOAT to S16
            auconv_to_s16(
                (int16_t*) af->sampv,
                AUFMT_FLOAT,
                flt,
                af->sampc);

            mem_deref(flt);
            break;

        case AUFMT_FLOAT:
            return encode((float*) af->sampv, af->sampc);

        default:
            return ENOTSUP;
    }

    return err;
}


// -----------------------------------------------------------------------------
// LADSPA filter encode function.
// -----------------------------------------------------------------------------
int LadspaFilter::encode(LADSPA_Data* sampv, size_t sampc)
{
    int err = 0;

    // Assign the memory location to the audio ports.
    // TODO: if there are more than one in/out port, we need to connect them!
    gDescriptor->connect_port(mHandle, mAudioPortsIn[0], sampv);
    gDescriptor->connect_port(mHandle, mAudioPortsOut[0], sampv);

    // Run the processing plugin.
    gDescriptor->run(mHandle, sampc);

    return err;
}


// -----------------------------------------------------------------------------
// LADSPA filter module function definitions.
// -----------------------------------------------------------------------------
static struct aufilt gLadspaFilter =
{
    .le      = LE_INIT,
    .name    = "ladspa",
    .encupdh = &LadspaFilter::create,
    .ench    = &LadspaFilter::encode,
    .decupdh = NULL,
    .dech    = NULL
};


// -----------------------------------------------------------------------------
// LADSPA filter module initialization.
// -----------------------------------------------------------------------------
static int moduleInit()
{
    // Get the config object.
    struct conf* conf = conf_cur();
    if (!conf)
    {
        return ENOMEM;
    }

    char temp[256] = "";

	// Get the config item for the shared object.
    if (conf_get_str(conf, "ladspa_target_library", temp, sizeof(temp)))
    {
        warning("ladspa: missing configuration; define 'ladspa_target_library'\n");
        return EINVAL;
    }
    std::string configTargetLibrary(temp);

	// Get the config item for the target descriptor.
    if (conf_get_str(conf, "ladspa_target_descriptor", temp, sizeof(temp)))
    {
        warning("ladspa: missing configuration; define 'ladspa_target_descriptor'\n");
        return EINVAL;
    }
    std::string configTargetDescriptor(temp);

    // Load the target LADSPA plugin.
    boost::dll::fs::path pluginPath("/usr/lib/ladspa");
    gDescriptorsById = boost::dll::import<decltype(ladspa_descriptor)>(
        pluginPath / configTargetLibrary,
        "ladspa_descriptor",
        boost::dll::load_mode::append_decorations);

    // Iterate through the plugin descriptors.
    unsigned long i = 0;
    while ((gDescriptor = gDescriptorsById(i++)))
    {
        // Check if this descriptor matches the target.
        if (gDescriptor->Label != configTargetDescriptor)
        {
            debug("ladspa: skipping descriptor (%s)\n", gDescriptor->Label);
            continue;
        }

        debug(
            "ladspa: found target plugin: %s (id: %u)\n",
            gDescriptor->Name,
            gDescriptor->UniqueID);
        break;
    }

    if (!gDescriptor)
    {
        warning("ladspa: failed to find target plugin\n");
        return ENOTSUP;
    }

    aufilt_register(baresip_aufiltl(), &gLadspaFilter);
    return 0;
}


// -----------------------------------------------------------------------------
// LADSPA filter module cleanup.
// -----------------------------------------------------------------------------
static int moduleClose()
{
    debug("ladspa: cleanup\n");

    // TODO: if possible, release shared object ref counter here?

    aufilt_unregister(&gLadspaFilter);
    return 0;
}


// -----------------------------------------------------------------------------
// LADSPA filter module exports.
// -----------------------------------------------------------------------------
extern "C" const struct mod_export DECL_EXPORTS(gLadspaFilterExports) =
{
    "ladspa",
    "aufilt",
    moduleInit,
    moduleClose
};

