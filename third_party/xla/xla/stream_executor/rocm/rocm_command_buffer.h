/* Copyright 2024 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifndef XLA_STREAM_EXECUTOR_ROCM_ROCM_COMMAND_BUFFER_H_
#define XLA_STREAM_EXECUTOR_ROCM_ROCM_COMMAND_BUFFER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "rocm/include/hip/hip_runtime.h"
#include "xla/stream_executor/command_buffer.h"
#include "xla/stream_executor/device_memory.h"
#include "xla/stream_executor/gpu/gpu_command_buffer.h"
#include "xla/stream_executor/gpu/gpu_executor.h"
#include "xla/stream_executor/kernel.h"
#include "xla/stream_executor/launch_dim.h"

namespace stream_executor::gpu {

// Implements GpuCommandBuffer for AMD GPUs.
class RocmCommandBuffer : public GpuCommandBuffer {
 public:
  // Creates a new ROCm command buffer and the underlying HIP graph.
  static absl::StatusOr<std::unique_ptr<RocmCommandBuffer>> Create(
      Mode mode, GpuExecutor* parent);

 private:
  RocmCommandBuffer(Mode mode, GpuExecutor* parent, hipGraph_t graph,
                    bool is_owned_graph)
      : GpuCommandBuffer(mode, parent, graph, is_owned_graph),
        parent_(parent) {}

  absl::StatusOr<SetIfConditionKernel*> GetSetIfConditionKernel() override;
  absl::StatusOr<SetIfElseConditionKernel*> GetSetIfElseConditionKernel()
      override;
  absl::StatusOr<SetCaseConditionKernel*> GetSetCaseConditionKernel() override;
  absl::StatusOr<SetForConditionKernel*> GetSetForConditionKernel() override;
  absl::StatusOr<SetWhileConditionKernel*> GetSetWhileConditionKernel()
      override;
  absl::StatusOr<NoOpKernel*> GetNoOpKernel() override;

  std::unique_ptr<GpuCommandBuffer> CreateNestedCommandBuffer(
      hipGraph_t graph) override;

  absl::StatusOr<GraphNodeHandle> CreateMemsetNode(
      const Dependencies& dependencies, DeviceMemoryBase destination,
      BitPattern bit_pattern, size_t num_elements) override;

  absl::Status UpdateMemsetNode(GraphNodeHandle node_handle,
                                DeviceMemoryBase destination,
                                BitPattern bit_pattern,
                                size_t num_elements) override;

  absl::StatusOr<GraphNodeHandle> CreateMemcpyD2DNode(
      const Dependencies& dependencies, DeviceMemoryBase destination,
      DeviceMemoryBase source, uint64_t size) override;

  absl::Status UpdateMemcpyD2DNode(GraphNodeHandle node_handle,
                                   DeviceMemoryBase destination,
                                   DeviceMemoryBase source,
                                   uint64_t size) override;

  absl::StatusOr<GraphNodeHandle> CreateChildNode(
      const Dependencies& dependencies, const CommandBuffer& nested) override;

  absl::Status UpdateChildNode(GraphNodeHandle node_handle,
                               const CommandBuffer& nested) override;

  absl::StatusOr<GraphNodeHandle> CreateKernelNode(
      const Dependencies& dependencies, const ThreadDim& threads,
      const BlockDim& blocks, const Kernel& kernel,
      const KernelArgsPackedArrayBase& args) override;

  absl::Status UpdateKernelNode(GraphNodeHandle node_handle,
                                const ThreadDim& threads,
                                const BlockDim& blocks, const Kernel& kernel,
                                const KernelArgsPackedArrayBase& args) override;

  GpuExecutor* parent_;
};

}  // namespace stream_executor::gpu

#endif  // XLA_STREAM_EXECUTOR_ROCM_ROCM_COMMAND_BUFFER_H_
