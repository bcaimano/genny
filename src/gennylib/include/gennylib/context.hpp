#ifndef HEADER_0E802987_B910_4661_8FAB_8B952A1E453B_INCLUDED
#define HEADER_0E802987_B910_4661_8FAB_8B952A1E453B_INCLUDED

#include <functional>
#include <iterator>
#include <optional>
#include <random>
#include <type_traits>
#include <vector>

#include <boost/noncopyable.hpp>

#include <mongocxx/pool.hpp>

#include <yaml-cpp/yaml.h>

#include <gennylib/Actor.hpp>
#include <gennylib/ActorProducer.hpp>
#include <gennylib/ActorVector.hpp>
#include <gennylib/InvalidConfigurationException.hpp>
#include <gennylib/Orchestrator.hpp>
#include <gennylib/conventions.hpp>
#include <gennylib/metrics.hpp>

/**
 * This file defines `WorkloadContext`, `ActorContext`, and `PhaseContext` which provide access
 * to configuration values and other workload collaborators (e.g. metrics) during the construction
 * of actors.
 *
 * Please see the documentation below on WorkloadContext, ActorContext, and PhaseContext.
 */


/*
 * This is all helper/private implementation details. Ideally this section could
 * be defined _below_ the important stuff, but we live in a cruel world.
 */
namespace genny::V1 {

/**
 * If Required, type is Out, else it's optional<Out>
 */
template <class Out, bool Required = true>
struct MaybeOptional {
    using type = typename std::conditional<Required, Out, std::optional<Out>>::type;
};

/**
 * The "path" to a configured value. E.g. given the structure
 *
 * ```yaml
 * foo:
 *   bar:
 *     baz: [10,20,30]
 * ```
 *
 * The path to the 10 is "foo/bar/baz/0".
 *
 * This is used to report meaningful exceptions in the case of mis-configuration.
 */
class ConfigPath {

public:
    ConfigPath() = default;

    ConfigPath(ConfigPath&) = delete;
    void operator=(ConfigPath&) = delete;

    template <class T>
    void add(const T& elt) {
        _elements.push_back(elt);
    }
    auto begin() const {
        return std::begin(_elements);
    }
    auto end() const {
        return std::end(_elements);
    }

private:
    /**
     * The parts of the path, so for this structure
     *
     * ```yaml
     * foo:
     *   bar: [bat, baz]
     * ```
     *
     * If this `ConfigPath` represents "baz", then `_elements`
     * will be `["foo", "bar", 1]`.
     *
     * To be "efficient" we only store a `function` that produces the
     * path component string; do this to avoid (maybe) expensive
     * string-formatting in the "happy case" where the ConfigPath is
     * never fully serialized to an exception.
     */
    std::vector<std::function<void(std::ostream&)>> _elements;
};

// Support putting ConfigPaths onto ostreams
inline std::ostream& operator<<(std::ostream& out, const ConfigPath& path) {
    for (const auto& f : path) {
        f(out);
        out << "/";
    }
    return out;
}

// Used by get() in WorkloadContext and ActorContext
//
// This is the base-case when we're out of Args... expansions in the other helper below
template <class Out,
          class Current,
          bool Required = true,
          class OutV = typename MaybeOptional<Out, Required>::type>
OutV get_helper(const ConfigPath& parent, const Current& curr) {
    if (!curr) {
        if constexpr (Required) {
            std::stringstream error;
            error << "Invalid key at path [" << parent << "]";
            throw InvalidConfigurationException(error.str());
        } else {
            return std::nullopt;
        }
    }
    try {
        if constexpr (Required) {
            return curr.template as<Out>();
        } else {
            return std::make_optional<Out>(curr.template as<Out>());
        }
    } catch (const YAML::BadConversion& conv) {
        std::stringstream error;
        // typeid(Out).name() is kinda hokey but could be useful when debugging config issues.
        error << "Bad conversion of [" << curr << "] to [" << typeid(Out).name() << "] "
              << "at path [" << parent << "]: " << conv.what();
        throw InvalidConfigurationException(error.str());
    }
}

// Used by get() in WorkloadContext and ActorContext
//
// Recursive case where we pick off first item and recurse:
//      get_helper(foo, a, b, c) // this fn
//   -> get_helper(foo[a], b, c) // this fn
//   -> get_helper(foo[a][b], c) // this fn
//   -> get_helper(foo[a][b][c]) // "base case" fn above
template <class Out,
          class Current,
          bool Required = true,
          class OutV = typename MaybeOptional<Out, Required>::type,
          class PathFirst,
          class... PathRest>
OutV get_helper(ConfigPath& parent,
                const Current& curr,
                PathFirst&& pathFirst,
                PathRest&&... rest) {
    if (curr.IsScalar()) {
        std::stringstream error;
        error << "Wanted [" << parent << pathFirst << "] but [" << parent << "] is scalar: ["
              << curr << "]";
        throw InvalidConfigurationException(error.str());
    }
    const auto& next = curr[std::forward<PathFirst>(pathFirst)];

    parent.add([&](std::ostream& out) { out << pathFirst; });

    if (!next.IsDefined()) {
        if constexpr (Required) {
            std::stringstream error;
            error << "Invalid key [" << pathFirst << "] at path [" << parent << "]. Last accessed ["
                  << curr << "].";
            throw InvalidConfigurationException(error.str());
        } else {
            return std::nullopt;
        }
    }
    return V1::get_helper<Out, Current, Required>(parent, next, std::forward<PathRest>(rest)...);
}

}  // namespace genny::V1

