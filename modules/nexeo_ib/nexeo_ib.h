/**
 * @file nexeo_ib.h
 * Nexeo IB audio.
 */
int ib_inbound_alloc(
    struct ausrc_st** stp,
    const struct ausrc* as,
    struct ausrc_prm* prm,
    const char* device,
    ausrc_read_h* rh,
    ausrc_error_h* errh,
    void* arg);
int ib_outbound_alloc(
    struct auplay_st** stp,
    const struct auplay* ap,
    struct auplay_prm* prm,
    const char* device,
    auplay_write_h* wh,
    void* arg);
int parse_device_interface(
    const char* device,
    char** interface);
int parse_device_ip(
    const char* device,
    char** ip);
int parse_device_port(
    const char* device,
    uint16_t* port);

