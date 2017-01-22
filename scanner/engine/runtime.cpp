/* Copyright 2016 Carnegie Mellon University, NVIDIA Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "scanner/api/run.h"
#include "scanner/engine/runtime.h"
#include "scanner/engine/evaluator_registry.h"
#include "scanner/engine/kernel_registry.h"
#include "scanner/engine/save_worker.h"
#include "scanner/engine/evaluate_worker.h"
#include "scanner/engine/load_worker.h"
#include "scanner/engine/db.h"
#include "scanner/engine/rpc.grpc.pb.h"

#include <grpc++/server.h>
#include <grpc++/server_builder.h>
#include <grpc++/security/server_credentials.h>
#include <grpc++/security/credentials.h>
#include <grpc++/create_channel.h>

#include <thread>

using storehouse::StoreResult;
using storehouse::WriteFile;
using storehouse::RandomReadFile;

namespace scanner {
namespace internal {

void create_io_items(
  const proto::TaskSet& task_set,
  std::vector<IOItem>& io_items,
  std::vector<LoadWorkEntry>& load_work_entries)
{
  const i32 io_item_size = rows_per_io_item();
  auto& tasks = task_set.tasks();
  i32 warmup_size = 0;
  i32 total_rows = 0;
  for (size_t i = 0; i < tasks.size(); ++i) {
    auto& task = tasks.Get(i);
    assert(task.samples().size() > 0);
    i64 item_id = 0;
    i64 rows_in_task = static_cast<i64>(task.samples(0).rows().size());
    i64 allocated_rows = 0;
    while (allocated_rows < rows_in_task) {
      i64 rows_to_allocate =
        std::min((i64)io_item_size, rows_in_task - allocated_rows);

      IOItem item;
      item.table_id = i;
      item.item_id = item_id++;
      item.start_row = allocated_rows;
      item.end_row = allocated_rows + rows_to_allocate;
      io_items.push_back(item);

      LoadWorkEntry load_item;
      load_item.set_io_item_index(io_items.size() - 1);
      for (auto& sample : task.samples()) {
        i32 sample_job_id = sample.job_id();
        i32 sample_table_id = sample.table_id();

        proto::TableSample* load_sample = load_item.add_samples();
        load_sample->set_job_id(sample_job_id);
        load_sample->set_table_id(sample_table_id);
        for (auto col : sample.column_ids()) {
          load_sample->add_column_ids(col);
        }
        i64 e = allocated_rows + rows_to_allocate;
        // Add extra frames for warmup
        i64 s = std::max(allocated_rows - warmup_size, 0L);
        for (; s < e; ++s) {
          load_sample->add_rows(sample.rows(s));
        }
      }
      load_work_entries.push_back(load_item);

      allocated_rows += rows_to_allocate;
    }
    total_rows += rows_in_task;
  }
}

class WorkerImpl final : public proto::Worker::Service {
public:
  WorkerImpl(DatabaseParameters& params, std::string master_address)
    : db_params_(params)
  {
    master_ = proto::Master::NewStub(
      grpc::CreateChannel(
        master_address,
        grpc::InsecureChannelCredentials()));

    proto::WorkerInfo worker_info;
    char hostname[1024];
    if (gethostname(hostname, 1024)) {
      LOG(FATAL) << "gethostname failed";
    }
    worker_info.set_address(std::string(hostname) + ":5002");

    grpc::ClientContext context;
    proto::Registration registration;
    master_->RegisterWorker(&context, worker_info, &registration);
    node_id_ = registration.node_id();

    storage_ =
      storehouse::StorageBackend::make_from_config(db_params_.storage_config);

    init_memory_allocators(params.memory_pool_config);
  }

  ~WorkerImpl() {
    delete storage_;
    destroy_memory_allocators();
  }

  grpc::Status NewJob(grpc::ServerContext* context,
                      const proto::JobParameters* job_params,
                      proto::Empty* empty) {
    timepoint_t base_time = now();
    const i32 work_item_size = rows_per_work_item();
    i32 warmup_size = 0;

    EvaluatorRegistry* evaluator_registry = get_evaluator_registry();
    KernelRegistry* kernel_registry = get_kernel_registry();
    std::vector<KernelFactory*> kernel_factories;
    std::vector<Kernel::Config> kernel_configs;
    i32 num_gpus = static_cast<i32>(GPU_DEVICE_IDS.size());
    auto& evaluators = job_params->task_set().evaluators();
    for (auto& evaluator : evaluators) {
      const std::string& name = evaluator.name();
      KernelFactory* kernel_factory =
        kernel_registry->get_kernel(name, evaluator.device_type());
      kernel_factories.push_back(kernel_factory);

      Kernel::Config kernel_config;
      kernel_config.args = std::vector<u8>(
        evaluator.kernel_args().begin(),
        evaluator.kernel_args().end());

      for (auto& input : evaluator.inputs()) {
        const proto::Evaluator& input_evaluator = evaluators.Get(
          input.evaluator_index());
        EvaluatorInfo* evaluator_info =
          evaluator_registry->get_evaluator_info(input_evaluator.name());
        // TODO: verify that input.columns() are all in
        // evaluator_info->output_columns()
        kernel_config.input_columns.insert(
          kernel_config.input_columns.end(),
          input.columns().begin(),
          input.columns().end());
      }

      DeviceType device_type = evaluator.device_type();
      if (device_type == DeviceType::CPU) {
        // TODO: multiple CPUs?
        kernel_config.devices.push_back(CPU_DEVICE);
      } else if (device_type == DeviceType::GPU) {
        // TODO: round robin GPUs within a node
        for (i32 i = 0; i < evaluator.device_count(); ++i) {
          i32 device_id = i % num_gpus;
          kernel_config.devices.push_back({device_type, device_id});
        }
      } else {
        LOG(FATAL) << "Unrecognized device type";
      }

      kernel_configs.push_back(kernel_config);
    }

    std::vector<IOItem> io_items;
    std::vector<LoadWorkEntry> load_work_entries;
    create_io_items(job_params->task_set(), io_items, load_work_entries);

    // Setup shared resources for distributing work to processing threads
    i64 accepted_items = 0;
    Queue<LoadWorkEntry> load_work;
    Queue<EvalWorkEntry> initial_eval_work;
    std::vector<std::vector<Queue<EvalWorkEntry>>> eval_work(PUS_PER_NODE);
    Queue<EvalWorkEntry> save_work;
    std::atomic<i64> retired_items{0};

    // Setup load workers
    std::vector<Profiler> load_thread_profilers(LOAD_WORKERS_PER_NODE,
                                                Profiler(base_time));
    std::vector<LoadThreadArgs> load_thread_args;
    for (i32 i = 0; i < LOAD_WORKERS_PER_NODE; ++i) {
      // Create IO thread for reading and decoding data
      load_thread_args.emplace_back(LoadThreadArgs{
          // Uniform arguments
          io_items, warmup_size,

            // Per worker arguments
            i, db_params_.storage_config, load_thread_profilers[i],

            // Queues
            load_work, initial_eval_work,
            });
    }
    std::vector<pthread_t> load_threads(LOAD_WORKERS_PER_NODE);
    for (i32 i = 0; i < LOAD_WORKERS_PER_NODE; ++i) {
      pthread_create(&load_threads[i], NULL, load_thread, &load_thread_args[i]);
    }

    // Setup evaluate workers
    std::vector<std::vector<Profiler>> eval_profilers(PUS_PER_NODE);
    std::vector<PreEvaluateThreadArgs> pre_eval_args;
    std::vector<EvaluateThreadArgs> eval_args;
    std::vector<PostEvaluateThreadArgs> post_eval_args;

    for (i32 pu = 0; pu < PUS_PER_NODE; ++pu) {
      std::vector<Queue<EvalWorkEntry>>& work_queues = eval_work[pu];
      std::vector<Profiler>& eval_thread_profilers = eval_profilers[pu];
      work_queues.resize(3);

      // Pre evaluate worker
      {
        Queue<EvalWorkEntry>* input_work_queue = &initial_eval_work;
        Queue<EvalWorkEntry>* output_work_queue = &work_queues[0];
        pre_eval_args.emplace_back(PreEvaluateThreadArgs{
            // Uniform arguments
            io_items, warmup_size,

              // Per worker arguments
              pu, eval_thread_profilers[0],

              // Queues
              *input_work_queue, *output_work_queue});
      }

      // Evaluate worker
      {
        // Input work queue
        Queue<EvalWorkEntry>* input_work_queue = &work_queues[1];
        // Create new queue for output, reuse previous queue as input
        Queue<EvalWorkEntry>* output_work_queue = &work_queues[2];
        // Create eval thread for passing data through neural net
        eval_args.emplace_back(EvaluateThreadArgs{
            // Uniform arguments
            io_items, warmup_size,

              // Per worker arguments
              pu, kernel_factories, kernel_configs,
              eval_thread_profilers[1],

              // Queues
              *input_work_queue, *output_work_queue});
      }

      // Post evaluate worker
      {
        Queue<EvalWorkEntry>* input_work_queue = &work_queues[2];
        Queue<EvalWorkEntry>* output_work_queue = &save_work;
        post_eval_args.emplace_back(PostEvaluateThreadArgs{
            // Uniform arguments
            io_items, warmup_size,

              // Per worker arguments
              pu, eval_thread_profilers[2],

              // Queues
              *input_work_queue, *output_work_queue});
      }
    }

    // Launch eval worker threads
    std::vector<pthread_t> pre_eval_threads(PUS_PER_NODE);
    std::vector<pthread_t> eval_threads(PUS_PER_NODE);
    std::vector<pthread_t> post_eval_threads(PUS_PER_NODE);
    for (i32 pu = 0; pu < PUS_PER_NODE; ++pu) {
      // Pre thread
      pthread_create(&pre_eval_threads[pu], NULL, pre_evaluate_thread,
                     &pre_eval_args[pu]);
      // Evaluator threads
      pthread_create(&eval_threads[pu], NULL, evaluate_thread,
                     &eval_args[pu]);
      // Post threads
      pthread_create(&post_eval_threads[pu], NULL, post_evaluate_thread,
                     &post_eval_args[pu]);
    }

    // Setup save workers
    std::vector<Profiler> save_thread_profilers(SAVE_WORKERS_PER_NODE,
                                                Profiler(base_time));
    std::vector<SaveThreadArgs> save_thread_args;
    for (i32 i = 0; i < SAVE_WORKERS_PER_NODE; ++i) {
      // Create IO thread for reading and decoding data
      save_thread_args.emplace_back(SaveThreadArgs{
          // Uniform arguments
          job_params->job_name(), io_items,

            // Per worker arguments
            i, db_params_.storage_config, save_thread_profilers[i],

            // Queues
            save_work, retired_items});
    }
    std::vector<pthread_t> save_threads(SAVE_WORKERS_PER_NODE);
    for (i32 i = 0; i < SAVE_WORKERS_PER_NODE; ++i) {
      pthread_create(&save_threads[i], NULL, save_thread, &save_thread_args[i]);
    }

    timepoint_t start_time = now();

    // Monitor amount of work left and request more when running low
    while (true) {
      i32 local_work = accepted_items - retired_items;
      if (local_work < PUS_PER_NODE * TASKS_IN_QUEUE_PER_PU) {
        grpc::ClientContext context;
        proto::Empty empty;
        proto::IOItem io_item;
        master_->NextIOItem(&context, empty, &io_item);

        i32 next_item = io_item.item_id();
        if (next_item == -1) {
          // No more work left
          break;
        } else {
          LoadWorkEntry& entry = load_work_entries[next_item];
          load_work.push(entry);
          accepted_items++;
        }
      }
      std::this_thread::yield();
    }

    // Push sentinel work entries into queue to terminate load threads
    for (i32 i = 0; i < LOAD_WORKERS_PER_NODE; ++i) {
      LoadWorkEntry entry;
      entry.set_io_item_index(-1);
      load_work.push(entry);
    }

    for (i32 i = 0; i < LOAD_WORKERS_PER_NODE; ++i) {
      // Wait until load has finished
      void* result;
      i32 err = pthread_join(load_threads[i], &result);
      if (err != 0) {
        fprintf(stderr, "error in pthread_join of load thread\n");
        exit(EXIT_FAILURE);
      }
      free(result);
    }

    // Push sentinel work entries into queue to terminate eval threads
    for (i32 i = 0; i < PUS_PER_NODE; ++i) {
      EvalWorkEntry entry;
      entry.io_item_index = -1;
      initial_eval_work.push(entry);
    }

    for (i32 i = 0; i < PUS_PER_NODE; ++i) {
      // Wait until pre eval has finished
      void* result;
      i32 err = pthread_join(pre_eval_threads[i], &result);
      if (err != 0) {
        fprintf(stderr, "error in pthread_join of pre eval thread\n");
        exit(EXIT_FAILURE);
      }
      free(result);
    }

    for (i32 pu = 0; pu < PUS_PER_NODE; ++pu) {
      EvalWorkEntry entry;
      entry.io_item_index = -1;
      eval_work[pu][1].push(entry);
    }
    for (i32 pu = 0; pu < PUS_PER_NODE; ++pu) {
      // Wait until eval has finished
      void* result;
      i32 err = pthread_join(eval_threads[pu], &result);
      if (err != 0) {
        fprintf(stderr, "error in pthread_join of eval thread\n");
        exit(EXIT_FAILURE);
      }
      free(result);
    }

    // Terminate post eval threads
    for (i32 pu = 0; pu < PUS_PER_NODE; ++pu) {
      EvalWorkEntry entry;
      entry.io_item_index = -1;
      eval_work[pu][2].push(entry);
    }
    for (i32 pu = 0; pu < PUS_PER_NODE; ++pu) {
      // Wait until eval has finished
      void* result;
      i32 err = pthread_join(post_eval_threads[pu], &result);
      if (err != 0) {
        fprintf(stderr, "error in pthread_join of post eval thread\n");
        exit(EXIT_FAILURE);
      }
      free(result);
    }

    // Push sentinel work entries into queue to terminate save threads
    for (i32 i = 0; i < SAVE_WORKERS_PER_NODE; ++i) {
      EvalWorkEntry entry;
      entry.io_item_index = -1;
      save_work.push(entry);
    }

    for (i32 i = 0; i < SAVE_WORKERS_PER_NODE; ++i) {
      // Wait until eval has finished
      void* result;
      i32 err = pthread_join(save_threads[i], &result);
      if (err != 0) {
        fprintf(stderr, "error in pthread_join of save thread\n");
        exit(EXIT_FAILURE);
      }
      free(result);
    }


    // Ensure all files are flushed
#ifdef SCANNER_PROFILING
    std::fflush(NULL);
    sync();
#endif
    // Write out total time interval
    timepoint_t end_time = now();

    // Execution done, write out profiler intervals for each worker
    // TODO: job_name -> job_id?
    std::string profiler_file_name =
      job_profiler_path(0xdeadbeef, node_id_);
    std::unique_ptr<WriteFile> profiler_output;
    BACKOFF_FAIL(
      make_unique_write_file(storage_, profiler_file_name, profiler_output));

    i64 base_time_ns =
      std::chrono::time_point_cast<std::chrono::nanoseconds>(base_time)
      .time_since_epoch()
      .count();
    i64 start_time_ns =
      std::chrono::time_point_cast<std::chrono::nanoseconds>(start_time)
      .time_since_epoch()
      .count();
    i64 end_time_ns =
      std::chrono::time_point_cast<std::chrono::nanoseconds>(end_time)
      .time_since_epoch()
      .count();
    s_write(profiler_output.get(), start_time_ns);
    s_write(profiler_output.get(), end_time_ns);

    i64 out_rank = node_id_;
    // Load worker profilers
    u8 load_worker_count = LOAD_WORKERS_PER_NODE;
    s_write(profiler_output.get(), load_worker_count);
    for (i32 i = 0; i < LOAD_WORKERS_PER_NODE; ++i) {
      write_profiler_to_file(profiler_output.get(), out_rank, "load", "", i,
                             load_thread_profilers[i]);
    }

    // Evaluate worker profilers
    u8 eval_worker_count = PUS_PER_NODE;
    s_write(profiler_output.get(), eval_worker_count);
    u8 profilers_per_chain = 3;
    s_write(profiler_output.get(), profilers_per_chain);
    for (i32 pu = 0; pu < PUS_PER_NODE; ++pu) {
      i32 i = pu;
      {
        std::string tag = "pre";
        write_profiler_to_file(profiler_output.get(), out_rank, "eval", tag, i,
                               eval_profilers[pu][0]);
      }
      {
        std::string tag = "eval";
        write_profiler_to_file(profiler_output.get(), out_rank, "eval", tag, i,
                               eval_profilers[pu][1]);
      }
      {
        std::string tag = "post";
        write_profiler_to_file(
          profiler_output.get(), out_rank, "eval", tag, i,
          eval_profilers[pu][2]);
      }
    }

    // Save worker profilers
    u8 save_worker_count = SAVE_WORKERS_PER_NODE;
    s_write(profiler_output.get(), save_worker_count);
    for (i32 i = 0; i < SAVE_WORKERS_PER_NODE; ++i) {
      write_profiler_to_file(profiler_output.get(), out_rank, "save", "", i,
                             save_thread_profilers[i]);
    }

    BACKOFF_FAIL(profiler_output->save());

    return grpc::Status::OK;
  }

private:
  std::unique_ptr<proto::Master::Stub> master_;
  storehouse::StorageConfig* storage_config_;
  DatabaseParameters db_params_;
  i32 node_id_;
  storehouse::StorageBackend* storage_;
};

class MasterImpl final : public proto::Master::Service {
public:
  MasterImpl(DatabaseParameters& params)
    : next_io_item_to_allocate_(0),
    num_io_items_(0),
    db_params_(params)
    {
      storage_ =
        storehouse::StorageBackend::make_from_config(db_params_.storage_config);
    }

  ~MasterImpl() {
    delete storage_;
  }

  grpc::Status RegisterWorker(grpc::ServerContext* context,
                              const proto::WorkerInfo* worker_info,
                              proto::Empty* empty) {
    workers_.push_back(
      proto::Worker::NewStub(
        grpc::CreateChannel(
          worker_info->address(),
          grpc::InsecureChannelCredentials())));
  }

  grpc::Status NextIOItem(grpc::ServerContext* context,
                          const proto::Empty* empty,
                          proto::IOItem* io_item) {
    if (next_io_item_to_allocate_ < num_io_items_) {
      io_item->set_item_id(next_io_item_to_allocate_);
      ++next_io_item_to_allocate_;
    } else {
      io_item->set_item_id(-1);
    }
    return grpc::Status::OK;
  }

  grpc::Status NewJob(grpc::ServerContext* context,
                      const proto::JobParameters* job_params,
                      proto::Empty* empty) {

    const i32 io_item_size = rows_per_io_item();
    const i32 work_item_size = rows_per_work_item();

    i32 warmup_size = 0;
    i32 total_rows = 0;

    proto::JobDescriptor job_descriptor;
    job_descriptor.set_io_item_size(io_item_size);
    job_descriptor.set_work_item_size(work_item_size);
    job_descriptor.set_num_nodes(workers_.size());

    auto& evaluators = job_params->task_set().evaluators();
    EvaluatorRegistry* evaluator_registry = get_evaluator_registry();
    EvaluatorInfo* output_evaluator = evaluator_registry->get_evaluator_info(
      evaluators.Get(evaluators.size()-1).name());
    const std::vector<std::string>& output_columns =
      output_evaluator->output_columns();
    for (size_t i = 0; i < output_columns.size(); ++i) {
      auto& col_name = output_columns[i];
      Column* col = job_descriptor.add_columns();
      col->set_id(i);
      col->set_name(col_name);
      col->set_type(ColumnType::None);
    }

    auto& tasks = job_params->task_set().tasks();
    job_descriptor.mutable_tasks()->CopyFrom(tasks);

    std::vector<IOItem> io_items;
    std::vector<LoadWorkEntry> load_work_entries;
    create_io_items(job_params->task_set(), io_items, load_work_entries);
    num_io_items_ = io_items.size();

    std::vector<std::thread> requests;
    for (auto& worker : workers_) {
      grpc::ClientContext context;
      proto::Empty empty;
      // TODO: this should probably use the grpc async stuff, but
      // hacking around it for now
      requests.push_back(
        std::thread([&]() {
            return worker->NewJob(&context, *job_params, &empty);
          }));
    }

    for (auto& r : requests) {
      r.join();
    }

    // Add job name into database metadata so we can look up what jobs have
    // been ran
    DatabaseMetadata meta = read_database_metadata(
      storage_, DatabaseMetadata::descriptor_path());
    i32 job_id = meta.add_job(job_params->job_name());
    write_database_metadata(storage_, meta);

    job_descriptor.set_id(job_id);
    job_descriptor.set_name(job_params->job_name());
    write_job_metadata(storage_, job_descriptor);
  }

private:
  i32 next_io_item_to_allocate_;
  i32 num_io_items_;
  std::vector<std::unique_ptr<proto::Worker::Stub>> workers_;
  DatabaseParameters db_params_;
  storehouse::StorageBackend* storage_;
};


proto::Master::Service* get_master_service(DatabaseParameters& param) {
  return new MasterImpl(param);
}

proto::Worker::Service *get_worker_service(DatabaseParameters &param,
                                           const std::string &master_address) {
  return new WorkerImpl(param, master_address);
}

}

}
