// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "pusch_demodulator_impl.h"
#include "ocudu/phy/generic_functions/transform_precoding/transform_precoding_factories.h"
#include "ocudu/phy/upper/channel_modulation/channel_modulation_factories.h"
#include "ocudu/phy/upper/channel_processors/pusch/factories.h"
#include "ocudu/phy/upper/sequence_generators/sequence_generator_factories.h"
#include "ocudu/support/ocudu_assert.h"

#ifdef ENABLE_CUDA
#include "pusch_demodulator_gpu_impl.h"
#include "pusch_demodulator_layer_select.h"
#include <atomic>
#include <cstdio>
#include <cuda_runtime.h>
#endif

using namespace ocudu;

namespace {

class pusch_demodulator_factory_generic : public pusch_demodulator_factory
{
public:
  pusch_demodulator_factory_generic(std::shared_ptr<channel_equalizer_factory>       equalizer_factory_,
                                    std::shared_ptr<transform_precoder_factory>      precoder_factory_,
                                    std::shared_ptr<demodulation_mapper_factory>     demodulation_factory_,
                                    std::shared_ptr<evm_calculator_factory>          evm_calc_factory_,
                                    std::shared_ptr<pseudo_random_generator_factory> prg_factory_,
                                    unsigned                                         max_nof_prb_,
                                    bool                                             enable_post_eq_sinr_) :
    equalizer_factory(std::move(equalizer_factory_)),
    precoder_factory(std::move(precoder_factory_)),
    demodulation_factory(std::move(demodulation_factory_)),
    evm_calc_factory(std::move(evm_calc_factory_)),
    prg_factory(std::move(prg_factory_)),
    max_nof_prb(max_nof_prb_),
    enable_post_eq_sinr(enable_post_eq_sinr_)
  {
    ocudu_assert(equalizer_factory, "Invalid equalizer factory.");
    ocudu_assert(precoder_factory, "Invalid transform precoder factory.");
    ocudu_assert(demodulation_factory, "Invalid demodulation factory.");
    ocudu_assert(prg_factory, "Invalid PRG factory.");
  }

  std::unique_ptr<pusch_demodulator> create() override
  {
    std::unique_ptr<evm_calculator> evm_calc;
    if (evm_calc_factory) {
      evm_calc = evm_calc_factory->create();
    }
    return std::make_unique<pusch_demodulator_impl>(equalizer_factory->create(),
                                                    precoder_factory->create(),
                                                    demodulation_factory->create(),
                                                    std::move(evm_calc),
                                                    prg_factory->create(),
                                                    max_nof_prb,
                                                    enable_post_eq_sinr);
  }

private:
  std::shared_ptr<channel_equalizer_factory>       equalizer_factory;
  std::shared_ptr<transform_precoder_factory>      precoder_factory;
  std::shared_ptr<demodulation_mapper_factory>     demodulation_factory;
  std::shared_ptr<evm_calculator_factory>          evm_calc_factory;
  std::shared_ptr<pseudo_random_generator_factory> prg_factory;
  unsigned                                         max_nof_prb;
  bool                                             enable_post_eq_sinr;
};

} // namespace

std::shared_ptr<pusch_demodulator_factory>
ocudu::create_pusch_demodulator_factory_sw(std::shared_ptr<channel_equalizer_factory>       equalizer_factory,
                                           std::shared_ptr<transform_precoder_factory>      precoder_factory,
                                           std::shared_ptr<demodulation_mapper_factory>     demodulation_factory,
                                           std::shared_ptr<evm_calculator_factory>          evm_calc_factory,
                                           std::shared_ptr<pseudo_random_generator_factory> prg_factory,
                                           unsigned                                         max_nof_prb,
                                           bool                                             enable_post_eq_sinr)
{
  return std::make_shared<pusch_demodulator_factory_generic>(std::move(equalizer_factory),
                                                             std::move(precoder_factory),
                                                             std::move(demodulation_factory),
                                                             std::move(evm_calc_factory),
                                                             std::move(prg_factory),
                                                             max_nof_prb,
                                                             enable_post_eq_sinr);
}

// ============================================================================
// Accelerated PUSCH demodulator factory.
// ============================================================================

#ifdef ENABLE_CUDA

