// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "ocudu/phy/upper/channel_processors/pdsch/factories.h"
#include "logging_pdsch_processor_decorator.h"
#include "pdsch_block_processor_hw_impl.h"
#include "pdsch_block_processor_impl.h"
#include "pdsch_encoder_hw_impl.h"
#include "pdsch_encoder_impl.h"
#include "pdsch_modulator_impl.h"
#include "pdsch_processor_flexible_impl.h"
#include "pdsch_processor_impl.h"
#include "pdsch_processor_pool.h"
#include "pdsch_processor_validator_impl.h"
#include "ocudu/phy/support/support_factories.h"
#include "ocudu/support/error_handling.h"
#include <algorithm>
#include <cstdlib>
#include <limits>
#include <memory>

#ifdef ENABLE_CUDA
#include "cuda/pdsch_tb_encoder_cuda.h"
#include "pdsch_block_processor_gpu_impl.h"
#endif

using namespace ocudu;

namespace {

class pdsch_encoder_factory_sw : public pdsch_encoder_factory
{
private:
  std::shared_ptr<ldpc_encoder_factory>      encoder_factory;
  std::shared_ptr<ldpc_rate_matcher_factory> rate_matcher_factory;
  std::shared_ptr<ldpc_segmenter_tx_factory> segmenter_factory;

public:
  explicit pdsch_encoder_factory_sw(pdsch_encoder_factory_sw_configuration& config) :
    encoder_factory(std::move(config.encoder_factory)),
    rate_matcher_factory(std::move(config.rate_matcher_factory)),
    segmenter_factory(std::move(config.segmenter_factory))
  {
    ocudu_assert(encoder_factory, "Invalid LDPC encoder factory.");
    ocudu_assert(rate_matcher_factory, "Invalid LDPC RM factory.");
    ocudu_assert(segmenter_factory, "Invalid LDPC segmenter factory.");
  }

  std::unique_ptr<pdsch_encoder> create() override
  {
    return std::make_unique<pdsch_encoder_impl>(
        segmenter_factory->create(), encoder_factory->create(), rate_matcher_factory->create());
  }
};

/// HW-accelerated PDSCH encoder factory.
class pdsch_encoder_factory_hw : public pdsch_encoder_factory
{
private:
  std::shared_ptr<crc_calculator_factory>                crc_factory;
  std::shared_ptr<ldpc_segmenter_tx_factory>             segmenter_factory;
  std::shared_ptr<hal::hw_accelerator_pdsch_enc_factory> hw_encoder_factory;

public:
  explicit pdsch_encoder_factory_hw(const pdsch_encoder_factory_hw_configuration& config) :
    crc_factory(std::move(config.crc_factory)),
    segmenter_factory(std::move(config.segmenter_factory)),
    hw_encoder_factory(std::move(config.hw_encoder_factory))
  {
    ocudu_assert(crc_factory, "Invalid CRC factory.");
    ocudu_assert(segmenter_factory, "Invalid LDPC segmenter factory.");
    ocudu_assert(hw_encoder_factory, "Invalid hardware accelerator factory.");
  }

  std::unique_ptr<pdsch_encoder> create() override
  {
    pdsch_encoder_hw_impl::sch_crc crc = {
        crc_factory->create(crc_generator_poly::CRC16),
        crc_factory->create(crc_generator_poly::CRC24A),
        crc_factory->create(crc_generator_poly::CRC24B),
    };
    return std::make_unique<pdsch_encoder_hw_impl>(crc, segmenter_factory->create(), hw_encoder_factory->create());
  }
};

class pdsch_modulator_factory_sw : public pdsch_modulator_factory
{
private:
  std::shared_ptr<modulation_mapper_factory>       modulator_factory;
  std::shared_ptr<pseudo_random_generator_factory> prg_factory;
  std::shared_ptr<resource_grid_mapper_factory>    rg_mapper_factory;

public:
  pdsch_modulator_factory_sw(std::shared_ptr<modulation_mapper_factory>       modulator_factory_,
                             std::shared_ptr<pseudo_random_generator_factory> prg_factory_,
                             std::shared_ptr<resource_grid_mapper_factory>    rg_mapper_factory_) :
    modulator_factory(std::move(modulator_factory_)),
    prg_factory(std::move(prg_factory_)),
    rg_mapper_factory(std::move(rg_mapper_factory_))
  {
    ocudu_assert(modulator_factory, "Invalid modulator factory.");
    ocudu_assert(prg_factory, "Invalid PRG factory.");
    ocudu_assert(rg_mapper_factory, "Invalid resource grid mapper factory.");
  }

