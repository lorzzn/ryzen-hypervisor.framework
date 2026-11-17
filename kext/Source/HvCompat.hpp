#ifndef RYZENHV_HVCOMPAT_HPP
#define RYZENHV_HVCOMPAT_HPP

#include <mach/mach_types.h>
#include <mach/mach_vm.h>

namespace rhv { namespace hv { namespace patch {

void startCompat();
void stopCompat();
void installHyperKitCompat(vm_map_t map, mach_vm_address_t image);

} } }
#endif
