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
                    std::initializer_list<std::string_view> expected)
        {
            if (!object.is_object())
            {
                throw std::invalid_argument(section + " must be an object");
            }
            for (const auto &[key, value] : object.items())
            {
                (void)value;
                if (std::find(expected.begin(), expected.end(), key) == expected.end())
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
        range(spec.limits.logical_write_bytes, 1, std::numeric_limits<std::uint64_t>::max(),
              "limits.logical_write_bytes");
        range(spec.limits.minimum_free_disk_bytes, 1, std::numeric_limits<std::uint64_t>::max(),
              "limits.minimum_free_disk_bytes");
        if (spec.engine.compression != "lz4" && spec.engine.compression != "none")
        {
            throw std::invalid_argument("engine.compression must be lz4 or none");
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
        fields(dataset, "dataset", {"records", "seed", "key_bytes", "value_bytes", "overwrite_rounds"});
        const auto &engine = document.at("engine");
        fields(engine, "engine", {"compression", "write_buffer_bytes", "target_file_bytes", "block_cache_bytes", "block_bytes", "background_jobs", "levels"});
        const auto &limits = document.at("limits");
        fields(limits, "limits", {"logical_write_bytes", "minimum_free_disk_bytes"});
        Spec spec;
        spec.dataset = {number(dataset, "records"), number(dataset, "seed"),
                        number32(dataset, "key_bytes"), number32(dataset, "value_bytes"),
                        number32(dataset, "overwrite_rounds")};
        spec.engine = {engine.at("compression").get<std::string>(),
                       number(engine, "write_buffer_bytes"), number(engine, "target_file_bytes"),
                       number(engine, "block_cache_bytes"), number32(engine, "block_bytes"),
                       number32(engine, "background_jobs"), number32(engine, "levels")};
        spec.limits = {number(limits, "logical_write_bytes"), number(limits, "minimum_free_disk_bytes")};
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
        return {{"schema_version", 1U},
                {"dataset", {{"records", spec.dataset.records}, {"seed", spec.dataset.seed}, {"key_bytes", spec.dataset.key_bytes}, {"value_bytes", spec.dataset.value_bytes}, {"overwrite_rounds", spec.dataset.overwrite_rounds}}},
                {"engine", {{"compression", spec.engine.compression}, {"write_buffer_bytes", spec.engine.write_buffer_bytes}, {"target_file_bytes", spec.engine.target_file_bytes}, {"block_cache_bytes", spec.engine.block_cache_bytes}, {"block_bytes", spec.engine.block_bytes}, {"background_jobs", spec.engine.background_jobs}, {"levels", spec.engine.levels}}},
                {"limits", {{"logical_write_bytes", spec.limits.logical_write_bytes}, {"minimum_free_disk_bytes", spec.limits.minimum_free_disk_bytes}}}};
    }

}