  std::unique_ptr<pdsch_modulator> create() override
  {
    return std::make_unique<pdsch_modulator_impl>(
        modulator_factory->create(), prg_factory->create(), rg_mapper_factory->create());
  }
};

class pdsch_processor_factory_sw : public pdsch_processor_factory
{
private:
  std::shared_ptr<pdsch_encoder_factory>        encoder_factory;
  std::shared_ptr<pdsch_modulator_factory>      modulator_factory;
  std::shared_ptr<dmrs_pdsch_processor_factory> dmrs_factory;
  std::shared_ptr<ptrs_pdsch_generator_factory> ptrs_factory;

public:
  pdsch_processor_factory_sw(std::shared_ptr<pdsch_encoder_factory>        encoder_factory_,
                             std::shared_ptr<pdsch_modulator_factory>      modulator_factory_,
                             std::shared_ptr<dmrs_pdsch_processor_factory> dmrs_factory_,
                             std::shared_ptr<ptrs_pdsch_generator_factory> ptrs_factory_) :
    encoder_factory(std::move(encoder_factory_)),
    modulator_factory(std::move(modulator_factory_)),
    dmrs_factory(std::move(dmrs_factory_)),
    ptrs_factory(std::move(ptrs_factory_))
  {
    ocudu_assert(encoder_factory, "Invalid encoder factory.");
    ocudu_assert(modulator_factory, "Invalid modulator factory.");
    ocudu_assert(dmrs_factory, "Invalid DM-RS factory.");
    ocudu_assert(ptrs_factory, "Invalid PT-RS factory.");
  }

  std::unique_ptr<pdsch_processor> create() override
  {
    return std::make_unique<pdsch_processor_impl>(
        encoder_factory->create(), modulator_factory->create(), dmrs_factory->create(), ptrs_factory->create());
  }

  std::unique_ptr<pdsch_pdu_validator> create_validator() override
  {
    return std::make_unique<pdsch_processor_validator_impl>();
  }
};

class pdsch_block_processor_factory_sw : public pdsch_block_processor_factory
{
public:
  pdsch_block_processor_factory_sw(std::shared_ptr<ldpc_encoder_factory>            encoder_factory_,
                                   std::shared_ptr<ldpc_rate_matcher_factory>       rate_matcher_factory_,
                                   std::shared_ptr<pseudo_random_generator_factory> prg_factory_,
                                   std::shared_ptr<modulation_mapper_factory>       modulator_factory_) :
    encoder_factory(std::move(encoder_factory_)),
    rate_matcher_factory(std::move(rate_matcher_factory_)),
    prg_factory(std::move(prg_factory_)),
    modulator_factory(std::move(modulator_factory_))
  {
    ocudu_assert(encoder_factory, "Invalid encoder factory.");
    ocudu_assert(rate_matcher_factory, "Invalid rate matcher factory.");
    ocudu_assert(prg_factory, "Invalid PRG factory.");
    ocudu_assert(modulator_factory, "Invalid modulator factory.");
  }

  std::unique_ptr<pdsch_block_processor> create() override
  {
    return std::make_unique<pdsch_block_processor_impl>(
        encoder_factory->create(), rate_matcher_factory->create(), prg_factory->create(), modulator_factory->create());
  }

private:
  std::shared_ptr<ldpc_encoder_factory>            encoder_factory;
  std::shared_ptr<ldpc_rate_matcher_factory>       rate_matcher_factory;
  std::shared_ptr<pseudo_random_generator_factory> prg_factory;
  std::shared_ptr<modulation_mapper_factory>       modulator_factory;
};

class pdsch_block_processor_factory_hw : public pdsch_block_processor_factory
{
public:
  pdsch_block_processor_factory_hw(std::shared_ptr<hal::hw_accelerator_pdsch_enc_factory> encoder_factory_,
                                   std::shared_ptr<pseudo_random_generator_factory>       prg_factory_,
                                   std::shared_ptr<modulation_mapper_factory>             modulator_factory_) :
    encoder_factory(std::move(encoder_factory_)),
    prg_factory(std::move(prg_factory_)),
    modulator_factory(std::move(modulator_factory_))
  {
    ocudu_assert(encoder_factory, "Invalid hardware-accelerated encoder factory.");
    ocudu_assert(prg_factory, "Invalid PRG factory.");
    ocudu_assert(modulator_factory, "Invalid modulator factory.");
  }

