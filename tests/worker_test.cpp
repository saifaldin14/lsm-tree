#include "lsm_lab/dataset.hpp"
#include "lsm_lab/load.hpp"
#include "lsm_lab/block_transform.hpp"
#include "lsm_lab/remote_compaction.hpp"
#include "lsm_lab/worker.hpp"
#include "benchmark.grpc.pb.h"

#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <spawn.h>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>
#include <gtest/gtest.h>
#include <rocksdb/db.h>

extern char **environ;

namespace lsm_lab
{
    namespace
    {
        namespace wire = rpc::v1;

        class Process
        {
        public:
            Process(std::vector<std::string> arguments, std::filesystem::path log) : log_(std::move(log))
            {
                std::vector<char *> pointers;
                for (auto &argument : arguments)
                {
                    pointers.push_back(argument.data());
                }
                pointers.push_back(nullptr);
                posix_spawn_file_actions_t actions;
                require(posix_spawn_file_actions_init(&actions) == 0, "cannot initialize process actions");
                const auto opened = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log_.c_str(),
                                                                     O_WRONLY | O_CREAT | O_EXCL, 0600);
                const auto duplicated = posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
                const auto result = opened == 0 && duplicated == 0 ? posix_spawn(&pid_, pointers.front(), &actions, nullptr, pointers.data(), environ) : -1;
                posix_spawn_file_actions_destroy(&actions);
                require(result == 0, "cannot spawn process");
            }

            ~Process()
            {
                if (pid_ > 0)
                {
                    kill(pid_, SIGKILL);
                    waitpid(pid_, nullptr, 0);
                }
            }

