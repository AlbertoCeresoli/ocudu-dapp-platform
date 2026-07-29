// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "phy/upper/phy_acceleration_prach_buffer_factory.h"
#include "ocudu/phy/support/support_factories.h"
#include "ocudu/phy/upper/channel_processors/prach/factories.h"
#include "ocudu/phy/upper/channel_processors/prach/formatters.h"
#include "ocudu/ran/prach/prach_preamble_information.h"
#include "ocudu/support/error_handling.h"
#include "ocudu/support/math/complex_normal_random.h"
#include "ocudu/support/math/math_utils.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <getopt.h>
#include <limits>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace ocudu;

// General test configuration parameters.
static uint64_t                    nof_repetitions   = 1000;
static uint64_t                    nof_warmups       = 0;
static std::set<unsigned>          set_nof_rx_ports  = {1, 2, 4};
static std::set<prach_format_type> set_format        = {prach_format_type::zero, prach_format_type::B4};
static std::set<unsigned>          set_zcz           = {0, 1, 14};
static std::set<unsigned>          set_nof_preambles = {4, 64};
static std::string                 detector_backend  = "cpu";
static bool                        use_device_buffer = false;
static std::string                 latency_csv_path;
static std::string                 sensitivity_csv_path;
static double                      snr_start_db           = -12.0;
static double                      snr_stop_db            = 8.0;
static double                      snr_step_db            = 2.0;
static unsigned                    nof_sensitivity_frames = 200;
static int                         target_preamble_index  = -1;

// Global pseudo-random generator.
static std::mt19937 rgen(0);

struct latency_percentiles {
  double p50   = 0.0;
  double p75   = 0.0;
  double p90   = 0.0;
  double p99   = 0.0;
  double p999  = 0.0;
  double p9999 = 0.0;
  double worst = 0.0;
};

struct detector_runtime {
  std::string                               backend;
  std::shared_ptr<prach_detector_factory>   factory;
  std::unique_ptr<prach_detector_validator> validator;
  std::unique_ptr<prach_detector>           detector;
};

static void usage(const char* prog)
{
  fmt::print("Usage: {} [-R repetitions]\n", prog);
  fmt::print("\t-R Repetitions [Default {}]\n", nof_repetitions);
  fmt::print("\t-W Warm-up repetitions per test case [Default {}]\n", nof_warmups);
  fmt::print("\t-p Fix the number of ports\n");
  fmt::print("\t-f Fix the PRACH format\n");
  fmt::print("\t-z Fix the Zero Correlation Zone configuration index.\n");
  fmt::print("\t-b Detector backend: cpu, gpu, auto, all [Default {}]\n", detector_backend);
  fmt::print("\t-d Use a CUDA-visible PRACH buffer when available [Default {}]\n", use_device_buffer);
  fmt::print("\t--latency-csv <path> Write latency percentiles to CSV.\n");
  fmt::print("\t--sensitivity-csv <path> Run an injected PRACH sensitivity sweep and write CSV.\n");
  fmt::print("\t--snr-start <dB> Sensitivity sweep start SNR [Default {}]\n", snr_start_db);
  fmt::print("\t--snr-stop <dB> Sensitivity sweep stop SNR [Default {}]\n", snr_stop_db);
  fmt::print("\t--snr-step <dB> Sensitivity sweep SNR step [Default {}]\n", snr_step_db);
  fmt::print("\t--frames <n> Sensitivity frames per SNR [Default {}]\n", nof_sensitivity_frames);
  fmt::print("\t--target-preamble <index> Injected preamble index; default is midpoint of monitored range.\n");
  fmt::print("\t-h Show this message\n");
}