  std::unique_ptr<pdsch_block_processor> create() override
  {
    return std::make_unique<pdsch_block_processor_hw_impl>(
        encoder_factory->create(), prg_factory->create(), modulator_factory->create());
  }

private:
  std::shared_ptr<hal::hw_accelerator_pdsch_enc_factory> encoder_factory;
  std::shared_ptr<pseudo_random_generator_factory>       prg_factory;
  std::shared_ptr<modulation_mapper_factory>             modulator_factory;
};

class pdsch_block_processor_pool_factory_sw
{
public:
  pdsch_block_processor_pool_factory_sw(std::shared_ptr<pdsch_block_processor_factory> block_processor_factory_,
                                        unsigned                                       nof_concurrent_threads_) :
    block_processor_factory(std::move(block_processor_factory_)), nof_concurrent_threads(nof_concurrent_threads_)
  {
    ocudu_assert(block_processor_factory, "Invalid PDSCH block processor factory.");
    ocudu_assert(nof_concurrent_threads > 0, "Number of concurrent threads must be greater or equal to one.");
  }

  std::shared_ptr<pdsch_processor_flexible_impl::pdsch_block_processor_pool> create()
  {
    if (!pool) {
      // Create vector of PDSCH block processors.
      std::vector<std::unique_ptr<pdsch_block_processor>> instances(nof_concurrent_threads);
      std::generate(instances.begin(), instances.end(), [this]() { return block_processor_factory->create(); });
      report_fatal_error_if_not(
          std::all_of(instances.begin(), instances.end(), [](const auto& instance) { return instance != nullptr; }),
          "Failed to create one or more PDSCH block processors.");
      pool = std::make_shared<pdsch_processor_flexible_impl::pdsch_block_processor_pool>(instances);
    }

    return pool;
  }

  void reset() { pool.reset(); }

private:
  std::shared_ptr<pdsch_processor_flexible_impl::pdsch_block_processor_pool> pool;
  std::shared_ptr<pdsch_block_processor_factory>                             block_processor_factory;
  unsigned                                                                   nof_concurrent_threads;
};

class dmrs_generator_pool_factory_sw
{
public:
  dmrs_generator_pool_factory_sw(std::shared_ptr<dmrs_pdsch_processor_factory> dmrs_factory_,
                                 unsigned                                      nof_concurrent_threads_) :
    dmrs_factory(std::move(dmrs_factory_)), nof_concurrent_threads(nof_concurrent_threads_)
  {
    ocudu_assert(dmrs_factory, "Invalid DM-RS factory.");
    ocudu_assert(nof_concurrent_threads > 0, "Number of concurrent threads must be greater or equal to one.");
  }

  std::shared_ptr<pdsch_processor_flexible_impl::pdsch_dmrs_generator_pool> create()
  {
    if (!pool) {
      // Create vector of PDSCH DM-RS generators.
      std::vector<std::unique_ptr<dmrs_pdsch_processor>> instances(nof_concurrent_threads);
      std::generate(instances.begin(), instances.end(), [this]() { return dmrs_factory->create(); });
      report_fatal_error_if_not(
          std::all_of(instances.begin(), instances.end(), [](const auto& instance) { return instance != nullptr; }),
          "Failed to create one or more PDSCH DM-RS generators.");
      pool = std::make_shared<pdsch_processor_flexible_impl::pdsch_dmrs_generator_pool>(instances);
    }
    return pool;
  }

  void reset() { pool.reset(); }

private:
  std::shared_ptr<pdsch_processor_flexible_impl::pdsch_dmrs_generator_pool> pool;
  std::shared_ptr<dmrs_pdsch_processor_factory>                             dmrs_factory;
  unsigned                                                                  nof_concurrent_threads;
};

class ptrs_generator_pool_factory_sw
{
public:
  ptrs_generator_pool_factory_sw(std::shared_ptr<ptrs_pdsch_generator_factory> ptrs_factory_,
                                 unsigned                                      nof_concurrent_threads_) :
    ptrs_factory(std::move(ptrs_factory_)), nof_concurrent_threads(nof_concurrent_threads_)
  {
    ocudu_assert(ptrs_factory, "Invalid PT-RS factory.");
    ocudu_assert(nof_concurrent_threads > 0, "Number of concurrent threads must be greater or equal to one.");
  }

