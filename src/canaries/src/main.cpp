// Copyright 2019-present MongoDB Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <boost/program_options.hpp>

#include <canaries/Loops.hpp>
#include <gennylib/InvalidConfigurationException.hpp>
#include <gennylib/context.hpp>

using namespace genny;

struct ProgramOptions {

    std::vector<std::string> _loopNames;

    bool _isHelp = false;

    int64_t _iterations = 0;

    std::string _description;
    std::string _mongoUri;
    std::string _task;
    std::string _metricsFileName;

    explicit ProgramOptions() = default;

    ProgramOptions(int argc, char** argv) {
        namespace po = boost::program_options;

        std::ostringstream progDesc;
        progDesc << "Genny Canaries - Microbenchmarks for measuring overhead of Genny\n";
        progDesc << "                 by running low-level tasks in Genny loops\n\n";
        progDesc << "Usage:\n";
        progDesc << "    " << argv[0] << " <task-name> [loop-type [loop-type] ..]\n\n";
        progDesc << "Types of task:‍";
        progDesc << R"(
    nop     Trivial task that reads a value from a register; intended for
             testing loops with the minimum amount of unrelated code
    sleep    Sleep for 1ms
    cpu      Multiply a large number 10000 times to stress the CPU's ALU.
    l2       Traverse through a 256KB array in 64KB strides; stress the CPU's L2 cache
    l3       Traverse through a 8MB array in 64KB strides; stress the CPU's L3 cache
             and/or RAM depending the CPU and its load
    ping     call db.ping() on a MongoDB server (running externally)
    )"
                 << "\n\n";

        progDesc << "Types of loops:‍";
        progDesc << R"(
    simple   Run native for-loop; used as the control group with no Genny code
    phase    Run just the PhaseLoop
    metrics  Run native for-loop and record one timer metric per iteration
    real     Run PhaseLoop and record one timer metric per iteration; resembles
             how a real actor runs
    )"
                 << "\n";

        progDesc << "Options";
        po::options_description optDesc(progDesc.str());
        po::positional_options_description positional;

        // clang-format off
        optDesc.add_options()
        ("help,h",
                "Show this help message")
        ("loop-type",
                po::value<std::vector<std::string>>()
                ->multitoken(),
                "The type of loop to benchmark; defaults to all loop types")
        ("task",
                po::value<std::string>(),
                "What type of task to do within each iteration of the loop")
        ("iterations,i",
                po::value<int64_t>()->default_value(1e6),
                "Number of iterations to run the tests")
        ("mongo-uri,u",
                po::value<std::string>()->default_value("mongodb://localhost:27017"))
        ("metrics-output-file,o",
                po::value<std::string>(),
                "Write output to file in addition to stdout. The format ouf the output"
                "file is [task-name]_[loop-type],[average_duration_in_picoseconds]");
        //clang-format on

        positional.add("task", 1);
        positional.add("loop-type", -1);

        auto run =
            po::command_line_parser(argc, argv).options(optDesc).positional(positional).run();

        std::ostringstream stm;
        stm << optDesc;
        _description = stm.str();

        po::variables_map vm;
        po::store(run, vm);
        po::notify(vm);

        if (vm.count("help") >= 1)
            _isHelp = true;

        if (!vm.count("task")) {
            // Missing required argument, print help.
            _isHelp = true;
        } else {
            _task = vm["task"].as<std::string>();
        }

        if (vm.count("metrics-output-file") >= 1) {
            _metricsFileName = vm["metrics-output-file"].as<std::string>();
        }

        if (vm.count("loop-type") >= 1)
            _loopNames = vm["loop-name"].as<std::vector<std::string>>();
        else
            _loopNames = {"simple", "phase", "metrics", "real"};

        _iterations = vm["iterations"].as<int64_t>();
        _mongoUri = vm["mongo-uri"].as<std::string>();
    }
};

int main(int argc, char** argv) {
    using namespace genny::canaries;
    auto opts = ProgramOptions(argc, argv);
    if (opts._isHelp || opts._loopNames.empty()) {
        std::cout << opts._description << std::endl;
        return 0;
    }

    std::vector<Nanosecond> results;

    if (opts._task == "nop")
        results = runTest<NopTask>(opts._loopNames, opts._iterations);
    else if (opts._task == "sleep")
        results = runTest<SleepTask>(opts._loopNames, opts._iterations);
    else if (opts._task == "cpu")
        results = runTest<CPUTask>(opts._loopNames, opts._iterations);
    else if (opts._task == "l2")
        results = runTest<L2Task>(opts._loopNames, opts._iterations);
    else if (opts._task == "l3")
        results = runTest<L3Task>(opts._loopNames, opts._iterations);
    else if (opts._task == "ping")
        results = runTest<PingTask>(opts._loopNames, opts._iterations, opts._mongoUri);
    else {
        std::ostringstream stm;
        stm << "Unknown task name: " << opts._task;
        throw InvalidConfigurationException(stm.str());
    }

    std::cout << "Total duration for " << opts._task << ":" << std::endl;
    for (int i = 0; i < results.size(); i++) {
        std::cout << std::setw(8) << opts._loopNames[i] << ": " << results[i] << "ns" << std::endl;
    }

    if (!opts._metricsFileName.empty()) {
        std::ofstream metrics;
        metrics.open(opts._metricsFileName, std::ofstream::out | std::ofstream::trunc);
        for (int i = 0; i < results.size(); i++) {
            metrics << opts._task << "_" << opts._loopNames[i] << "," << (results[i] * 1000 / opts._iterations);
            metrics << std::endl;
        }
    }

    return 0;
}