namespace {


class pusch_demodulator_factory_accelerated : public pusch_demodulator_factory
{
public:
  pusch_demodulator_factory_accelerated(const pusch_demodulator_factory_accelerated_configuration& config) :
    equalizer_factory(config.equalizer_factory),
    precoder_factory(config.precoder_factory),
    demodulation_factory(config.demodulation_factory),
    evm_calc_factory(config.evm_calc_factory),
    prg_factory(config.prg_factory),
    max_nof_prb(config.max_nof_prb),
    enable_post_eq_sinr(config.enable_post_eq_sinr),
    compensate_cfo(config.compensate_cfo),
    equalizer_algorithm(config.equalizer_algorithm),
    nvcuda_device_id(config.nvcuda_device_id)
  {
    ocudu_assert(equalizer_factory, "Invalid equalizer factory.");
    ocudu_assert(precoder_factory, "Invalid transform precoder factory.");
    ocudu_assert(demodulation_factory, "Invalid demodulation factory.");
    ocudu_assert(prg_factory, "Invalid PRG factory.");
  }

  std::unique_ptr<pusch_demodulator> create() override
  {
    std::unique_ptr<evm_calculator> accelerated_evm_calc;
    std::unique_ptr<evm_calculator> sw_evm_calc;
    if (evm_calc_factory) {
      accelerated_evm_calc = evm_calc_factory->create();
      sw_evm_calc          = evm_calc_factory->create();
    }

    auto accelerated_demodulator = std::make_unique<pusch_demodulator_gpu_impl>(equalizer_factory->create(),
                                                                                precoder_factory->create(),
                                                                                demodulation_factory->create(),
                                                                                std::move(accelerated_evm_calc),
                                                                                prg_factory->create(),
                                                                                max_nof_prb,
                                                                                enable_post_eq_sinr,
                                                                                compensate_cfo,
                                                                                equalizer_algorithm,
                                                                                nvcuda_device_id);
    auto sw_demodulator          = std::make_unique<pusch_demodulator_impl>(equalizer_factory->create(),
                                                                   precoder_factory->create(),
                                                                   demodulation_factory->create(),
                                                                   std::move(sw_evm_calc),
                                                                   prg_factory->create(),
                                                                   max_nof_prb,
                                                                   enable_post_eq_sinr);
    return std::make_unique<pusch_demodulator_layer_select>(std::move(accelerated_demodulator),
                                                            std::move(sw_demodulator));
  }

private:
  std::shared_ptr<channel_equalizer_factory>       equalizer_factory;
  std::shared_ptr<transform_precoder_factory>      precoder_factory;
  std::shared_ptr<demodulation_mapper_factory>     demodulation_factory;
  std::shared_ptr<evm_calculator_factory>          evm_calc_factory;
  std::shared_ptr<pseudo_random_generator_factory> prg_factory;
  unsigned                                         max_nof_prb;
  bool                                             enable_post_eq_sinr;
  bool                                             compensate_cfo;
  channel_equalizer_algorithm_type                 equalizer_algorithm;
  int                                              nvcuda_device_id;
};

} // namespace

std::shared_ptr<pusch_demodulator_factory>
ocudu::create_pusch_demodulator_factory_accelerated(const pusch_demodulator_factory_accelerated_configuration& config)
{
  return std::make_shared<pusch_demodulator_factory_accelerated>(config);
}

bool ocudu::is_pusch_demodulator_acceleration_available(int nvcuda_device_id)
{
  // Check CUDA availability at runtime.
  (void)cudaGetLastError();
  int         device_count = 0;
  cudaError_t err          = cudaGetDeviceCount(&device_count);
  if (err != cudaSuccess) {
    (void)cudaGetLastError();
    err = cudaGetDeviceCount(&device_count);
  }
  if (err == cudaSuccess) {
    // Reject a configured device index that does not exist.
    return (nvcuda_device_id >= 0) && (nvcuda_device_id < device_count);
  }

  // On some systems a previous CUDA runtime call can leave cudaGetDeviceCount()
  // reporting cudaErrorUnknown even though the primary device context remains
  // usable. Try binding the configured device before falling back to the CPU demodulator.
  (void)cudaGetLastError();
  err = cudaSetDevice(nvcuda_device_id);
  if (err == cudaSuccess) {
    (void)cudaGetLastError();
    return true;
  }
  (void)cudaGetLastError();
  return false;
}

#else // ENABLE_CUDA

std::shared_ptr<pusch_demodulator_factory> ocudu::create_pusch_demodulator_factory_accelerated(
    const pusch_demodulator_factory_accelerated_configuration& /*config*/)
{
  return nullptr;
}

bool ocudu::is_pusch_demodulator_acceleration_available(int /*nvcuda_device_id*/)
{
  return false;
}

#endif // ENABLE_CUDA