  std::shared_ptr<pdsch_processor_flexible_impl::pdsch_ptrs_generator_pool> create()
  {
    if (!pool) {
      // Create vector of PDSCH PT-RS generators.
      std::vector<std::unique_ptr<ptrs_pdsch_generator>> instances(nof_concurrent_threads);
      std::generate(instances.begin(), instances.end(), [this]() { return ptrs_factory->create(); });
      report_fatal_error_if_not(
          std::all_of(instances.begin(), instances.end(), [](const auto& instance) { return instance != nullptr; }),
          "Failed to create one or more PDSCH PT-RS generators.");
      pool = std::make_shared<pdsch_processor_flexible_impl::pdsch_ptrs_generator_pool>(instances);
    }
    return pool;
  }

  void reset() { pool.reset(); }

private:
  std::shared_ptr<pdsch_processor_flexible_impl::pdsch_ptrs_generator_pool> pool;
  std::shared_ptr<ptrs_pdsch_generator_factory>                             ptrs_factory;
  unsigned                                                                  nof_concurrent_threads;
};

class pdsch_processor_flexible_factory_sw : public pdsch_processor_factory
{
public:
  pdsch_processor_flexible_factory_sw(std::shared_ptr<ldpc_segmenter_tx_factory>     segmenter_factory_,
                                      std::shared_ptr<pdsch_block_processor_factory> block_processor_factory,
                                      std::shared_ptr<resource_grid_mapper_factory>  rg_mapper_factory_,
                                      std::shared_ptr<dmrs_pdsch_processor_factory>  dmrs_factory,
                                      std::shared_ptr<ptrs_pdsch_generator_factory>  ptrs_factory,
                                      task_executor&                                 executor_,
                                      unsigned                                       nof_concurrent_threads_,
                                      unsigned                                       cb_batch_length_) :
    segmenter_factory(std::move(segmenter_factory_)),
    rg_mapper_factory(std::move(rg_mapper_factory_)),
    executor(executor_),
    block_pool_factory(std::move(block_processor_factory), nof_concurrent_threads_),
    dmrs_pool_factory(std::move(dmrs_factory), nof_concurrent_threads_),
    ptrs_pool_factory(std::move(ptrs_factory), nof_concurrent_threads_),
    cb_batch_length(cb_batch_length_)
  {
    ocudu_assert(segmenter_factory, "Invalid segmenter factory.");
    ocudu_assert(rg_mapper_factory, "Invalid resource grid mapper factory.");
  }

  std::unique_ptr<pdsch_processor> create() override
  {
    std::array<std::unique_ptr<ldpc_segmenter_tx>, pdsch_processor::MAX_NOF_TRANSPORT_BLOCKS> segmenters;
    std::generate(segmenters.begin(), segmenters.end(), [this]() { return segmenter_factory->create(); });
    return std::make_unique<pdsch_processor_flexible_impl>(std::move(segmenters),
                                                           rg_mapper_factory->create(),
                                                           block_pool_factory.create(),
                                                           dmrs_pool_factory.create(),
                                                           ptrs_pool_factory.create(),
                                                           executor,
                                                           cb_batch_length);
  }

  std::unique_ptr<pdsch_pdu_validator> create_validator() override
  {
    return std::make_unique<pdsch_processor_validator_impl>();
  }

