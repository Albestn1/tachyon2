#include "tachyon/c/math/elliptic_curves/msm/msm_gpu.h"

#include "absl/types/span.h"

#include "tachyon/base/console/console_stream.h"
#include "tachyon/base/environment.h"
#include "tachyon/base/files/file_util.h"
#include "tachyon/c/math/elliptic_curves/bn/bn254/g1_point_traits.h"
#include "tachyon/c/math/elliptic_curves/msm/msm_input_provider.h"
#include "tachyon/cc/math/elliptic_curves/point_conversions.h"
#include "tachyon/device/gpu/gpu_memory.h"
#include "tachyon/device/gpu/scoped_mem_pool.h"
#include "tachyon/math/elliptic_curves/bn/bn254/g1_gpu.h"
#include "tachyon/math/elliptic_curves/msm/variable_base_msm_gpu.h"

namespace tachyon {

using namespace math;
using namespace device;

namespace c::math {

namespace {

gpu::ScopedMemPool g_mem_pool;
gpu::ScopedStream g_stream;
gpu::GpuMemory<bn254::G1AffinePointGpu> g_d_bases;
gpu::GpuMemory<bn254::FrGpu> g_d_scalars;
std::unique_ptr<MSMInputProvider<bn254::G1AffinePoint>> g_provider;
std::unique_ptr<VariableBaseMSMGpu<bn254::G1AffinePointGpu::Curve>> g_msm;

// TODO(chokobole): Remove this when MSM gpu is stabilized.
std::string g_save_location;
bool g_log_msm = false;

size_t g_idx = 0;

void DoInitMSMGpu(uint8_t degree) {
  {
    // NOTE(chokobole): This should be replaced with VLOG().
    // Currently, there's no way to delegate VLOG flags from rust side.
    base::ConsoleStream cs;
    cs.Green();
    std::cout << "DoInitMSMGpu()" << std::endl;
  }
  GPU_MUST_SUCCESS(gpuDeviceReset(), "Failed to gpuDeviceReset()");

  std::string_view save_location_str;
  if (base::Environment::Get("TACHYON_SAVE_LOCATION", &save_location_str)) {
    g_save_location = std::string(save_location_str);
  }
  std::string_view log_msm_str;
  if (base::Environment::Get("TACHYON_LOG_MSM", &log_msm_str)) {
    if (log_msm_str == "1") g_log_msm = true;
  }

  bn254::G1AffinePointGpu::Curve::Init();

  gpuMemPoolProps props = {gpuMemAllocationTypePinned,
                           gpuMemHandleTypeNone,
                           {gpuMemLocationTypeDevice, 0}};
  g_mem_pool = gpu::CreateMemPool(&props);
  uint64_t mem_pool_threshold = std::numeric_limits<uint64_t>::max();
  GPU_MUST_SUCCESS(
      gpuMemPoolSetAttribute(g_mem_pool.get(), gpuMemPoolAttrReleaseThreshold,
                             &mem_pool_threshold),
      "Failed to gpuMemPoolSetAttribute()");

  uint64_t size = static_cast<uint64_t>(1) << degree;
  g_d_bases = gpu::GpuMemory<bn254::G1AffinePointGpu>::Malloc(size);
  g_d_scalars = gpu::GpuMemory<bn254::FrGpu>::Malloc(size);

  g_stream = gpu::CreateStream();
  g_provider.reset(new MSMInputProvider<bn254::G1AffinePoint>());
  g_provider->set_needs_align(true);
  g_msm.reset(new VariableBaseMSMGpu<bn254::G1AffinePointGpu::Curve>(
      tachyon::math::MSMAlgorithmKind::kBellmanMSM, g_mem_pool.get(),
      g_stream.get()));
}

void DoReleaseMSMGpu() {
  {
    // NOTE(chokobole): This should be replaced with VLOG().
    // Currently, there's no way to delegate VLOG flags from rust side.
    base::ConsoleStream cs;
    cs.Green();
    std::cout << "DoReleaseMSMGpu()" << std::endl;
  }
  g_d_bases.reset();
  g_d_scalars.reset();
  g_stream.reset();
  g_mem_pool.reset();
  g_provider.reset();
  g_msm.reset();
}

bn254::G1JacobianPoint DoMSMGpuInternal(
    absl::Span<const bn254::G1AffinePoint> bases,
    absl::Span<const bn254::Fr> scalars) {
  CHECK(g_d_bases.CopyFrom(bases.data(), gpu::GpuMemoryType::kHost, 0,
                           bases.size()));
  CHECK(g_d_scalars.CopyFrom(scalars.data(), gpu::GpuMemoryType::kHost, 0,
                             scalars.size()));

  bn254::G1JacobianPoint ret;
  CHECK(g_msm->Run(g_d_bases, g_d_scalars, bases.size(), &ret));
  if (g_log_msm) {
    // NOTE(chokobole): This should be replaced with VLOG().
    // Currently, there's no way to delegate VLOG flags from rust side.
    base::ConsoleStream cs;
    cs.Yellow();
    std::cout << "DoMSMGpuInternal()" << g_idx++ << std::endl;
    std::cout << ret.ToHexString() << std::endl;
  }
  if (!g_save_location.empty()) {
    {
      std::vector<std::string> results;
      for (const bn254::G1AffinePoint& base : bases) {
        results.push_back(base.ToMontgomery().ToString());
      }
      results.push_back("");
      base::WriteFile(base::FilePath(absl::Substitute(
                          "$0/bases$1.txt", g_save_location, g_idx - 1)),
                      absl::StrJoin(results, "\n"));
    }
    {
      std::vector<std::string> results;
      for (const bn254::Fr& scalar : scalars) {
        results.push_back(scalar.ToMontgomery().ToString());
      }
      results.push_back("");
      base::WriteFile(base::FilePath(absl::Substitute(
                          "$0/scalars$1.txt", g_save_location, g_idx - 1)),
                      absl::StrJoin(results, "\n"));
    }
  }
  return ret;
}

template <typename T>
tachyon_bn254_g1_jacobian* DoMSMGpu(const T* bases, size_t bases_len,
                                    const tachyon_bn254_fr* scalars,
                                    size_t scalars_len) {
  g_provider->Inject(bases, bases_len, scalars, scalars_len);
  tachyon_bn254_g1_jacobian* ret = new tachyon_bn254_g1_jacobian();
  cc::math::ToCPoint3(
      DoMSMGpuInternal(g_provider->bases(), g_provider->scalars()), ret);
  return ret;
}

}  // namespace

}  // namespace c::math
}  // namespace tachyon

void tachyon_init_msm_gpu(uint8_t degree) {
  tachyon::c::math::DoInitMSMGpu(degree);
}

void tachyon_release_msm_gpu() { tachyon::c::math::DoReleaseMSMGpu(); }

tachyon_bn254_g1_jacobian* tachyon_bn254_g1_point2_msm_gpu(
    const tachyon_bn254_g1_point2* bases, size_t bases_len,
    const tachyon_bn254_fr* scalars, size_t scalars_len) {
  return tachyon::c::math::DoMSMGpu(bases, bases_len, scalars, scalars_len);
}

tachyon_bn254_g1_jacobian* tachyon_bn254_g1_affine_msm_gpu(
    const tachyon_bn254_g1_affine* bases, size_t bases_len,
    const tachyon_bn254_fr* scalars, size_t scalars_len) {
  return tachyon::c::math::DoMSMGpu(bases, bases_len, scalars, scalars_len);
}
