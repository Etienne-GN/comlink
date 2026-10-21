/* Compiles with no pjproject include path on purpose. If core.h ever
   leaks a PJSIP type, this translation unit stops building. */
#include "comlink/core.h"

int main(void) {
    return sp_strerror(SP_OK) ? 0 : 1;
}