  void on_new_processor_pool_scope() override
  {
    // The flexible processor wrappers inside one outer pool are meant to share
    // these helpers. A new downlink-processor instance needs its own pool.
    block_pool_factory.reset();
    dmrs_pool_factory.reset();
    ptrs_pool_factory.reset();
  }

private:
  std::shared_ptr<ldpc_segmenter_tx_factory>    segmenter_factory;
  std::shared_ptr<resource_grid_mapper_factory> rg_mapper_factory;
  task_executor&                                executor;
  pdsch_block_processor_pool_factory_sw         block_pool_factory;
  dmrs_generator_pool_factory_sw                dmrs_pool_factory;
  ptrs_generator_pool_factory_sw                ptrs_pool_factory;
  unsigned                                      cb_batch_length;
};

class pdsch_processor_asynchronous_pool_factory : public pdsch_processor_factory
{
public:
  pdsch_processor_asynchronous_pool_factory(std::shared_ptr<pdsch_processor_factory> factory_,
                                            unsigned                                 max_nof_processors_,
                                            bool                                     block_on_exhaustion_) :
    factory(std::move(factory_)), max_nof_processors(max_nof_processors_), block_on_exhaustion(block_on_exhaustion_)
  {
    ocudu_assert(factory, "Invalid PDSCH processor factory.");
    ocudu_assert(max_nof_processors >= 1,
                 "The number of processors (i.e., {}) must be greater than or equal to one.",
                 max_nof_processors);
  }

  std::unique_ptr<pdsch_processor> create() override
  {
    factory->on_new_processor_pool_scope();

    // Create processors.
    std::vector<std::unique_ptr<pdsch_processor_wrapper>> processors(max_nof_processors);
    for (std::unique_ptr<pdsch_processor_wrapper>& processor : processors) {
      processor = std::make_unique<pdsch_processor_wrapper>(factory->create());
    }

    return std::make_unique<pdsch_processor_pool>(processors, block_on_exhaustion);
  }

  std::unique_ptr<pdsch_processor> create(ocudulog::basic_logger& logger, bool enable_logging_broadcast) override
  {
    factory->on_new_processor_pool_scope();

    // Create processors.
    std::vector<std::unique_ptr<pdsch_processor_wrapper>> processors(max_nof_processors);
    for (std::unique_ptr<pdsch_processor_wrapper>& processor : processors) {
      processor = std::make_unique<pdsch_processor_wrapper>(factory->create(logger, enable_logging_broadcast));
    }

    return std::make_unique<pdsch_processor_pool>(processors, block_on_exhaustion);
  }

  std::unique_ptr<pdsch_pdu_validator> create_validator() override { return factory->create_validator(); }

private:
  std::shared_ptr<pdsch_processor_factory> factory;
  unsigned                                 max_nof_processors;
  bool                                     block_on_exhaustion;
};

} // namespace

std::shared_ptr<pdsch_encoder_factory>
ocudu::create_pdsch_encoder_factory_sw(pdsch_encoder_factory_sw_configuration& config)
{
  return std::make_shared<pdsch_encoder_factory_sw>(config);
}

std::shared_ptr<pdsch_encoder_factory>
ocudu::create_pdsch_encoder_factory_hw(const pdsch_encoder_factory_hw_configuration& config)
{
  return std::make_shared<pdsch_encoder_factory_hw>(config);
}

std::shared_ptr<pdsch_modulator_factory>
ocudu::create_pdsch_modulator_factory_sw(std::shared_ptr<modulation_mapper_factory>       modulator_factory,
                                         std::shared_ptr<pseudo_random_generator_factory> prg_factory,
                                         std::shared_ptr<resource_grid_mapper_factory>    rg_mapper_factory)
{
  return std::make_shared<pdsch_modulator_factory_sw>(
      std::move(modulator_factory), std::move(prg_factory), std::move(rg_mapper_factory));
}

std::shared_ptr<pdsch_processor_factory>
ocudu::create_pdsch_processor_factory_sw(std::shared_ptr<pdsch_encoder_factory>        encoder_factory,
                                         std::shared_ptr<pdsch_modulator_factory>      modulator_factory,
                                         std::shared_ptr<dmrs_pdsch_processor_factory> dmrs_factory,
                                         std::shared_ptr<ptrs_pdsch_generator_factory> ptrs_factory)
{
  return std::make_shared<pdsch_processor_factory_sw>(
      std::move(encoder_factory), std::move(modulator_factory), std::move(dmrs_factory), std::move(ptrs_factory));
}

std::shared_ptr<pdsch_block_processor_factory>
ocudu::create_pdsch_block_processor_factory_sw(std::shared_ptr<ldpc_encoder_factory>            encoder_factory,
                                               std::shared_ptr<ldpc_rate_matcher_factory>       rate_matcher_factory,
                                               std::shared_ptr<pseudo_random_generator_factory> prg_factory,
                                               std::shared_ptr<modulation_mapper_factory>       modulator_factory)
{
  return std::make_shared<pdsch_block_processor_factory_sw>(std::move(encoder_factory),
                                                            std::move(rate_matcher_factory),
                                                            std::move(prg_factory),
                                                            std::move(modulator_factory));
}

