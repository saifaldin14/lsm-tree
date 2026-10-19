#include "lsm_lab/spec.hpp"

#include <algorithm>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <set>
#include <stdexcept>
#include <vector>

#include <nlohmann/json.hpp>

namespace lsm_lab
{
    namespace
    {

        using Json = nlohmann::json;

        void fields(const Json &object, const std::string &section,
                    std::initializer_list<std::string_view> expected,
                    std::initializer_list<std::string_view> optional = {})
        {
            if (!object.is_object())
            {
                throw std::invalid_argument(section + " must be an object");
            }
            for (const auto &[key, value] : object.items())
            {
                (void)value;
                if (std::find(expected.begin(), expected.end(), key) == expected.end() &&
                    std::find(optional.begin(), optional.end(), key) == optional.end())
                {
                    throw std::invalid_argument("unknown field: " + section + "." + key);
                }
            }
            for (const auto key : expected)
            {
                if (!object.contains(std::string(key)))
                {
                    throw std::invalid_argument("missing field: " + section + "." + std::string(key));
                }
            }
        }

        std::uint64_t number(const Json &object, const char *field,
                             std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max())
        {
            const auto &value = object.at(field);
            if (!value.is_number_unsigned() || value.get<std::uint64_t>() > maximum)
            {
                throw std::invalid_argument(std::string(field) + " must be an unsigned integer within range");
            }
            return value.get<std::uint64_t>();
        }

        std::uint32_t number32(const Json &object, const char *field)
        {
            return static_cast<std::uint32_t>(number(object, field, std::numeric_limits<std::uint32_t>::max()));
        }

        void range(std::uint64_t value, std::uint64_t minimum, std::uint64_t maximum,
                   const char *field)
        {
            if (value < minimum || value > maximum)
            {
                throw std::invalid_argument(std::string(field) + " is outside the supported range");
            }
        }

        template <typename Integer>
        void optional_number(const Json &object, const char *field, Integer &target)
        {
            if (object.contains(field))
            {
                target = static_cast<Integer>(number(object, field, std::numeric_limits<Integer>::max()));
            }
        }

        template <typename Integer>
        void optional_number(const Json &object, const char *field, std::optional<Integer> &target)
        {
            if (object.contains(field))
            {
                target = static_cast<Integer>(number(object, field, std::numeric_limits<Integer>::max()));
            }
        }

        void optional_range(const std::optional<std::uint32_t> &value, std::uint64_t minimum, std::uint64_t maximum,
                            const char *field)
        {
            if (value)
            {
                range(*value, minimum, maximum, field);
            }
        }

        // RocksDB defaults used when a level-0 trigger is not configured.
        constexpr std::uint32_t default_level0_compaction_trigger = 4;
        constexpr std::uint32_t default_level0_slowdown_trigger = 20;
        constexpr std::uint32_t default_level0_stop_trigger = 36;

    }

    std::uint64_t logical_write_bytes(const DatasetSpec &dataset)
    {
        const auto record_bytes = static_cast<std::uint64_t>(dataset.key_bytes) + dataset.value_bytes;
        const auto passes = static_cast<std::uint64_t>(dataset.overwrite_rounds) + 1;
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        if (record_bytes != 0 && dataset.records > maximum / record_bytes)
        {
            throw std::invalid_argument("dataset byte count overflows uint64");
        }
        const auto initial_bytes = dataset.records * record_bytes;
        if (initial_bytes > maximum / passes)
        {
            throw std::invalid_argument("dataset overwrite byte count overflows uint64");
        }
        return initial_bytes * passes;
    }