namespace genny {

/**
 * Represents the top-level/"global" configuration and context for configuring actors.
 * Call `.get()` to access top-level yaml configs.
 */
class WorkloadContext {
public:
    /**
     * @param producers
     *  producers are called eagerly at construction-time.
     */
    WorkloadContext(YAML::Node node,
                    metrics::Registry& registry,
                    Orchestrator& orchestrator,
                    const std::string& mongoUri,
                    const std::vector<ActorProducer>& producers)
        : _node{std::move(node)},
          _registry{&registry},
          _orchestrator{&orchestrator},
          // TODO: make this optional and default to mongodb://localhost:27017
          _clientPool{mongocxx::uri{mongoUri}},
          _done{false} {
        // This is good enough for now. Later can add a WorkloadContextValidator concept
        // and wire in a vector of those similar to how we do with the vector of Producers.
        if (get_static<std::string>(_node, "SchemaVersion") != "2018-07-01") {
            throw InvalidConfigurationException("Invalid schema version");
        }

        // Default value selected from random.org, by selecting 2 random numbers
        // between 1 and 10^9 and concatenating.
        rng.seed(get_static<int, false>(node, "RandomSeed").value_or(269849313357703264));
        _actorContexts = constructActorContexts(_node, this);
        _actors = constructActors(producers, _actorContexts);
        _done = true;
    }

    // no copy or move
    WorkloadContext(WorkloadContext&) = delete;
    void operator=(WorkloadContext&) = delete;
    WorkloadContext(WorkloadContext&&) = default;
    void operator=(WorkloadContext&&) = delete;

    /**
     * Retrieve configuration values from the top-level workload configuration.
     * Returns `root[arg1][arg2]...[argN]`.
     *
     * This is somewhat expensive and should only be called during actor/workload setup.
     *
     * Typical usage:
     *
     * ```c++
     *     class MyActor ... {
     *       string name;
     *       MyActor(ActorContext& context)
     *       : name{context.get<string>("Name")} {}
     *     }
     * ```
     *
     * Given this YAML:
     *
     * ```yaml
     *     SchemaVersion: 2018-07-01
     *     Actors:
     *     - Name: Foo
     *       Count: 100
     *     - Name: Bar
     * ```
     *
     * Then traverse as with the following:
     *
     * ```c++
     *     auto schema = context.get<std::string>("SchemaVersion");
     *     auto actors = context.get("Actors"); // actors is a YAML::Node
     *     auto name0  = context.get<std::string>("Actors", 0, "Name");
     *     auto count0 = context.get<int>("Actors", 0, "Count");
     *     auto name1  = context.get<std::string>("Actors", 1, "Name");
     *
     *     // if value may not exist:
     *     std::optional<int> = context.get<int,false>("Actors", 0, "Count");
     * ```
     * @tparam T the output type required. Will forward to YAML::Node.as<T>()
     * @tparam Required If true, will error if item not found. If false, will return an optional<T>
     * that will be empty if not found.
     */
    template <class T = YAML::Node,
              bool Required = true,
              class OutV = typename V1::MaybeOptional<T, Required>::type,
              class... Args>
    static OutV get_static(const YAML::Node& node, Args&&... args) {
        V1::ConfigPath p;
        return V1::get_helper<T, YAML::Node, Required>(p, node, std::forward<Args>(args)...);
    };

    /**
     * @see get_static()
     */
    template <typename T = YAML::Node,
              bool Required = true,
              class OutV = typename V1::MaybeOptional<T, Required>::type,
              class... Args>
    OutV get(Args&&... args) const {
        return WorkloadContext::get_static<T, Required>(_node, std::forward<Args>(args)...);
    };

    /**
     * @return all the actors produced. This should only be called by workload drivers.
     */
    constexpr const ActorVector& actors() const {
        return _actors;
    }