static int parse_args(int argc, char** argv)
{
  enum long_option_id {
    latency_csv_id = 1000,
    sensitivity_csv_id,
    snr_start_id,
    snr_stop_id,
    snr_step_id,
    frames_id,
    target_preamble_id
  };

  static const option long_options[] = {{"latency-csv", required_argument, nullptr, latency_csv_id},
                                        {"sensitivity-csv", required_argument, nullptr, sensitivity_csv_id},
                                        {"snr-start", required_argument, nullptr, snr_start_id},
                                        {"snr-stop", required_argument, nullptr, snr_stop_id},
                                        {"snr-step", required_argument, nullptr, snr_step_id},
                                        {"frames", required_argument, nullptr, frames_id},
                                        {"target-preamble", required_argument, nullptr, target_preamble_id},
                                        {nullptr, 0, nullptr, 0}};

  int opt = 0;
  while ((opt = getopt_long(argc, argv, "R:W:p:f:z:b:dh", long_options, nullptr)) != -1) {
    switch (opt) {
      case 'R':
        nof_repetitions = std::strtol(optarg, nullptr, 10);
        break;
      case 'W':
        nof_warmups = std::strtol(optarg, nullptr, 10);
        break;
      case 'p':
        set_nof_rx_ports = {static_cast<unsigned>(std::strtol(optarg, nullptr, 10))};
        break;
      case 'f':
        set_format = {to_prach_format_type(optarg)};
        break;
      case 'z':
        set_zcz = {static_cast<unsigned>(std::strtol(optarg, nullptr, 10))};
        break;
      case 'b':
        detector_backend = optarg;
        if ((detector_backend != "cpu") && (detector_backend != "gpu") && (detector_backend != "auto") &&
            (detector_backend != "all")) {
          fmt::print("Invalid backend '{}'. Valid backends are: cpu, gpu, auto, all.\n", detector_backend);
          std::exit(-1);
        }
        break;
      case 'd':
        use_device_buffer = true;
        break;
      case latency_csv_id:
        latency_csv_path = optarg;
        break;
      case sensitivity_csv_id:
        sensitivity_csv_path = optarg;
        break;
      case snr_start_id:
        snr_start_db = std::strtod(optarg, nullptr);
        break;
      case snr_stop_id:
        snr_stop_db = std::strtod(optarg, nullptr);
        break;
      case snr_step_id:
        snr_step_db = std::strtod(optarg, nullptr);
        break;
      case frames_id:
        nof_sensitivity_frames = static_cast<unsigned>(std::strtoul(optarg, nullptr, 10));
        break;
      case target_preamble_id:
        target_preamble_index = static_cast<int>(std::strtol(optarg, nullptr, 10));
        break;
      case 'h':
      default:
        usage(argv[0]);
        std::exit(0);
    }
  }

  return 0;
}

static std::shared_ptr<prach_detector_factory> create_detector_factory(const std::string& backend)
{
  std::shared_ptr<dft_processor_factory> dft_proc_factory = create_dft_processor_factory_fftw_fast();
  report_fatal_error_if_not(dft_proc_factory, "Failed to create DFT processor factory.");

  std::shared_ptr<prach_generator_factory> prach_gen_factory = create_prach_generator_factory_sw();
  report_fatal_error_if_not(prach_gen_factory, "Failed to create PRACH generator factory.");

  std::shared_ptr<prach_detector_factory> factory;
  if (backend == "cpu") {
    factory = create_prach_detector_factory_sw(dft_proc_factory, prach_gen_factory);
  } else if ((backend == "gpu") || (backend == "auto")) {
    factory = create_prach_detector_factory_accelerated(
        dft_proc_factory, prach_gen_factory, {}, backend == "gpu" ? "enabled" : "auto");
  } else {
    report_fatal_error("Unsupported PRACH detector backend '{}'.", backend);
  }
  report_fatal_error_if_not(factory, "Failed to create PRACH detector factory.");

  return factory;
}

static std::vector<std::string> get_requested_backends()
{
  if (detector_backend == "all") {
    return {"cpu", "gpu"};
  }
  return {detector_backend};
}

static detector_runtime create_runtime(const std::string& backend)
{
  detector_runtime runtime;
  runtime.backend   = backend;
  runtime.factory   = create_detector_factory(backend);
  runtime.validator = runtime.factory->create_validator();
  runtime.detector  = runtime.factory->create();
  report_fatal_error_if_not(runtime.validator, "Failed to create PRACH validator.");
  report_fatal_error_if_not(runtime.detector, "Failed to create PRACH detector.");
  return runtime;
}

