/*
 * Copyright (C) 2021 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <sys/sysinfo.h>
#include <libinit_utils.h>

#include <libinit_dalvik_heap.h>

#define HEAPSTARTSIZE_PROP "dalvik.vm.heapstartsize"
#define HEAPGROWTHLIMIT_PROP "dalvik.vm.heapgrowthlimit"
#define HEAPSIZE_PROP "dalvik.vm.heapsize"
#define HEAPMINFREE_PROP "dalvik.vm.heapminfree"
#define HEAPMAXFREE_PROP "dalvik.vm.heapmaxfree"
#define HEAPTARGETUTILIZATION_PROP "dalvik.vm.heaptargetutilization"

static constexpr uint64_t kLowerRamCapacityBytes = 8ULL * 1024 * 1024 * 1024;

static const dalvik_heap_info_t dalvik_heap_info_12288 = {
    .heapstartsize = "24m",
    .heapgrowthlimit = "384m",
    .heapsize = "512m",
    .heapminfree = "8m",
    .heapmaxfree = "56m",
    .heaptargetutilization = "0.42",
};

static const dalvik_heap_info_t dalvik_heap_info_8192 = {
    .heapstartsize = "24m",
    .heapgrowthlimit = "256m",
    .heapsize = "512m",
    .heapminfree = "8m",
    .heapmaxfree = "48m",
    .heaptargetutilization = "0.46",
};

void set_dalvik_heap() {
    struct sysinfo sys = {};
    const dalvik_heap_info_t *dhi = &dalvik_heap_info_8192;

    // Reserved memory reduces the usable total below the physical RAM capacity.
    // More than 8 GiB usable RAM rules out the 8 GiB variant without requiring
    // the 12 GiB variant to retain at least 11 GiB after reservations.
    // Compare in sysinfo units to avoid overflowing a totalram * mem_unit product.
    if (sysinfo(&sys) == 0 && sys.mem_unit != 0 &&
        static_cast<uint64_t>(sys.totalram) > kLowerRamCapacityBytes / sys.mem_unit) {
        dhi = &dalvik_heap_info_12288;
    }

    property_override(HEAPSTARTSIZE_PROP, dhi->heapstartsize);
    property_override(HEAPGROWTHLIMIT_PROP, dhi->heapgrowthlimit);
    property_override(HEAPSIZE_PROP, dhi->heapsize);
    property_override(HEAPTARGETUTILIZATION_PROP, dhi->heaptargetutilization);
    property_override(HEAPMINFREE_PROP, dhi->heapminfree);
    property_override(HEAPMAXFREE_PROP, dhi->heapmaxfree);
}