    void validate(const Spec &spec)
    {
        range(spec.dataset.records, 1, 1'000'000'000, "dataset.records");
        range(spec.dataset.key_bytes, 16, 1024, "dataset.key_bytes");
        range(spec.dataset.value_bytes, 1, 1ULL << 20, "dataset.value_bytes");
        range(spec.dataset.overwrite_rounds, 0, 1024, "dataset.overwrite_rounds");
        range(spec.engine.write_buffer_bytes, 64ULL << 10, 1ULL << 30, "engine.write_buffer_bytes");
        range(spec.engine.target_file_bytes, 64ULL << 10, 1ULL << 30, "engine.target_file_bytes");
        range(spec.engine.block_cache_bytes, 64ULL << 10, 64ULL << 30, "engine.block_cache_bytes");
        range(spec.engine.block_bytes, 512, 1ULL << 20, "engine.block_bytes");
        range(spec.engine.background_jobs, 1, 256, "engine.background_jobs");
        range(spec.engine.levels, 2, 16, "engine.levels");
        range(spec.engine.max_subcompactions, 1, 256, "engine.max_subcompactions");
        if (spec.engine.max_bytes_for_level_base)
        {
            range(*spec.engine.max_bytes_for_level_base, 64ULL << 10, 1ULL << 40, "engine.max_bytes_for_level_base");
        }
        range(spec.engine.max_write_buffer_number, 2, 64, "engine.max_write_buffer_number");
        optional_range(spec.engine.level0_file_num_compaction_trigger, 1, 1024, "engine.level0_file_num_compaction_trigger");
        optional_range(spec.engine.level0_slowdown_writes_trigger, 1, 1024, "engine.level0_slowdown_writes_trigger");
        optional_range(spec.engine.level0_stop_writes_trigger, 1, 1024, "engine.level0_stop_writes_trigger");
        const auto trigger = spec.engine.level0_file_num_compaction_trigger.value_or(default_level0_compaction_trigger);
        const auto slowdown = spec.engine.level0_slowdown_writes_trigger.value_or(default_level0_slowdown_trigger);
        const auto stop = spec.engine.level0_stop_writes_trigger.value_or(default_level0_stop_trigger);
        if (trigger > slowdown || slowdown > stop)
        {
            throw std::invalid_argument("engine level-0 triggers must satisfy compaction <= slowdown <= stop "
                                        "(unset triggers use RocksDB defaults 4, 20, 36)");
        }
        if (spec.dataset.load_mode != "sync" && spec.dataset.load_mode != "bulk" && spec.dataset.load_mode != "ingest")
        {
            throw std::invalid_argument("dataset.load_mode must be sync, bulk, or ingest");
        }
        range(spec.limits.logical_write_bytes, 1, std::numeric_limits<std::uint64_t>::max(),
              "limits.logical_write_bytes");
        range(spec.limits.minimum_free_disk_bytes, 1, std::numeric_limits<std::uint64_t>::max(),
              "limits.minimum_free_disk_bytes");
        if (spec.engine.compression != "lz4" && spec.engine.compression != "zstd" && spec.engine.compression != "none")
        {
            throw std::invalid_argument("engine.compression must be lz4, zstd, or none");
        }
        if (logical_write_bytes(spec.dataset) > spec.limits.logical_write_bytes)
        {
            throw std::invalid_argument("dataset exceeds limits.logical_write_bytes");
        }
    }

    Spec parse_spec(std::string_view text)
    {
        if (text.size() > 64 * 1024)
        {
            throw std::invalid_argument("configuration exceeds 64 KiB");
        }
        std::vector<std::set<std::string>> object_keys;
        const auto callback = [&object_keys](int depth, Json::parse_event_t event, Json &parsed)
        {
            if (depth > 8)
            {
                throw std::invalid_argument("configuration nesting exceeds eight levels");
            }
            if (event == Json::parse_event_t::object_start)
            {
                object_keys.emplace_back();
            }
            else if (event == Json::parse_event_t::key)
            {
                if (!object_keys.back().insert(parsed.get<std::string>()).second)
                {
                    throw std::invalid_argument("duplicate JSON field: " + parsed.get<std::string>());
                }
            }
            else if (event == Json::parse_event_t::object_end)
            {
                object_keys.pop_back();
            }
            return true;
        };
        const auto document = Json::parse(text, callback);
        fields(document, "spec", {"schema_version", "dataset", "engine", "limits"});
        if (number(document, "schema_version") != 1)
        {
            throw std::invalid_argument("unsupported schema_version");
        }
        const auto &dataset = document.at("dataset");
        fields(dataset, "dataset", {"records", "seed", "key_bytes", "value_bytes", "overwrite_rounds"}, {"load_mode"});
        const auto &engine = document.at("engine");
        fields(engine, "engine", {"compression", "write_buffer_bytes", "target_file_bytes", "block_cache_bytes", "block_bytes", "background_jobs", "levels"},
               {"max_subcompactions", "max_bytes_for_level_base", "max_write_buffer_number", "level0_file_num_compaction_trigger",
                "level0_slowdown_writes_trigger", "level0_stop_writes_trigger", "use_direct_io", "verify_output"});
        const auto &limits = document.at("limits");
        fields(limits, "limits", {"logical_write_bytes", "minimum_free_disk_bytes"});
        Spec spec;
        spec.dataset.records = number(dataset, "records");
        spec.dataset.seed = number(dataset, "seed");
        spec.dataset.key_bytes = number32(dataset, "key_bytes");
        spec.dataset.value_bytes = number32(dataset, "value_bytes");
        spec.dataset.overwrite_rounds = number32(dataset, "overwrite_rounds");
        if (dataset.contains("load_mode"))
        {
            if (!dataset.at("load_mode").is_string())
            {
                throw std::invalid_argument("dataset.load_mode must be a string");
            }
            spec.dataset.load_mode = dataset.at("load_mode").get<std::string>();
        }
        if (!engine.at("compression").is_string())
        {
            throw std::invalid_argument("engine.compression must be a string");
        }
        spec.engine.compression = engine.at("compression").get<std::string>();
        spec.engine.write_buffer_bytes = number(engine, "write_buffer_bytes");
        spec.engine.target_file_bytes = number(engine, "target_file_bytes");
        spec.engine.block_cache_bytes = number(engine, "block_cache_bytes");
        spec.engine.block_bytes = number32(engine, "block_bytes");
        spec.engine.background_jobs = number32(engine, "background_jobs");
        spec.engine.levels = number32(engine, "levels");
        optional_number(engine, "max_subcompactions", spec.engine.max_subcompactions);
        optional_number(engine, "max_bytes_for_level_base", spec.engine.max_bytes_for_level_base);
        optional_number(engine, "max_write_buffer_number", spec.engine.max_write_buffer_number);
        optional_number(engine, "level0_file_num_compaction_trigger", spec.engine.level0_file_num_compaction_trigger);
        optional_number(engine, "level0_slowdown_writes_trigger", spec.engine.level0_slowdown_writes_trigger);
        optional_number(engine, "level0_stop_writes_trigger", spec.engine.level0_stop_writes_trigger);
        if (engine.contains("use_direct_io"))
        {
            if (!engine.at("use_direct_io").is_boolean())
            {
                throw std::invalid_argument("engine.use_direct_io must be a boolean");
            }
            spec.engine.use_direct_io = engine.at("use_direct_io").get<bool>();
        }
        if (engine.contains("verify_output"))
        {
            if (!engine.at("verify_output").is_boolean())
            {
                throw std::invalid_argument("engine.verify_output must be a boolean");
            }
            spec.engine.verify_output = engine.at("verify_output").get<bool>();
        }
        spec.limits.logical_write_bytes = number(limits, "logical_write_bytes");
        spec.limits.minimum_free_disk_bytes = number(limits, "minimum_free_disk_bytes");
        validate(spec);
        return spec;
    }

    Spec load_spec(const std::filesystem::path &path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            throw std::runtime_error("cannot open configuration: " + path.string());
        }
        std::string text(64 * 1024 + 1, '\0');
        input.read(text.data(), static_cast<std::streamsize>(text.size()));
        if (input.bad())
        {
            throw std::runtime_error("cannot read configuration: " + path.string());
        }
        text.resize(static_cast<std::size_t>(input.gcount()));
        return parse_spec(text);
    }

