#ifndef HEADER_81A374DA_8E23_4E4D_96D2_619F27016F2A_INCLUDED
#define HEADER_81A374DA_8E23_4E4D_96D2_619F27016F2A_INCLUDED

#include <string>

namespace genny::driver {

struct ProgramOptions {
    explicit ProgramOptions() = default;

    /**
     * @param argc c-style argc
     * @param argv c-style argv
     */
    ProgramOptions(int argc, char** argv);

    std::string workloadFileName;
    std::string metricsFormat;
    std::string metricsOutputFileName;
    std::string mongoUri;
    std::string description;
    bool isHelp = false;
    bool shouldListActors = false;
};

/**
 * Basic workload driver that spins up one thread per actor.
 */
class DefaultDriver {

public:
    /**
     * @return c-style exit code
     */
    int run(const ProgramOptions& options) const;
};

}  // namespace genny::driver

#endif  // HEADER_81A374DA_8E23_4E4D_96D2_619F27016F2A_INCLUDED