std::shared_ptr<pdsch_block_processor_factory>
ocudu::create_pdsch_block_processor_factory_hw(std::shared_ptr<hal::hw_accelerator_pdsch_enc_factory> encoder_factory,
                                               std::shared_ptr<pseudo_random_generator_factory>       prg_factory,
                                               std::shared_ptr<modulation_mapper_factory>             modulator_factory)
{
  return std::make_shared<pdsch_block_processor_factory_hw>(
      std::move(encoder_factory), std::move(prg_factory), std::move(modulator_factory));
}

std::shared_ptr<pdsch_processor_factory> ocudu::create_pdsch_flexible_processor_factory_sw(
    std::shared_ptr<ldpc_segmenter_tx_factory>     ldpc_segmenter_factory,
    std::shared_ptr<pdsch_block_processor_factory> block_processor_factory,
    std::shared_ptr<resource_grid_mapper_factory>  rg_mapper_factory,
    std::shared_ptr<dmrs_pdsch_processor_factory>  dmrs_factory,
    std::shared_ptr<ptrs_pdsch_generator_factory>  ptrs_factory,
    task_executor&                                 executor,
    unsigned                                       nof_concurrent_threads,
    unsigned                                       cb_batch_length)
{
  return std::make_shared<pdsch_processor_flexible_factory_sw>(std::move(ldpc_segmenter_factory),
                                                               std::move(block_processor_factory),
                                                               std::move(rg_mapper_factory),
                                                               std::move(dmrs_factory),
                                                               std::move(ptrs_factory),
                                                               executor,
                                                               nof_concurrent_threads,
                                                               cb_batch_length);
}

std::shared_ptr<pdsch_processor_factory>
ocudu::create_pdsch_processor_pool(std::shared_ptr<pdsch_processor_factory> pdsch_proc_factory,
                                   unsigned                                 max_nof_processors,
                                   bool                                     block_on_exhaustion)
{
  return std::make_shared<pdsch_processor_asynchronous_pool_factory>(
      std::move(pdsch_proc_factory), max_nof_processors, block_on_exhaustion);
}

std::unique_ptr<pdsch_processor> pdsch_processor_factory::create(ocudulog::basic_logger& logger,
                                                                 bool                    enable_logging_broadcast)
{
  return std::make_unique<logging_pdsch_processor_decorator>(logger, enable_logging_broadcast, create());
}

// ============================================================================
// Accelerated PDSCH block processor factory.
// ============================================================================

#ifdef ENABLE_CUDA

namespace {

class pdsch_block_processor_factory_accelerated : public pdsch_block_processor_factory
{
public:
  explicit pdsch_block_processor_factory_accelerated(int cuda_device_id_) : nvcuda_device_id(cuda_device_id_) {}

  std::unique_ptr<pdsch_block_processor> create() override
  {
    auto tb_encoder = create_pdsch_tb_encoder_cuda();
    if (!tb_encoder || !tb_encoder->is_gpu_available()) {
      return nullptr;
    }
    return std::make_unique<pdsch_block_processor_gpu_impl>(std::move(tb_encoder), nvcuda_device_id);
  }

private:
  int nvcuda_device_id;
};

} // namespace

std::shared_ptr<pdsch_block_processor_factory>
ocudu::create_pdsch_block_processor_factory_accelerated(int nvcuda_device_id)
{
  if (!is_pdsch_tb_encoder_gpu_available()) {
    return nullptr;
  }
  return std::make_shared<pdsch_block_processor_factory_accelerated>(nvcuda_device_id);
}

bool ocudu::is_pdsch_block_processor_acceleration_available()
{
  return is_pdsch_tb_encoder_gpu_available();
}

#else // ENABLE_CUDA

std::shared_ptr<pdsch_block_processor_factory>
ocudu::create_pdsch_block_processor_factory_accelerated(int /*nvcuda_device_id*/)
{
  return nullptr;
}

bool ocudu::is_pdsch_block_processor_acceleration_available()
{
  return false;
}

#endif // ENABLE_CUDA

