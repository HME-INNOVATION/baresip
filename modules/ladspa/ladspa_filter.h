// -----------------------------------------------------------------------------
// LADSPA Filter Plugin
// -----------------------------------------------------------------------------

#pragma once

// System and Library Includes
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include <ladspa.h>
#include <map>
#include <string>
#include <vector>

struct LadspaControlPort
{
    std::string mName;
    std::string mConfigName;
    LADSPA_Data mValue;
    LADSPA_PortRangeHint mRange;
};

class LadspaFilter
{
public:
    static int create(
        struct aufilt_enc_st** stp,
        void** ctx,
        const struct aufilt* af,
        struct aufilt_prm* prm,
        const struct audio* au);

    static int encode(
        struct aufilt_enc_st* stp,
        struct auframe* af);

    ~LadspaFilter();

private:
    LadspaFilter(
        struct aufilt_prm* prm);

    int encode(
        struct auframe* af);
    int encode(
        LADSPA_Data* sampv,
        unsigned long sampc);

private:
    LADSPA_Handle mHandle;
    std::vector<unsigned long> mAudioPortsIn;
    std::vector<unsigned long> mAudioPortsOut;
    std::map<unsigned long, LadspaControlPort> mControlPortsIn;
    std::map<unsigned long, LadspaControlPort> mControlPortsOut;
};

struct LadspaState
{
    struct aufilt_enc_st af;    // Required by caller.
    LadspaFilter* state;        // Our state object.
};
