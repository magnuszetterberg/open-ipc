#include "wfb_core.hpp"

const char *wfb_status_name(wfb_status status)
{
    switch (status)
    {
    case WFB_OK:
        return "ok";
    case WFB_ERR_CONFIG:
        return "bad setting or key";
    case WFB_ERR_NO_MEMORY:
        return "out of memory";
    case WFB_ERR_CRYPTO:
        return "libsodium failed";
    case WFB_ERR_TOO_BIG:
        return "payload too big";
    case WFB_ERR_NOT_READY:
        return "not initialized";
    }
    return "unknown";
}