// Create buffer.
static std::unique_ptr<prach_buffer>
create_buffer(prach_format_type format, unsigned nof_antennas, bool enable_device_buffer, bool fill_noise = true)
{
  complex_normal_distribution<cf_t> complex_float_dist(cf_t(1, 1), std::sqrt(2.0F));

  std::unique_ptr<prach_buffer> buffer;
  if (is_long_preamble(format)) {
    buffer = create_phy_acceleration_prach_buffer_long(nof_antennas, 1, enable_device_buffer);
  } else {
    buffer = create_phy_acceleration_prach_buffer_short(nof_antennas, 1, 1, enable_device_buffer);
  }
  report_fatal_error_if_not(buffer, "Failed to create buffer.");

  if (!fill_noise) {
    return buffer;
  }

  for (unsigned i_antenna = 0; i_antenna != nof_antennas; ++i_antenna) {
    for (unsigned i_symbol = 0, i_symbol_end = buffer->get_max_nof_symbols(); i_symbol != i_symbol_end; ++i_symbol) {
      span<cbf16_t> prach_symbol = buffer->get_symbol(i_antenna, 0, 0, i_symbol);
      for (cbf16_t& sample : prach_symbol) {
        sample = complex_float_dist(rgen);
      }
    }
  }

  return buffer;
}

std::vector<prach_detector::configuration> generate_test_cases()
{
  std::vector<prach_detector::configuration> test_cases;
  for (unsigned nof_rx_ports : set_nof_rx_ports) {
    for (prach_format_type format : set_format) {
      std::uniform_int_distribution<unsigned> root_sequence_index_dist(0, (is_long_preamble(format) ? 837 : 138));

      for (unsigned zcz : set_zcz) {
        if (((format == prach_format_type::zero) && (zcz == 14)) || ((format == prach_format_type::B4) && (zcz == 1))) {
          continue;
        }

        for (unsigned nof_preambles : set_nof_preambles) {
          std::uniform_int_distribution<unsigned> start_preamble_index_dist(0, 64 - nof_preambles);

          // Create new test case.
          test_cases.emplace_back();

          // Select test case.
          prach_detector::configuration& config = test_cases.back();

          // Select RA subcarrier spacing. Set to 30kHz for all short preambles.
          prach_subcarrier_spacing ra_scs = prach_subcarrier_spacing::kHz30;
          if (is_long_preamble(format)) {
            if (format == prach_format_type::three) {
              ra_scs = prach_subcarrier_spacing::kHz5;
            } else {
              ra_scs = prach_subcarrier_spacing::kHz1_25;
            }
          }

          // Fill test case.
          config.root_sequence_index   = root_sequence_index_dist(rgen);
          config.format                = format;
          config.restricted_set        = restricted_set_config::UNRESTRICTED;
          config.zero_correlation_zone = zcz;
          config.start_preamble_index  = start_preamble_index_dist(rgen);
          config.nof_preamble_indices  = nof_preambles;
          config.ra_scs                = ra_scs;
          config.nof_rx_ports          = nof_rx_ports;
        }
      }
    }
  }

  return test_cases;
}

static double get_percentile(const std::vector<double>& sorted_values, double percentile)
{
  if (sorted_values.empty()) {
    return 0.0;
  }
  const double rank  = percentile / 100.0 * static_cast<double>(sorted_values.size());
  const size_t index = std::min(sorted_values.size() - 1, static_cast<size_t>(std::max(1.0, std::ceil(rank))) - 1U);
  return sorted_values[index];
}

static latency_percentiles compute_percentiles(std::vector<double> values)
{
  std::sort(values.begin(), values.end());
  latency_percentiles stats;
  stats.p50   = get_percentile(values, 50.0);
  stats.p75   = get_percentile(values, 75.0);
  stats.p90   = get_percentile(values, 90.0);
  stats.p99   = get_percentile(values, 99.0);
  stats.p999  = get_percentile(values, 99.9);
  stats.p9999 = get_percentile(values, 99.99);
  stats.worst = values.empty() ? 0.0 : values.back();
  return stats;
}

