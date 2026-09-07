#pragma once

namespace parqit_plugin {

struct OpenMPProbe {
    int threads;
    int checksum;
};

int openmp_version();
int openmp_max_threads();
OpenMPProbe openmp_probe();

} // namespace parqit_plugin