    Json spec_json(const Spec &spec)
    {
        validate(spec);
        // Optional fields are emitted only when they differ from the defaults so that existing
        // specifications keep their canonical encoding, fingerprints, and plan checksums.
        Json dataset{{"records", spec.dataset.records}, {"seed", spec.dataset.seed}, {"key_bytes", spec.dataset.key_bytes}, {"value_bytes", spec.dataset.value_bytes}, {"overwrite_rounds", spec.dataset.overwrite_rounds}};
        if (spec.dataset.load_mode != DatasetSpec{}.load_mode)
        {
            dataset["load_mode"] = spec.dataset.load_mode;
        }
        Json engine{{"compression", spec.engine.compression}, {"write_buffer_bytes", spec.engine.write_buffer_bytes}, {"target_file_bytes", spec.engine.target_file_bytes}, {"block_cache_bytes", spec.engine.block_cache_bytes}, {"block_bytes", spec.engine.block_bytes}, {"background_jobs", spec.engine.background_jobs}, {"levels", spec.engine.levels}};
        const EngineSpec defaults;
        if (spec.engine.max_subcompactions != defaults.max_subcompactions)
        {
            engine["max_subcompactions"] = spec.engine.max_subcompactions;
        }
        if (spec.engine.max_bytes_for_level_base)
        {
            engine["max_bytes_for_level_base"] = *spec.engine.max_bytes_for_level_base;
        }
        if (spec.engine.max_write_buffer_number != defaults.max_write_buffer_number)
        {
            engine["max_write_buffer_number"] = spec.engine.max_write_buffer_number;
        }
        if (spec.engine.level0_file_num_compaction_trigger)
        {
            engine["level0_file_num_compaction_trigger"] = *spec.engine.level0_file_num_compaction_trigger;
        }
        if (spec.engine.level0_slowdown_writes_trigger)
        {
            engine["level0_slowdown_writes_trigger"] = *spec.engine.level0_slowdown_writes_trigger;
        }
        if (spec.engine.level0_stop_writes_trigger)
        {
            engine["level0_stop_writes_trigger"] = *spec.engine.level0_stop_writes_trigger;
        }
        if (spec.engine.use_direct_io)
        {
            engine["use_direct_io"] = true;
        }
        if (!spec.engine.verify_output)
        {
            engine["verify_output"] = false;
        }
        return {{"schema_version", 1U},
                {"dataset", dataset},
                {"engine", engine},
                {"limits", {{"logical_write_bytes", spec.limits.logical_write_bytes}, {"minimum_free_disk_bytes", spec.limits.minimum_free_disk_bytes}}}};
    }

}