static latency_percentiles
measure_latency(prach_detector& detector, const prach_buffer& buffer, const prach_detector::configuration& config)
{
  for (uint64_t warmup = 0; warmup != nof_warmups; ++warmup) {
    (void)detector.detect(buffer, config);
  }

  std::vector<double> samples_us;
  samples_us.reserve(nof_repetitions);
  unsigned detected_preambles = 0;
  for (uint64_t i = 0; i != nof_repetitions; ++i) {
    const auto start  = std::chrono::steady_clock::now();
    auto       result = detector.detect(buffer, config);
    const auto stop   = std::chrono::steady_clock::now();
    detected_preambles += result.preambles.size();
    samples_us.push_back(std::chrono::duration<double, std::micro>(stop - start).count());
  }

  // Keep the compiler from proving that detection results are unused in optimized builds.
  if (detected_preambles == std::numeric_limits<unsigned>::max()) {
    fmt::print("unreachable {}\n", detected_preambles);
  }
  return compute_percentiles(std::move(samples_us));
}

static void write_latency_csv_header(std::ofstream& csv)
{
  csv << "backend,device_buffer,format,ra_scs,zcz,nof_rx_ports,root_sequence_index,start_preamble,nof_preambles,"
         "repetitions,warmups,p50_us,p75_us,p90_us,p99_us,p999_us,p9999_us,worst_us\n";
}

static void write_latency_csv_row(std::ofstream&                       csv,
                                  const std::string&                   backend,
                                  bool                                 device_buffer,
                                  const prach_detector::configuration& config,
                                  const latency_percentiles&           stats)
{
  csv << backend << ',' << (device_buffer ? 1 : 0) << ',' << to_string(config.format) << ',' << to_string(config.ra_scs)
      << ',' << config.zero_correlation_zone << ',' << config.nof_rx_ports << ',' << config.root_sequence_index << ','
      << config.start_preamble_index << ',' << config.nof_preamble_indices << ',' << nof_repetitions << ','
      << nof_warmups << ',' << stats.p50 << ',' << stats.p75 << ',' << stats.p90 << ',' << stats.p99 << ','
      << stats.p999 << ',' << stats.p9999 << ',' << stats.worst << '\n';
}

static bool backend_uses_device_buffer(const std::string& backend)
{
  return use_device_buffer && (backend != "cpu");
}

static int run_latency_benchmark(const std::vector<prach_detector::configuration>& configurations)
{
  std::ofstream csv;
  if (!latency_csv_path.empty()) {
    csv.open(latency_csv_path);
    report_fatal_error_if_not(csv.is_open(), "Unable to open latency CSV file '{}'.", latency_csv_path);
    write_latency_csv_header(csv);
  }

  fmt::print("\"PRACH detector\" latency performance for {} repetitions. All values are in microseconds.\n",
             nof_repetitions);
  fmt::print(" {:<5} {:<6} {:<84}|{:>8}|{:>8}|{:>8}|{:>8}|{:>8}|{:>8}|{:>8}|\n",
             "B/E",
             "Dev",
             "Configuration",
             "50th",
             "75th",
             "90th",
             "99th",
             "99.9th",
             "99.99th",
             "Worst");

  for (const std::string& backend : get_requested_backends()) {
    detector_runtime runtime = create_runtime(backend);
    for (const prach_detector::configuration& config : configurations) {
      report_fatal_error_if_not(
          runtime.validator->is_valid(config), "Invalid PRACH detector configuration {}.", config);

      const bool                    device_buffer = backend_uses_device_buffer(backend);
      std::unique_ptr<prach_buffer> buffer        = create_buffer(config.format, config.nof_rx_ports, device_buffer);
      report_fatal_error_if_not(buffer, "Failed to create buffer.");

      latency_percentiles stats = measure_latency(*runtime.detector, *buffer, config);
      fmt::print(" {:<5} {:<6} {:<84}|{:>8.1f}|{:>8.1f}|{:>8.1f}|{:>8.1f}|{:>8.1f}|{:>8.1f}|{:>8.1f}|\n",
                 backend,
                 device_buffer ? "yes" : "no",
                 fmt::format("{}", config),
                 stats.p50,
                 stats.p75,
                 stats.p90,
                 stats.p99,
                 stats.p999,
                 stats.p9999,
                 stats.worst);
      if (csv.is_open()) {
        write_latency_csv_row(csv, backend, device_buffer, config, stats);
      }
    }
  }

  return 0;
}

