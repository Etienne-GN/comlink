#include "comlink/core.h"

const char *sp_strerror(sp_status status) {
    switch (status) {
    case SP_OK:              return "success";
    case SP_ERR_INVALID_ARG: return "invalid argument";
    case SP_ERR_STATE:       return "operation invalid in current state";
    case SP_ERR_NOT_FOUND:   return "handle not found or retired";
    case SP_ERR_TRANSPORT:   return "transport failure";
    case SP_ERR_AUTH:        return "authentication rejected";
    case SP_ERR_MEDIA:       return "media failure";
    case SP_ERR_CONFIG:      return "configuration error";
    case SP_ERR_INTERNAL:    return "internal error";
    case SP_ERR_DISABLED:    return "module disabled";
    case SP_ERR_IO:          return "filesystem operation failed";
    }
    return "unrecognised status code";
}
