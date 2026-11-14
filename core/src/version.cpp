#include <flowget/flowget.h>
#include <flowget/version.h>

extern "C" const char* flowget_version_string(void) {
    return flowget::kVersionString.data();
}