static unsigned get_target_preamble(const prach_detector::configuration& config)
{
  const unsigned start = config.start_preamble_index;
  const unsigned end   = config.start_preamble_index + config.nof_preamble_indices;
  if ((target_preamble_index >= 0) && (static_cast<unsigned>(target_preamble_index) >= start) &&
      (static_cast<unsigned>(target_preamble_index) < end)) {
    return static_cast<unsigned>(target_preamble_index);
  }
  return start + config.nof_preamble_indices / 2;
}

static std::unique_ptr<prach_generator> create_preamble_generator()
{
  std::shared_ptr<prach_generator_factory> factory = create_prach_generator_factory_sw();
  report_fatal_error_if_not(factory, "Failed to create PRACH generator factory.");
  std::unique_ptr<prach_generator> generator = factory->create();
  report_fatal_error_if_not(generator, "Failed to create PRACH generator.");
  return generator;
}

static void fill_sensitivity_buffer(prach_buffer&                        buffer,
                                    prach_generator&                     generator,
                                    const prach_detector::configuration& config,
                                    unsigned                             target_preamble,
                                    double                               snr_db,
                                    std::mt19937&                        frame_rng)
{
  prach_preamble_information preamble_info = is_long_preamble(config.format)
                                                 ? get_prach_preamble_long_info(config.format)
                                                 : get_prach_preamble_short_info(config.format, config.ra_scs, false);

  prach_generator::configuration generator_config;
  generator_config.format                = config.format;
  generator_config.root_sequence_index   = config.root_sequence_index;
  generator_config.preamble_index        = target_preamble;
  generator_config.restricted_set        = config.restricted_set;
  generator_config.zero_correlation_zone = config.zero_correlation_zone;
  span<const cf_t> preamble              = generator.generate(generator_config);

  float preamble_avg_power = 0.0F;
  for (cf_t sample : preamble) {
    preamble_avg_power += std::norm(sample);
  }
  preamble_avg_power /= static_cast<float>(preamble.size());

  const float                       snr_linear = static_cast<float>(std::pow(10.0, snr_db / 10.0));
  const float                       amplitude  = std::sqrt(snr_linear / std::max(preamble_avg_power, 1.0e-12F));
  complex_normal_distribution<cf_t> noise_dist(cf_t(0, 0), 1.0F);

  for (unsigned i_port = 0; i_port != config.nof_rx_ports; ++i_port) {
    for (unsigned i_symbol = 0; i_symbol != preamble_info.nof_symbols; ++i_symbol) {
      span<cbf16_t> symbol = buffer.get_symbol(i_port, 0, 0, i_symbol);
      for (unsigned i_re = 0; i_re != preamble_info.sequence_length; ++i_re) {
        symbol[i_re] = noise_dist(frame_rng) + amplitude * preamble[i_re];
      }
    }
  }
}

static const prach_detection_result::preamble_indication* find_preamble(const prach_detection_result& result,
                                                                        unsigned                      preamble_index)
{
  for (const auto& preamble : result.preambles) {
    if (preamble.preamble_index == preamble_index) {
      return &preamble;
    }
  }
  return nullptr;
}

static void write_sensitivity_csv_header(std::ofstream& csv)
{
  csv << "backend,device_buffer,format,ra_scs,zcz,nof_rx_ports,root_sequence_index,start_preamble,nof_preambles,"
         "target_preamble,snr_db,frames,detections,misses,false_alarm_frames,detection_probability,"
         "false_alarm_probability,avg_detection_metric,avg_preamble_power_db,avg_rssi_db\n";
}