    /*
     * @return a new seeded random number generator. This should only be called during construction
     * to ensure reproducibility.
     */
    std::mt19937_64 createRNG() {
        if (_done) {
            throw InvalidConfigurationException(
                "Tried to create a random number generator after construction");
        }
        return std::mt19937_64{rng()};
    }

private:
    friend class ActorContext;

    // helper methods used during construction
    static ActorVector constructActors(const std::vector<ActorProducer>& producers,
                                       const std::vector<std::unique_ptr<ActorContext>>&);
    static std::vector<std::unique_ptr<ActorContext>> constructActorContexts(const YAML::Node&,
                                                                             WorkloadContext*);
    YAML::Node _node;

    std::mt19937_64 rng;
    metrics::Registry* const _registry;
    Orchestrator* const _orchestrator;
    // we own the child ActorContexts
    std::vector<std::unique_ptr<ActorContext>> _actorContexts;
    mongocxx::pool _clientPool;
    ActorVector _actors;
    // Indicate that we are doing building the context. This is used to gate certain methods that
    // should not be called after construction.
    bool _done;
};

// For some reason need to decl this; see impl below
class PhaseContext;

/**
 * Represents each `Actor:` block within a WorkloadConfig.
 */
class ActorContext final {
public:
    ActorContext(const YAML::Node& node, WorkloadContext& workloadContext)
        : _node{node}, _workload{&workloadContext}, _phaseContexts{} {
        _phaseContexts = constructPhaseContexts(node, this);
    }

    // no copy or move
    ActorContext(ActorContext&) = delete;
    void operator=(ActorContext&) = delete;
    ActorContext(ActorContext&&) = default;
    void operator=(ActorContext&&) = delete;

    /**
     * Retrieve configuration values from a particular `Actor:` block in the workload configuration.
     * `Returns actor[arg1][arg2]...[argN]`.
     *
     * This is somewhat expensive and should only be called during actor/workload setup.
     *
     * Typical usage:
     *
     * ```c++
     *     class MyActor ... {
     *       string name;
     *       MyActor(ActorContext& context)
     *       : name{context.get<string>("Name")} {}
     *     }
     * ```
     *
     * Given this YAML:
     *
     * ```yaml
     *     SchemaVersion: 2018-07-01
     *     Actors:
     *     - Name: Foo
     *     - Name: Bar
     * ```
     *
     * There will be two ActorConfigs, one for `{Name:Foo}` and another for `{Name:Bar}`.
     *
     * ```
     * auto name = cx.get<std::string>("Name");
     * ```
     * @tparam T the return-value type. Will return a T if Required (and throw if not found) else
     * will return an optional<T> (empty optional if not found).
     * @tparam Required If true, will error if item not found. If false, will return an optional<T>
     * that will be empty if not found.
     */
    template <typename T = YAML::Node,
              bool Required = true,
              class OutV = typename V1::MaybeOptional<T, Required>::type,
              class... Args>
    OutV get(Args&&... args) const {
        V1::ConfigPath p;
        return V1::get_helper<T, YAML::Node, Required>(p, _node, std::forward<Args>(args)...);
    };

    /**
     * Access top-level workload configuration.
     */
    WorkloadContext& workload() {
        return *this->_workload;
    }

    Orchestrator& orchestrator() {
        return *this->_workload->_orchestrator;
    }

    /**
     * @return a structure representing the `Phases:` block in the Actor config.
     *
     * If you want per-Phase configuration, consider using `PhaseLoop<T>` which
     * will let you construct a `T` for each Phase at constructor-time and will
     * automatically coordinate with the `Orchestrator`.
     *   ** See extended example on the `PhaseLoop` class. **
     *
     * Keys are phase numbers and values are the Phase blocks associated with them.
     * Empty if there are no configured Phases.
     *
     * E.g.
     *
     * ```yaml
     * ...
     * Actors:
     * - Name: Linkbench
     *   Type: Linkbench
     *   Collection: links
     *
     *   Phases:
     *   - Phase: 0
     *     Operation: Insert
     *     Repeat: 1000
     *     # Inherits `Collection: links` from parent
     *
     *   - Phase: 1
     *     Operation: Request
     *     Duration: 1 minute
     *     Collection: links2 # Overrides `Collection: links` from parent
     *
     *   - Operation: Cleanup
     *     # inherits `Collection: links` from parent,
     *     # and `Phase: 3` is derived based on index
     * ```
     *
     * This would result in 3 `PhaseContext` structures. Keys are inherited from the
     * parent (Actor-level) unless overridden, and the `Phase` key is defaulted from
     * the block's index if not otherwise specified.
     *
     * *Note* that Phases are "opt-in" to all Actors and may represent phase-specific
     * configuration in other mechanisms if desired. The `Phases:` structure and
     * related PhaseContext type are purely for conventional convenience.
     */
    const std::unordered_map<genny::PhaseNumber, std::unique_ptr<PhaseContext>>& phases() const {
        return _phaseContexts;
    };