namespace {

/// Dispatches small transport blocks to SW (low latency) and large ones to GPU.
class pdsch_block_processor_hybrid_impl final : public pdsch_block_processor
{
public:
  pdsch_block_processor_hybrid_impl(std::unique_ptr<pdsch_block_processor> accelerated_,
                                    std::unique_ptr<pdsch_block_processor> sw_,
                                    unsigned                               min_gpu_tb_bytes_) :
    accelerated(std::move(accelerated_)), sw(std::move(sw_)), min_gpu_tb_bytes(min_gpu_tb_bytes_)
  {
    ocudu_assert(accelerated, "Invalid accelerated PDSCH block processor.");
    ocudu_assert(sw, "Invalid SW PDSCH block processor.");
  }

  resource_grid_mapper::symbol_buffer& configure_new_transmission(span<const uint8_t>          data,
                                                                  unsigned                     i_cw,
                                                                  const configuration&         config,
                                                                  const ldpc_segmenter_buffer& segment_buffer,
                                                                  unsigned                     start_i_cb,
                                                                  unsigned                     cb_batch_len) override
  {
    // Host for small/medium TBs (RAR + typical OTA data grants): GPU launch often >500 us and
    // collapses DL scheduling. min_gpu_tb_bytes==0 forces GPU for all sizes (debug/A-B).
    const bool             use_sw = (min_gpu_tb_bytes > 0) && (data.size() < min_gpu_tb_bytes);
    pdsch_block_processor& chosen = use_sw ? *sw : *accelerated;
    return chosen.configure_new_transmission(data, i_cw, config, segment_buffer, start_i_cb, cb_batch_len);
  }

private:
  std::unique_ptr<pdsch_block_processor> accelerated;
  std::unique_ptr<pdsch_block_processor> sw;
  unsigned                               min_gpu_tb_bytes;
};

class pdsch_block_processor_factory_hybrid final : public pdsch_block_processor_factory
{
public:
  pdsch_block_processor_factory_hybrid(std::shared_ptr<pdsch_block_processor_factory> accelerated_factory_,
                                       std::shared_ptr<pdsch_block_processor_factory> sw_factory_,
                                       unsigned                                       min_gpu_tb_bytes_) :
    accelerated_factory(std::move(accelerated_factory_)),
    sw_factory(std::move(sw_factory_)),
    min_gpu_tb_bytes(min_gpu_tb_bytes_)
  {
    ocudu_assert(accelerated_factory, "Invalid accelerated factory.");
    ocudu_assert(sw_factory, "Invalid SW factory.");
  }

  std::unique_ptr<pdsch_block_processor> create() override
  {
    auto accelerated = accelerated_factory->create();
    auto sw          = sw_factory->create();
    if (!accelerated || !sw) {
      return nullptr;
    }
    return std::make_unique<pdsch_block_processor_hybrid_impl>(std::move(accelerated), std::move(sw), min_gpu_tb_bytes);
  }

private:
  std::shared_ptr<pdsch_block_processor_factory> accelerated_factory;
  std::shared_ptr<pdsch_block_processor_factory> sw_factory;
  unsigned                                       min_gpu_tb_bytes;
};

unsigned get_pdsch_gpu_min_tb_bytes(unsigned default_bytes)
{
  const char* value = std::getenv("OCUDU_PDSCH_GPU_MIN_TB_BYTES");
  if (value == nullptr || value[0] == '\0') {
    return default_bytes;
  }
  char*         end    = nullptr;
  unsigned long parsed = std::strtoul(value, &end, 10);
  if (end == value || *end != '\0' || parsed > static_cast<unsigned long>(std::numeric_limits<unsigned>::max())) {
    return default_bytes;
  }
  return static_cast<unsigned>(parsed);
}

} // namespace

std::shared_ptr<pdsch_block_processor_factory>
ocudu::create_pdsch_block_processor_factory_hybrid(std::shared_ptr<pdsch_block_processor_factory> accelerated_factory,
                                                   std::shared_ptr<pdsch_block_processor_factory> sw_factory,
                                                   unsigned                                       min_gpu_tb_bytes)
{
  if (!accelerated_factory || !sw_factory) {
    return nullptr;
  }
  const unsigned threshold = get_pdsch_gpu_min_tb_bytes(min_gpu_tb_bytes);
  return std::make_shared<pdsch_block_processor_factory_hybrid>(
      std::move(accelerated_factory), std::move(sw_factory), threshold);
}