static int run_sensitivity_sweep(const std::vector<prach_detector::configuration>& configurations)
{
  report_fatal_error_if_not(!sensitivity_csv_path.empty(), "Sensitivity sweep requires --sensitivity-csv <path>.");
  report_fatal_error_if_not(snr_step_db > 0.0, "SNR step must be positive.");
  report_fatal_error_if_not(nof_sensitivity_frames > 0, "Number of sensitivity frames must be positive.");

  std::ofstream csv(sensitivity_csv_path);
  report_fatal_error_if_not(csv.is_open(), "Unable to open sensitivity CSV file '{}'.", sensitivity_csv_path);
  write_sensitivity_csv_header(csv);

  fmt::print("PRACH detector injected sensitivity sweep: SNR [{}, {}] step {} dB, frames_per_snr={}\n",
             snr_start_db,
             snr_stop_db,
             snr_step_db,
             nof_sensitivity_frames);

  for (const std::string& backend : get_requested_backends()) {
    detector_runtime                 runtime   = create_runtime(backend);
    std::unique_ptr<prach_generator> generator = create_preamble_generator();

    for (const prach_detector::configuration& config : configurations) {
      report_fatal_error_if_not(
          runtime.validator->is_valid(config), "Invalid PRACH detector configuration {}.", config);

      const bool     device_buffer = backend_uses_device_buffer(backend);
      const unsigned target        = get_target_preamble(config);
      const uint32_t base_seed = static_cast<uint32_t>(config.root_sequence_index * 131U + config.nof_rx_ports * 17U +
                                                       config.start_preamble_index * 7U + target);

      std::unique_ptr<prach_buffer> buffer = create_buffer(config.format, config.nof_rx_ports, device_buffer, false);
      report_fatal_error_if_not(buffer, "Failed to create buffer.");

      for (double snr_db = snr_start_db; snr_db <= snr_stop_db + 1.0e-6; snr_db += snr_step_db) {
        unsigned detections         = 0;
        unsigned false_alarm_frames = 0;
        double   metric_sum         = 0.0;
        double   power_sum_db       = 0.0;
        double   rssi_sum_db        = 0.0;

        for (unsigned frame = 0; frame != nof_sensitivity_frames; ++frame) {
          std::mt19937 frame_rng(base_seed + frame + static_cast<uint32_t>((snr_db - snr_start_db) * 1000.0));
          fill_sensitivity_buffer(*buffer, *generator, config, target, snr_db, frame_rng);

          prach_detection_result result = runtime.detector->detect(*buffer, config);
          rssi_sum_db += result.rssi_dB;

          const auto* target_result = find_preamble(result, target);
          if (target_result != nullptr) {
            ++detections;
            metric_sum += target_result->detection_metric;
            power_sum_db += target_result->preamble_power_dB;
          }
          bool false_alarm = false;
          for (const auto& detected : result.preambles) {
            false_alarm |= detected.preamble_index != target;
          }
          if (false_alarm) {
            ++false_alarm_frames;
          }
        }

        const unsigned misses = nof_sensitivity_frames - detections;
        const double   detection_probability =
            static_cast<double>(detections) / static_cast<double>(nof_sensitivity_frames);
        const double false_alarm_probability =
            static_cast<double>(false_alarm_frames) / static_cast<double>(nof_sensitivity_frames);
        const double avg_metric =
            (detections == 0) ? std::numeric_limits<double>::quiet_NaN() : metric_sum / static_cast<double>(detections);
        const double avg_power_db = (detections == 0) ? std::numeric_limits<double>::quiet_NaN()
                                                      : power_sum_db / static_cast<double>(detections);
        const double avg_rssi_db  = rssi_sum_db / static_cast<double>(nof_sensitivity_frames);

        csv << backend << ',' << (device_buffer ? 1 : 0) << ',' << to_string(config.format) << ','
            << to_string(config.ra_scs) << ',' << config.zero_correlation_zone << ',' << config.nof_rx_ports << ','
            << config.root_sequence_index << ',' << config.start_preamble_index << ',' << config.nof_preamble_indices
            << ',' << target << ',' << snr_db << ',' << nof_sensitivity_frames << ',' << detections << ',' << misses
            << ',' << false_alarm_frames << ',' << detection_probability << ',' << false_alarm_probability << ','
            << avg_metric << ',' << avg_power_db << ',' << avg_rssi_db << '\n';

        fmt::print(" {:<5} dev={} {} target={} snr={:>5.1f} dB detect={:>5.1f}% false={:>5.1f}%\n",
                   backend,
                   device_buffer ? "yes" : "no",
                   fmt::format("{}", config),
                   target,
                   snr_db,
                   100.0 * detection_probability,
                   100.0 * false_alarm_probability);
      }
    }
  }

  return 0;
}

int main(int argc, char** argv)
{
  int ret = parse_args(argc, argv);
  if (ret < 0) {
    return ret;
  }

  // Generate the test cases.
  std::vector<prach_detector::configuration> configurations = generate_test_cases();

  if (!sensitivity_csv_path.empty()) {
    return run_sensitivity_sweep(configurations);
  }
  return run_latency_benchmark(configurations);
}