    mongocxx::pool::entry client() {
        return _workload->_clientPool.acquire();
    }

    // <Forwarding to delegates>

    /**
     * Convenience method for creating a metrics::Timer.
     *
     * @param operationName
     *   the name of the thing being timed.
     *   Will automatically add prefixes to make the full name unique
     *   across Actors and threads.
     * @param thread the thread number of this Actor, if any.
     */
    auto timer(const std::string& operationName, ActorId id = 0u) const {
        auto name = this->metricsName(operationName, id);
        return this->_workload->_registry->timer(name);
    }

    /**
     * Convenience method for creating a metrics::Gauge.
     *
     * @param operationName
     *   the name of the thing being gauged.
     *   Will automatically add prefixes to make the full name unique
     *   across Actors and threads.
     * @param thread the thread number of this Actor, if any.
     */
    auto gauge(const std::string& operationName, ActorId id = 0u) const {
        auto name = this->metricsName(operationName, id);
        return this->_workload->_registry->gauge(name);
    }

    /**
     * Convenience method for creating a metrics::Counter.
     *
     *
     * @param operationName
     *   the name of the thing being counted.
     *   Will automatically add prefixes to make the full name unique
     *   across Actors and threads.
     * @param thread the thread number of this Actor, if any.
     */
    auto counter(const std::string& operationName, ActorId id = 0u) const {
        auto name = this->metricsName(operationName, id);
        return this->_workload->_registry->counter(name);
    }

    auto morePhases() {
        return this->_workload->_orchestrator->morePhases();
    }

    auto currentPhase() {
        return this->_workload->_orchestrator->currentPhase();
    }
    auto awaitPhaseStart() {
        return this->_workload->_orchestrator->awaitPhaseStart();
    }

    template <class... Args>
    auto awaitPhaseEnd(Args&&... args) {
        return this->_workload->_orchestrator->awaitPhaseEnd(std::forward<Args>(args)...);
    }

    auto abort() {
        return this->_workload->_orchestrator->abort();
    }

    // </Forwarding to delegates>

private:
    /**
     * Apply metrics names conventions based on configuration.
     *
     * @param operation base name of a metrics object e.g. "inserts"
     * @param thread the thread number of the Actor owning the object.
     * @return the fully-qualified metrics name e.g. "MyActor.0.inserts".
     */
    std::string metricsName(const std::string& operation, ActorId id) const {
        return this->get<std::string>("Name") + ".id-" + std::to_string(id) + "." + operation;
    }

    static std::unordered_map<genny::PhaseNumber, std::unique_ptr<PhaseContext>>
    constructPhaseContexts(const YAML::Node&, ActorContext*);
    YAML::Node _node;
    WorkloadContext* _workload;
    std::unordered_map<PhaseNumber, std::unique_ptr<PhaseContext>> _phaseContexts;
};


class PhaseContext final {

public:
    PhaseContext(const YAML::Node& node, const ActorContext& actorContext)
        : _node{node}, _actor{&actorContext} {}

    // no copy or move
    PhaseContext(PhaseContext&) = delete;
    void operator=(PhaseContext&) = delete;
    PhaseContext(PhaseContext&&) = default;
    void operator=(PhaseContext&&) = delete;

    /**
     * @return the value associated with the given key. If not specified
     * directly in this `Phases` block, then the value from the parent `Actor`
     * context is used, if present.
     */
    template <typename T = YAML::Node,
              bool Required = true,
              class OutV = typename V1::MaybeOptional<T, Required>::type,
              class... Args>
    OutV get(Args&&... args) const {
        V1::ConfigPath p;
        // try to extract from own node
        auto fromSelf = V1::get_helper<T, YAML::Node, false>(p, _node, std::forward<Args>(args)...);
        if (fromSelf) {
            if constexpr (Required) {
                // unwrap from optional<T>
                return *fromSelf;
            } else {
                // don't unwrap, return the optional<T> itself
                return fromSelf;
            }
        }

        // fallback to actor node
        return this->_actor->get<T, Required>(std::forward<Args>(args)...);
    };

private:
    YAML::Node _node;
    const ActorContext* _actor;
};

inline ActorProducer makeThreadedProducer(ActorProducer producer){
    return [producer{std::move(producer)}](ActorContext& context) {
        ActorProducer::result_type out;

        auto threads = context.get<int>("Threads");
        for (int i = 0; i < threads; ++i)
            for (auto & actor : producer(context))
                out.emplace_back(std::move(actor));

        return out;
    };
}

}  // namespace genny

#endif  // HEADER_0E802987_B910_4661_8FAB_8B952A1E453B_INCLUDED