            std::string ready(const std::filesystem::path &path)
            {
                const auto deadline = Clock::now() + std::chrono::seconds(20);
                while (Clock::now() < deadline)
                {
                    if (std::filesystem::exists(path))
                    {
                        return nlohmann::json::parse(read_file(path)).at("endpoint").get<std::string>();
                    }
                    int outcome = 0;
                    if (waitpid(pid_, &outcome, WNOHANG) == pid_)
                    {
                        pid_ = -1;
                        throw std::runtime_error("server exited before readiness: " + read_file(log_));
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                throw std::runtime_error("server readiness timeout: " + read_file(log_));
            }

            int stop()
            {
                require(pid_ > 0 && kill(pid_, SIGTERM) == 0, "cannot stop test process");
                return wait();
            }

            int wait()
            {
                const auto deadline = Clock::now() + std::chrono::seconds(20);
                while (Clock::now() < deadline)
                {
                    int outcome = 0;
                    if (waitpid(pid_, &outcome, WNOHANG) == pid_)
                    {
                        pid_ = -1;
                        return WIFEXITED(outcome) ? WEXITSTATUS(outcome) : 128 + WTERMSIG(outcome);
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                throw std::runtime_error("process completion timeout: " + read_file(log_));
            }

        private:
            pid_t pid_ = -1;
            std::filesystem::path log_;
        };

        class WorkerTest : public ::testing::Test
        {
        protected:
            void SetUp() override
            {
                auto pattern = (std::filesystem::temp_directory_path() / "lsm-worker-XXXXXX").string();
                std::vector<char> name(pattern.begin(), pattern.end());
                name.push_back('\0');
                const auto *created = ::mkdtemp(name.data());
                ASSERT_NE(created, nullptr);
                root = created;
                std::filesystem::create_directory(root / "shared");
                std::filesystem::create_directory(root / "shared" / ".lsm-staging");
                durable_json(root / "shared" / ".lsm-shared.json",
                             {{"owner", "lsm-lab"}, {"schema_version", 1}, {"shared_root_id", "test-root"}});
                spec.dataset.records = 32;
                spec.limits.minimum_free_disk_bytes = 1;
                create_dataset(spec, root / "shared" / "dataset");
                request.set_wire_version(wire_version);
                request.set_engine_fingerprint(engine_fingerprint());
                request.set_shared_root_id("test-root");
                request.set_resolved_spec_sha256(digest_hex(spec_json(spec).dump()));
                request.set_database_relative_path("dataset/db");
                request.set_backend("cpu_native");
                request.set_engine_input("intentionally malformed native payload");
                request.set_engine_input_sha256(digest_bytes(request.engine_input()));
                auto *identity = request.mutable_identity();
                identity->set_attempt_id(unique_id());
                identity->mutable_job()->set_run_id("test-run");
                identity->mutable_job()->set_db_id("test-db");
                identity->mutable_job()->set_db_session_id("test-session");
                identity->mutable_job()->set_job_id(1ULL << 32);
            }

            void TearDown() override
            {
                if (!root.empty())
                {
                    std::error_code error;
                    std::filesystem::remove_all(root, error);
                }
            }

            WorkerOptions options() const
            {
                return {root / "shared", root / "journal", 1, 4};
            }

            wire::AttemptRequest inspection(const Worker &worker) const
            {
                wire::AttemptRequest result;
                result.set_wire_version(wire_version);
                *result.mutable_identity() = request.identity();
                result.set_worker_instance_id(worker.capabilities().worker_instance_id());
                result.set_wait_ms(1000);
                return result;
            }

            std::filesystem::path root;
            Spec spec;
            wire::SubmitCompactionRequest request;
        };

        TEST_F(WorkerTest, PersistsFailureAndDeduplicatesActualRequest)
        {
            Worker worker(options());
            const auto first = worker.submit(request);
            EXPECT_EQ(first.identity().attempt_id(), request.identity().attempt_id());
            const auto second = worker.submit(request);
            EXPECT_EQ(second.identity().attempt_id(), first.identity().attempt_id());
            auto report = worker.inspect(inspection(worker));
            const auto deadline = Clock::now() + std::chrono::seconds(15);
            while ((report.state() == wire::ATTEMPT_QUEUED || report.state() == wire::ATTEMPT_RUNNING) && Clock::now() < deadline)
            {
                report = worker.inspect(inspection(worker));
            }
            EXPECT_EQ(report.state(), wire::ATTEMPT_FAILED);
            EXPECT_EQ(report.failure_code(), "EXECUTION_FAILED");
            EXPECT_TRUE(std::filesystem::is_regular_file(root / "journal" / request.identity().attempt_id() / "state.pb"));
            EXPECT_NO_THROW(verify_dataset(root / "shared" / "dataset"));
            auto altered = request;
            altered.set_engine_input("different");
            altered.set_engine_input_sha256(digest_bytes(altered.engine_input()));
            EXPECT_THROW(worker.submit(altered), std::runtime_error);
        }

        TEST_F(WorkerTest, RejectsMismatchesAndTraversalWithoutExecution)
        {
            Worker worker(options());
            auto invalid = request;
            invalid.set_backend("unavailable_backend");
            EXPECT_THROW(worker.submit(invalid), std::runtime_error);
            invalid = request;
            invalid.set_engine_fingerprint("wrong-engine");
            EXPECT_THROW(worker.submit(invalid), std::runtime_error);
            invalid = request;
            invalid.set_database_relative_path("../dataset/db");
            EXPECT_THROW(worker.submit(invalid), std::runtime_error);
            invalid = request;
            invalid.set_shared_root_id("wrong-root");
            EXPECT_THROW(worker.submit(invalid), std::runtime_error);
            EXPECT_TRUE(std::filesystem::is_empty(root / "shared" / ".lsm-staging"));
        }

        TEST_F(WorkerTest, CancellationAndRestartFenceOldInstance)
        {
            std::string previous_instance;
            {
                Worker worker(options());
                previous_instance = worker.capabilities().worker_instance_id();
                worker.submit(request);
                EXPECT_NO_THROW(worker.cancel(inspection(worker)));
            }
            Worker recovered(options());
            auto query = inspection(recovered);
            query.set_worker_instance_id(previous_instance);
            EXPECT_THROW(recovered.inspect(query), std::runtime_error);
            query.set_worker_instance_id(recovered.capabilities().worker_instance_id());
            const auto result = recovered.inspect(query);
            EXPECT_TRUE(result.state() == wire::ATTEMPT_CANCELLED || result.state() == wire::ATTEMPT_FAILED ||
                        result.state() == wire::ATTEMPT_ABANDONED);
            EXPECT_NO_THROW(verify_dataset(root / "shared" / "dataset"));
        }

        TEST_F(WorkerTest, JournalExclusionAndSecureEndpointRules)
        {
            Worker worker(options());
            EXPECT_THROW(Worker{options()}, std::runtime_error);
            EXPECT_TRUE(loopback_endpoint("127.0.0.1:50051"));
            EXPECT_TRUE(loopback_endpoint("[::1]:0"));
            EXPECT_FALSE(loopback_endpoint("127.0.0.1.evil:50051"));
            EXPECT_FALSE(loopback_endpoint("0.0.0.0:50051"));
            EXPECT_THROW(server_credentials("0.0.0.0:50051", {}), std::runtime_error);
            EXPECT_THROW(connect("10.0.0.1:50051", {}), std::runtime_error);
            EXPECT_EQ(digest_hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        }

        class CompactionBackendTest : public WorkerTest, public ::testing::WithParamInterface<std::string>
        {
        };

        TEST_P(CompactionBackendTest, RemoteProcessPreservesSnapshotsAndCommits)
        {
            Process worker({LSM_WORKER_EXE, "--shared-root", (root / "shared").string(), "--journal", (root / "journal").string(),
                            "--listen", "127.0.0.1:0", "--ready-file", (root / "worker.ready").string()},
                           root / "worker.log");
            const auto endpoint = worker.ready(root / "worker.ready");
            clone_dataset(root / "shared" / "dataset", root / "shared" / "trial");
            RemoteOptions remote_options;
            remote_options.shared_root = root / "shared";
            remote_options.database = root / "shared" / "trial" / "db";
            remote_options.endpoint = endpoint;
            remote_options.run_id = "snapshot-test";
            remote_options.backend = GetParam();
            remote_options.spec_sha256 = digest_hex(spec_json(spec).dump());
            auto events = std::make_shared<EventLog>(root / "events.jsonl");
            auto remote = std::make_shared<RemoteCompaction>(remote_options, events);
            auto engine = engine_options(spec);
            engine.disable_auto_compactions = true;
            engine.max_subcompactions = 2;
            engine.compaction_service = remote;
            engine.listeners.push_back(remote);
            std::unique_ptr<rocksdb::DB> database;
            check(rocksdb::DB::Open(engine, remote_options.database.string(), &database));
            const auto release = [&database](const rocksdb::Snapshot *snapshot)
            { database->ReleaseSnapshot(snapshot); };
            std::unique_ptr<const rocksdb::Snapshot, decltype(release)> snapshot(database->GetSnapshot(), release);
            rocksdb::WriteOptions writes;
            writes.sync = true;
            const auto key = record_key(spec.dataset, 0);
            const auto removed = record_key(spec.dataset, 1);
            const auto replacement = record_value(spec.dataset, 0, 17);
            check(database->Put(writes, key, replacement));
            check(database->Delete(writes, removed));
            check(database->Flush(rocksdb::FlushOptions()));
            rocksdb::CompactRangeOptions compact;
            compact.bottommost_level_compaction = rocksdb::BottommostLevelCompaction::kForce;
            const auto compacted = database->CompactRange(compact, nullptr, nullptr);
            ASSERT_TRUE(compacted.ok()) << compacted.ToString() << '\n'
                                        << read_file(root / "events.jsonl");
            rocksdb::ReadOptions historical;
            historical.snapshot = snapshot.get();
            std::string value;
            check(database->Get(historical, key, &value));
            EXPECT_EQ(value, record_value(spec.dataset, 0, spec.dataset.overwrite_rounds));
            check(database->Get(historical, removed, &value));
            EXPECT_EQ(value, record_value(spec.dataset, 1, spec.dataset.overwrite_rounds));
            check(database->Get(rocksdb::ReadOptions(), key, &value));
            EXPECT_EQ(value, replacement);
            EXPECT_TRUE(database->Get(rocksdb::ReadOptions(), removed, &value).IsNotFound());
            snapshot.reset();
            check(database->VerifyChecksum());
            check(database->Close());
            database.reset();
            const auto report = remote->summary();
            EXPECT_EQ(report.at("healthy"), true);
            EXPECT_GT(report.at("committed").get<std::uint64_t>(), 0U);
            EXPECT_EQ(report.at("local_fallback"), 0);
            if (GetParam() != "cpu_native")
            {
                EXPECT_GT(report.at("transformed_blocks").get<std::uint64_t>(), 0U);
            }
            if (GetParam() == "metal_batched" || GetParam() == "cuda_batched")
            {
                EXPECT_GT(report.at("gpu_blocks").get<std::uint64_t>(), 0U);
            }
            std::istringstream log(read_file(root / "events.jsonl"));
            std::map<std::string, std::vector<std::string>> phases;
            for (std::string line; std::getline(log, line);)
            {
                const auto event = nlohmann::json::parse(line);
                if (event.contains("attempt_id") && event.at("role") == "primary")
                {
                    phases[event.at("attempt_id").get<std::string>()].push_back(event.at("phase").get<std::string>());
                }
            }
            ASSERT_FALSE(phases.empty());
            for (const auto &[attempt_id, history] : phases)
            {
                static_cast<void>(attempt_id);
                EXPECT_EQ(history, (std::vector<std::string>{"submitted", "output_ready", "outputs_imported", "committed"}));
            }
            EXPECT_NO_THROW(verify_dataset(root / "shared" / "dataset"));
            EXPECT_EQ(worker.stop(), 0);
        }

        INSTANTIATE_TEST_SUITE_P(AvailableBackends, CompactionBackendTest, ::testing::ValuesIn(available_transforms()));

        TEST_P(CompactionBackendTest, StreamsMultipleBatchesAndReopensNative)
        {
            auto large = spec;
            large.dataset.records = 4097;
            large.dataset.value_bytes = 2048;
            create_dataset(large, root / "shared" / "large-baseline");
            clone_dataset(root / "shared" / "large-baseline", root / "shared" / "large-trial");
            Process worker({LSM_WORKER_EXE, "--shared-root", (root / "shared").string(), "--journal", (root / "large-journal").string(),
                            "--listen", "127.0.0.1:0", "--ready-file", (root / "large-worker.ready").string()},
                           root / "large-worker.log");
            RemoteOptions remote_options;
            remote_options.shared_root = root / "shared";
            remote_options.database = root / "shared" / "large-trial" / "db";
            remote_options.endpoint = worker.ready(root / "large-worker.ready");
            remote_options.run_id = "stream-test";
            remote_options.spec_sha256 = digest_hex(spec_json(large).dump());
            remote_options.backend = GetParam();
            auto remote = std::make_shared<RemoteCompaction>(remote_options, std::make_shared<EventLog>(root / "large-events.jsonl"));
            auto engine = engine_options(large);
            engine.disable_auto_compactions = true;
            engine.compaction_service = remote;
            engine.listeners.push_back(remote);
            std::unique_ptr<rocksdb::DB> database;
            check(rocksdb::DB::Open(engine, remote_options.database.string(), &database));
            rocksdb::WriteOptions writes;
            writes.sync = true;
            rocksdb::WriteBatch batch;
            for (std::uint64_t index = 0; index < large.dataset.records; ++index)
            {
                check(batch.Put(record_key(large.dataset, index), record_value(large.dataset, index, 19)));
                if (batch.GetDataSize() >= 256 * 1024 || index + 1 == large.dataset.records)
                {
                    check(database->Write(writes, &batch));
                    batch.Clear();
                }
            }
            check(database->Flush(rocksdb::FlushOptions()));
            rocksdb::CompactRangeOptions compact;
            compact.bottommost_level_compaction = rocksdb::BottommostLevelCompaction::kForce;
            const auto compacted = database->CompactRange(compact, nullptr, nullptr);
            ASSERT_TRUE(compacted.ok()) << compacted.ToString() << '\n'
                                        << read_file(root / "large-events.jsonl");
            check(database->Close());
            database.reset();
            const auto metrics = remote->summary();
            ASSERT_EQ(metrics.at("healthy"), true);
            EXPECT_GT(metrics.at("committed").get<std::uint64_t>(), 0U);
            if (GetParam() != "cpu_native")
            {
                EXPECT_GT(metrics.at("transformed_blocks").get<std::uint64_t>(), 32U);
            }
            check(rocksdb::DB::OpenForReadOnly(engine_options(large), remote_options.database.string(), &database));
            for (std::uint64_t index = 0; index < large.dataset.records; ++index)
            {
                std::string value;
                check(database->Get(rocksdb::ReadOptions(), record_key(large.dataset, index), &value));
                ASSERT_EQ(value, record_value(large.dataset, index, 19));
            }
            check(database->VerifyChecksum());
            check(database->Close());
            EXPECT_EQ(worker.stop(), 0);
        }

        TEST_F(WorkerTest, ForegroundRpcPersistsAcrossSeparateProcesses)
        {
            Process worker({LSM_WORKER_EXE, "--shared-root", (root / "shared").string(), "--journal", (root / "journal").string(),
                            "--listen", "127.0.0.1:0", "--ready-file", (root / "worker.ready").string()},
                           root / "worker.log");
            const auto worker_endpoint = worker.ready(root / "worker.ready");
            std::vector<std::string> modes{"native_cpu", "remote_worker"};
#ifdef LSM_HAVE_CUDA
            modes.push_back("cuda_local");
#endif
            for (const auto &mode : modes)
            {
                const auto trial = root / "shared" / mode;
                clone_dataset(root / "shared" / "dataset", trial);
                const auto ready = root / (mode + ".ready");
                std::vector<std::string> arguments{LSM_DB_EXE, "--dataset", trial.string(), "--run-id", mode, "--mode", mode,
                                                   "--listen", "127.0.0.1:0", "--ready-file", ready.string(), "--events", (root / (mode + ".jsonl")).string(),
                                                   "--summary", (root / (mode + ".summary")).string()};
                if (mode == "remote_worker")
                {
                    arguments.insert(arguments.end(), {"--worker", worker_endpoint, "--shared-root", (root / "shared").string()});
                }
                Process primary(arguments, root / (mode + ".log"));
                auto stub = wire::BenchmarkService::NewStub(connect(primary.ready(ready), {}));
                wire::PutRequest put;
                put.mutable_identity()->set_wire_version(wire_version);
                put.mutable_identity()->set_run_id(mode);
                put.mutable_identity()->set_request_id(1);
                put.set_key(record_key(spec.dataset, 0));
                put.set_value(record_value(spec.dataset, 0, 23));
                wire::WriteResponse written;
                grpc::ClientContext put_context;
                put_context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
                const auto put_status = stub->Put(&put_context, put, &written);
                ASSERT_TRUE(put_status.ok()) << put_status.error_message();
                wire::DeleteRequest remove;
                *remove.mutable_identity() = put.identity();
                remove.set_key(record_key(spec.dataset, 1));
                grpc::ClientContext remove_context;
                remove_context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
                ASSERT_TRUE(stub->Delete(&remove_context, remove, &written).ok());
                wire::MaintenanceRequest maintenance;
                *maintenance.mutable_identity() = put.identity();
                maintenance.set_compact(true);
                maintenance.set_verify_checksums(true);
                maintenance.set_timeout_seconds(30);
                wire::MaintenanceResponse drained;
                grpc::ClientContext drain_context;
                drain_context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(60));
                const auto drain_status = stub->Maintain(&drain_context, maintenance, &drained);
                ASSERT_TRUE(drain_status.ok()) << drain_status.error_message();
                EXPECT_TRUE(drained.checksums_verified());
                const auto statistics = nlohmann::json::parse(drained.statistics_json());
                const auto properties = statistics.at("properties");
                EXPECT_TRUE(properties.at("rocksdb.num-files-at-level0").is_number_unsigned());
                EXPECT_GT(statistics.at("compactions").at("successful_jobs").get<std::uint64_t>(), 0U);
                EXPECT_GT(statistics.at("compactions").at("elapsed_micros_total").get<std::uint64_t>(), 0U);
                if (mode == "remote_worker")
                {
                    EXPECT_EQ(statistics.at("remote").at("healthy"), true);
                    EXPECT_GT(statistics.at("remote").at("committed").get<std::uint64_t>(), 0U);
                }
                if (mode == "cuda_local")
                {
                    const auto &local = statistics.at("local_transform");
                    EXPECT_GT(local.at("blocks").get<std::uint64_t>(), 0U);
                    EXPECT_GT(local.at("batches").get<std::uint64_t>(), 0U);
                    EXPECT_LE(local.at("batches").get<std::uint64_t>(),
                              local.at("compaction_sessions").get<std::uint64_t>() * 2);
                }
                EXPECT_EQ(primary.stop(), 0);
                std::unique_ptr<rocksdb::DB> reopened;
                check(rocksdb::DB::OpenForReadOnly(engine_options(spec), (trial / "db").string(), &reopened));
                std::string value;
                check(reopened->Get(rocksdb::ReadOptions(), put.key(), &value));
                EXPECT_EQ(value, put.value());
                EXPECT_TRUE(reopened->Get(rocksdb::ReadOptions(), remove.key(), &value).IsNotFound());
                check(reopened->VerifyChecksum());
                check(reopened->Close());
            }
            EXPECT_EQ(worker.stop(), 0);
            EXPECT_NO_THROW(verify_dataset(root / "shared" / "dataset"));
        }

        TEST_F(WorkerTest, OpenLoopClientPairsSchedulesAndVerifiesEveryValue)
        {
            Process worker({LSM_WORKER_EXE, "--shared-root", (root / "shared").string(), "--journal", (root / "journal").string(),
                            "--listen", "127.0.0.1:0", "--ready-file", (root / "worker.ready").string(),
                            "--resources", (root / "worker.resources.jsonl").string()},
                           root / "worker.log");
            const auto worker_endpoint = worker.ready(root / "worker.ready");
            std::string fingerprint;
            for (const std::string mode : {"native_cpu", "remote_worker"})
            {
                const auto trial = root / "shared" / mode;
                clone_dataset(root / "shared" / "dataset", trial);
                const auto ready = root / (mode + ".ready");
                std::vector<std::string> arguments{LSM_DB_EXE, "--dataset", trial.string(), "--run-id", mode, "--mode", mode,
                                                   "--listen", "127.0.0.1:0", "--ready-file", ready.string(), "--events", (root / (mode + ".events")).string(),
                                                   "--summary", (root / (mode + ".summary")).string()};
                if (mode == "remote_worker")
                    arguments.insert(arguments.end(), {"--worker", worker_endpoint, "--shared-root", (root / "shared").string()});
                Process primary(arguments, root / (mode + ".log"));
                const auto endpoint = primary.ready(ready);
                const auto output = root / (mode + "-load");
                Process load({LSM_LOADGEN_EXE, "--endpoint", endpoint, "--run-id", mode, "--require-mode", mode,
                              "--output", output.string(), "--requests", "120", "--warmup-requests", "32", "--rate", "500"},
                             root / (mode + "-load.log"));
                ASSERT_EQ(load.wait(), 0) << read_file(root / (mode + "-load.log"));
                const auto result = nlohmann::json::parse(read_file(output / "result.json"));
                EXPECT_EQ(result.at("resources").at("status"), "complete");
                EXPECT_GT(result.at("resource_window").at("measurement_start_ns"), result.at("resource_window").at("load_start_ns"));
                EXPECT_TRUE(std::filesystem::file_size(output / "resources.jsonl") > 0);
                EXPECT_EQ(result.at("measurement").at("completed"), 120);
                EXPECT_EQ(result.at("warmup").at("completed"), 32);
                EXPECT_EQ(result.at("measurement").at("queue_rejected"), 0);
                EXPECT_EQ(result.at("read_latency").at("count").get<std::uint64_t>() +
                              result.at("write_latency").at("count").get<std::uint64_t>(),
                          120U);
                EXPECT_TRUE(std::filesystem::file_size(output / "read.hdr") > 0);
                std::istringstream samples(read_file(output / "primary-metrics.jsonl"));
                for (std::string sample_line; std::getline(samples, sample_line);)
                {
                    const auto sample = nlohmann::json::parse(sample_line);
                    EXPECT_LE(sample.at("load_sample_start_ns").get<std::int64_t>(), sample.at("load_elapsed_ns").get<std::int64_t>());
                }
                const auto current = result.at("schedule_sha256").get<std::string>();
                if (fingerprint.empty())
                    fingerprint = current;
                EXPECT_EQ(fingerprint, current);
                EXPECT_EQ(primary.stop(), 0);
                const auto primary_summary = nlohmann::json::parse(read_file(root / (mode + ".summary")));
                EXPECT_EQ(primary_summary.at("resources").at("status"), "complete");
                const auto verification = verify_trial(trial, output);
                EXPECT_EQ(verification.at("records_verified"), spec.dataset.records);
                EXPECT_EQ(verification.at("native_checksums_verified"), true);
                std::ofstream(output / "final-versions.jsonl", std::ios::app) << "invalid\n";
                EXPECT_THROW(verify_trial(trial, output), std::exception);
                LoadOptions expected_options;
                expected_options.rate = 500;
                std::map<std::uint64_t, std::uint64_t> versions;
                for (std::uint64_t request_id = 0; request_id < 152; ++request_id)
                {
                    const auto operation = scheduled_operation(spec.dataset, expected_options, request_id);
                    if (operation.write)
                        versions[operation.key_index] = operation.generation;
                }
                std::unique_ptr<rocksdb::DB> database;
                check(rocksdb::DB::OpenForReadOnly(engine_options(spec), (trial / "db").string(), &database));
                for (std::uint64_t index = 0; index < spec.dataset.records; ++index)
                {
                    std::string value;
                    check(database->Get(rocksdb::ReadOptions(), record_key(spec.dataset, index), &value));
                    const auto previous = versions.find(index);
                    const auto generation = previous == versions.end() ? spec.dataset.overwrite_rounds : previous->second;
                    EXPECT_EQ(value, record_value(spec.dataset, index, generation));
                }
                check(database->VerifyChecksum());
                check(database->Close());
            }
            EXPECT_EQ(worker.stop(), 0);
            const auto stopped_worker = nlohmann::json::parse(read_file(root / "worker.ready"));
            EXPECT_EQ(stopped_worker.at("resources").at("status"), "complete");
            EXPECT_TRUE(std::filesystem::file_size(root / "worker.resources.jsonl") > 0);
        }

        TEST_F(WorkerTest, OpenLoopOverloadIsRecordedAndBudgetRefusesWrites)
        {
            const auto trial = root / "shared" / "trial";
            clone_dataset(root / "shared" / "dataset", trial);
            Process primary({LSM_DB_EXE, "--dataset", trial.string(), "--run-id", "overload", "--listen", "127.0.0.1:0",
                             "--ready-file", (root / "db.ready").string(), "--events", (root / "db.events").string(),
                             "--summary", (root / "db.summary").string()},
                            root / "db.log");
            const auto endpoint = primary.ready(root / "db.ready");
            Process limited({LSM_LOADGEN_EXE, "--endpoint", endpoint, "--run-id", "overload", "--output", (root / "rejected").string(),
                             "--requests", "1", "--read-percent", "0", "--write-budget-bytes", "1"},
                            root / "limited.log");
            EXPECT_EQ(limited.wait(), 1);
            EXPECT_FALSE(std::filesystem::exists(root / "rejected"));
            Process load({LSM_LOADGEN_EXE, "--endpoint", endpoint, "--run-id", "overload", "--output", (root / "load").string(),
                          "--requests", "1000", "--rate", "10000000", "--threads", "1", "--queue-capacity", "1"},
                         root / "load.log");
            ASSERT_EQ(load.wait(), 3) << read_file(root / "load.log");
            const auto result = nlohmann::json::parse(read_file(root / "load" / "result.json"));
            EXPECT_EQ(result.at("valid"), false);
            const auto &counts = result.at("measurement");
            EXPECT_GT(counts.at("queue_rejected").get<std::uint64_t>(), 0U);
            EXPECT_EQ(counts.at("completed").get<std::uint64_t>() + counts.at("failed").get<std::uint64_t>() +
                          counts.at("expired_before_send").get<std::uint64_t>() + counts.at("blocked_by_indeterminate_write").get<std::uint64_t>() +
                          counts.at("queue_rejected").get<std::uint64_t>(),
                      1000U);
            EXPECT_EQ(primary.stop(), 0);
        }
    }
}