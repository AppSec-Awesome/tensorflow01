/* Copyright 2023 The OpenXLA Authors.

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

#ifndef XLA_STREAM_EXECUTOR_GPU_GPU_COMMAND_BUFFER_H_
#define XLA_STREAM_EXECUTOR_GPU_GPU_COMMAND_BUFFER_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/inlined_vector.h"
#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "xla/stream_executor/bit_pattern.h"
#include "xla/stream_executor/command_buffer.h"
#include "xla/stream_executor/device_memory.h"
#include "xla/stream_executor/gpu/gpu_driver.h"
#include "xla/stream_executor/gpu/gpu_executor.h"
#include "xla/stream_executor/gpu/gpu_types.h"
#include "xla/stream_executor/kernel.h"
#include "xla/stream_executor/launch_dim.h"

namespace stream_executor::gpu {

// GpuCommandBuffer provides platform-specific CommandBuffer implementation
// (it's backed by CUDA or HIP graphs on NVIDIA and AMD devices).
class GpuCommandBuffer : public CommandBuffer {
  // GraphNodeHandleOpaque is an opaque type that won't be ODR used, hence
  // doesn't need to fully defined. It's an implementation detail of the
  // GraphNodeHandle defined below.
  struct GraphNodeHandleOpaque;

 public:
  // A graph node handle is an opaque handle that identifies a graph node in the
  // graph associated with a command buffer. GraphNodeHandles are created by
  // node factory functions and can be referenced in node update functions.
  // The handle has the same properties as a pointer (can be constructed from a
  // nullptr, trivial copyable, POD, etc.), that's why we use a pointer to
  // define it.
  using GraphNodeHandle = GraphNodeHandleOpaque*;

  // A handle to a Gpu graph node and a metadata describing its properties. Each
  // command (launch, memcpy, etc.) creates one or more graph nodes.
  struct GpuGraphNodeInfo {
    // A handle to the gpu graph node corresponding to a command.
    GraphNodeHandle handle{};
  };

  // A handle to Gpu graph barrier and metadata describing its properties. Each
  // call to `Barrier` creates a new barrier record.
  struct GpuGraphBarrierInfo {
    // A handle to graph node acting as a barrier that defines execution order.
    // It can be a handle to a `GpuGraphNodeInfo` node or a handle to an empty
    // node created to be a barrier. We try to reuse existing nodes as barriers
    // if possible to reduce the size of constructed gpu graphs.
    GraphNodeHandle handle{};

    // If `true` it means `handle` corresponds to an empty node specifically
    // created to act as an execution barrier, otherwise `handle` points to one
    // of the nodes created for recorded commands.
    bool is_barrier_node = true;

    // Nodes with index smaller than `nodes_offset` are synchronized with this
    // barrier. We use this offset to find nodes added after the last barrier
    // that should be added as dependencies to the next barrier.
    size_t nodes_offset = 0;
  };

  GpuCommandBuffer(Mode mode, GpuExecutor* parent, GpuGraphHandle graph,
                   bool is_owned_graph = true);
  ~GpuCommandBuffer() override;

  absl::Status Barrier(ExecutionScopeId execution_scope_id) override;

  absl::Status Barrier(
      absl::Span<const ExecutionScopeId> execution_scope_ids) override;

  absl::Status Barrier(ExecutionScopeId from_execution_scope_id,
                       ExecutionScopeId to_execution_scope_id) override;

  absl::Status Launch(ExecutionScopeId execution_scope_id,
                      const ThreadDim& threads, const BlockDim& blocks,
                      const Kernel& kernel, const KernelArgs& args) override;

  absl::Status AddNestedCommandBuffer(ExecutionScopeId execution_scope_id,
                                      const CommandBuffer& nested) override;

  absl::Status MemcpyDeviceToDevice(ExecutionScopeId execution_scope_id,
                                    DeviceMemoryBase* dst,
                                    const DeviceMemoryBase& src,
                                    uint64_t size) override;

  absl::Status Memset(ExecutionScopeId execution_scope_id,
                      DeviceMemoryBase* dst, BitPattern bit_pattern,
                      size_t num_elements) override;

  absl::Status If(ExecutionScopeId execution_scope_id,
                  DeviceMemory<bool> predicate, Builder then_builder) override;

  absl::Status IfElse(ExecutionScopeId execution_scope_id,
                      DeviceMemory<bool> predicate, Builder then_builder,
                      Builder else_builder) override;

  absl::Status Case(ExecutionScopeId execution_scope_id,
                    DeviceMemory<int32_t> index,
                    std::vector<Builder> branches) override;

  absl::Status For(ExecutionScopeId execution_scope_id, int32_t num_iteration,
                   DeviceMemory<int32_t> loop_counter,
                   Builder body_builder) override;

  absl::Status While(ExecutionScopeId execution_scope_id,
                     DeviceMemory<bool> pred,
                     ExecutionScopeBuilder cond_builder,
                     Builder body_builder) override;

  absl::Status Finalize() override;
  absl::Status Update() override;
  absl::Status Submit(Stream* stream) override;

  GpuGraphExecHandle executable() const { return exec_; }

  Mode mode() const override { return mode_; }
  State state() const override { return state_; }

  static GpuCommandBuffer* Cast(CommandBuffer* command_buffer) {
    return static_cast<GpuCommandBuffer*>(command_buffer);
  }

  static const GpuCommandBuffer* Cast(const CommandBuffer* command_buffer) {
    return static_cast<const GpuCommandBuffer*>(command_buffer);
  }

  absl::Span<const GpuGraphNodeInfo> nodes(ExecutionScopeId id) const;
  absl::Span<const GpuGraphBarrierInfo> barriers(ExecutionScopeId id) const;

  absl::Span<const GpuGraphNodeInfo> nodes() const {
    return nodes(kDefaulExecutionScope);
  }

  absl::Span<const GpuGraphBarrierInfo> barriers() const {
    return barriers(kDefaulExecutionScope);
  }

 private:
  absl::Status Trace(Stream* stream,
                     absl::AnyInvocable<absl::Status()> function) override;

  // We track the total number of allocated and alive executable graphs in the
  // process to track the command buffers resource usage. Executable graph
  // allocates resources on a GPU devices (rule of thumb is ~8kb per node), so
  // we have to be careful not to keep too many of them alive for too long, or
  // we have a higher risk of OOM errors.
  static int64_t AliveExecs();

 protected:
  using Dependencies = absl::InlinedVector<GraphNodeHandle, 1>;

  using NoOpKernel = TypedKernel<>;

  // A signature of a device kernels updating conditional handle(s).
  using SetIfConditionKernel =
      TypedKernel<GpuGraphConditionalHandle, DeviceMemory<bool>>;

  using SetIfElseConditionKernel =
      TypedKernel<GpuGraphConditionalHandle, GpuGraphConditionalHandle,
                  DeviceMemory<bool>>;

  using SetCaseConditionKernel =
      TypedKernel<GpuGraphConditionalHandle, GpuGraphConditionalHandle,
                  GpuGraphConditionalHandle, GpuGraphConditionalHandle,
                  GpuGraphConditionalHandle, GpuGraphConditionalHandle,
                  GpuGraphConditionalHandle, GpuGraphConditionalHandle,
                  DeviceMemory<int32_t>, int32_t, int32_t, bool>;

  using SetForConditionKernel =
      TypedKernel<GpuGraphConditionalHandle, DeviceMemory<int32_t>, int32_t>;

  using SetWhileConditionKernel =
      TypedKernel<GpuGraphConditionalHandle, DeviceMemory<bool>>;

 private:
  // A callback to launch a kernel that updates conditional handles state.
  using SetConditionFn = std::function<absl::Status(
      ExecutionScopeId, absl::Span<const GpuGraphConditionalHandle>)>;

  // An extension of `Builder` for building conditional command buffers tied to
  // conditional handles.
  using ConditionBuilder =
      std::function<absl::Status(CommandBuffer*, GpuGraphConditionalHandle)>;

  // Wraps a regular command buffer builder into condition builder.
  static ConditionBuilder ToConditionBuilder(Builder builder);

  using ConditionType = typename GpuDriver::GpuGraphConditionalNodeParams::Type;

  // Overwrites the `exec_` handle in a Gpu command buffer by `exec`, and
  // restores to the original handle when destroyed. This allows us updating
  // primary graph executable using nested command buffers (command buffers that
  // do not have their own executable), which is required for updating
  // conditional commands.
  struct ScopedGpuGraphExec {
    ScopedGpuGraphExec(GpuCommandBuffer* cmd_buffer, GpuGraphExecHandle exec);
    ~ScopedGpuGraphExec();

    GpuCommandBuffer* cmd_buffer;
    GpuGraphExecHandle restore;
    bool restore_is_owned;
  };

  // For each conditional node in the Gpu graph we keep a record of conditional
  // command buffers attached to a node, so we can apply updates to them.
  struct ConditionalCommandBuffers {
    std::vector<GpuGraphConditionalHandle> handles;
    std::vector<std::unique_ptr<GpuCommandBuffer>> command_buffers;
  };

  using AllocationResult = std::pair<GpuDevicePtr, uint64_t>;

  absl::StatusOr<std::vector<GpuGraphConditionalHandle>>
  CreateConditionalHandles(size_t num_handles);

  absl::StatusOr<std::vector<std::unique_ptr<GpuCommandBuffer>>>
  CreateConditionalCommandBuffers(
      absl::Span<const GpuGraphConditionalHandle> handles,
      absl::Span<const GpuGraphHandle> graphs,
      absl::Span<const ConditionBuilder> builders);

  absl::Status UpdateConditionalCommandBuffers(
      absl::Span<const GpuGraphConditionalHandle> handles,
      absl::Span<const std::unique_ptr<GpuCommandBuffer>> command_buffers,
      absl::Span<const ConditionBuilder> builders);

  absl::StatusOr<std::vector<GpuGraphHandle>> CreateConditionalNodes(
      ExecutionScopeId execution_scope_id, ConditionType type,
      absl::Span<const GpuGraphConditionalHandle> handles);

  absl::Status CreateConditionalCommand(
      ExecutionScopeId execution_scope_id, ConditionType type,
      SetConditionFn set_condition,
      absl::Span<const ConditionBuilder> builders);

  Dependencies GetBarrier(ExecutionScopeId execution_scope_id);

  // Returns loaded auxiliary kernels, or loads them on a given stream executor.
  // Loaded kernels owned by a current command buffer.
  virtual absl::StatusOr<SetIfConditionKernel*> GetSetIfConditionKernel() = 0;
  virtual absl::StatusOr<SetIfElseConditionKernel*>
  GetSetIfElseConditionKernel() = 0;
  virtual absl::StatusOr<SetCaseConditionKernel*>
  GetSetCaseConditionKernel() = 0;
  virtual absl::StatusOr<SetForConditionKernel*> GetSetForConditionKernel() = 0;
  virtual absl::StatusOr<SetWhileConditionKernel*>
  GetSetWhileConditionKernel() = 0;
  virtual absl::StatusOr<NoOpKernel*> GetNoOpKernel() = 0;

  // Recursively disable all nodes corresponding to barriers (including nested
  // conditional command buffers). This is work around the fact that we can't
  // use empty nodes inside conditional CUDA graphs and instead we add no-op
  // kernel nodes, however large number of no-op kernels impacts performance.
  absl::Status DisableBarriersExecution(GpuGraphExecHandle exec);

  // Launches CUDA kernels with packed arguments.
  absl::Status LaunchWithPackedArgs(
      ExecutionScopeId execution_scope_id, const ThreadDim& threads,
      const BlockDim& blocks, const Kernel& kernel,
      const KernelArgsPackedArrayBase& packed_args);

  // Returns OK status if command buffer is not finalized and it is still
  // possible to add new commands to it, otherwise returns internal error.
  absl::Status CheckNotFinalized();

  // Returns OK status if the number of command buffers is equal to the expected
  // one, otherwise returns internal error.
  absl::Status CheckNumCommandBuffers(
      const ConditionalCommandBuffers& cmd_buffers, size_t num_cmd_buffers);

  // Creates a new no-op node acting as a barrier.
  absl::StatusOr<GraphNodeHandle> CreateBarrierNode(
      const Dependencies& dependencies);

  // Collects a set of dependencies for a new barrier.
  Dependencies GetBarrierDependencies(ExecutionScopeId execution_scope_id);

  static_assert(std::is_pointer_v<GpuGraphHandle>,
                "GpuGraphHandle must be a pointer");
  static_assert(std::is_pointer_v<GpuGraphExecHandle>,
                "GpuGraphExecHandle must be a pointer");
  static_assert(std::is_pointer_v<GpuGraphNodeHandle>,
                "GpuGraphNodeHandle must be a pointer");

  Mode mode_;
  State state_ = State::kCreate;

  GpuExecutor* parent_;  // not owned, must outlive *this

  // TODO(hebecker): Move fields to subclasses once we have moved all GpuDriver
  // calls.
 protected:
  GpuGraphHandle graph_ = nullptr;  // owned if `is_owned_graph_`
  bool is_owned_graph_ = true;      // ownership of `graph_`

  GpuGraphExecHandle exec_ = nullptr;  // owned if `is_owned_graph_exec_`
  bool is_owned_graph_exec_ = true;    // ownership of `is_owned_graph_exec_`

 private:
  // ExecutionScope holds the state of an underlying CUDA graph (nodes an
  // barriers added to a graph) for a single execution scope.
  struct ExecutionScope {
    // Tracks indices into data structures during command buffer updates.
    struct UpdateState {
      // Index points to the graph node inside `nodes` that will be updated
      // next.
      int64_t node_idx = 0;

      // Index points to the barrier node inside `barriers` that will be updated
      // on a next call to `Barrier(...)`.
      int64_t barrier_idx = 0;

      // Index points to the conditional command buffers that will be updated
      // next when we'll be updating next conditional command (If, Case, While).
      int64_t conditional_idx = 0;
    };

    // Gpu graph nodes corresponding to recorded commands (launch, memcpy,
    // etc.).
    std::vector<GpuGraphNodeInfo> nodes;

    // Gpu graph barriers that define recorded commands execution order.
    std::vector<GpuGraphBarrierInfo> barriers;

    // Command buffers for conditional nodes in the Gpu graph. Underlying Gpu
    // graphs owned by the `graph_` instance.
    std::vector<ConditionalCommandBuffers> conditional_command_buffers;

    // Tracks execution scope update state.
    UpdateState update_state;
  };

  // Execution scopes recorded into the command buffer.
  absl::flat_hash_map<ExecutionScopeId, ExecutionScope> execution_scopes_;

  // Track the number of command buffer updates for debugging.
  int64_t num_updates_ = 0;

  // Creates a nested command buffer, associated with the same executor.
  // The given graph will not be owned by the created command buffer.
  virtual std::unique_ptr<GpuCommandBuffer> CreateNestedCommandBuffer(
      GpuGraphHandle graph) = 0;

  // Adds a new memset node to the graph.
  virtual absl::StatusOr<GraphNodeHandle> CreateMemsetNode(
      const Dependencies& dependencies, DeviceMemoryBase destination,
      BitPattern bit_pattern, size_t num_elements) = 0;

  // Updates an existing memset node. Note that `node_handle` needs to be refer
  // to a node created by `CreateMemsetNode`.
  virtual absl::Status UpdateMemsetNode(GraphNodeHandle node_handle,
                                        DeviceMemoryBase destination,
                                        BitPattern bit_pattern,
                                        size_t num_elements) = 0;

  // Adds a new memcpy node to the graph.
  virtual absl::StatusOr<GraphNodeHandle> CreateMemcpyD2DNode(
      const Dependencies& dependencies, DeviceMemoryBase destination,
      DeviceMemoryBase source, uint64_t size) = 0;

  virtual absl::Status UpdateMemcpyD2DNode(GraphNodeHandle node_handle,
                                           DeviceMemoryBase destination,
                                           DeviceMemoryBase source,
                                           uint64_t size) = 0;

  // Adds a new nested command buffer node to the graph.
  virtual absl::StatusOr<GraphNodeHandle> CreateChildNode(
      const Dependencies& dependencies, const CommandBuffer& nested) = 0;

  // Associate another command buffer with this child node. Will return an
  // error if the given node has not been created as a child node.
  virtual absl::Status UpdateChildNode(GraphNodeHandle node_handle,
                                       const CommandBuffer& nested) = 0;

  // Adds a new kernel launch node to the graph.
  virtual absl::StatusOr<GraphNodeHandle> CreateKernelNode(
      const Dependencies& dependencies, const ThreadDim& threads,
      const BlockDim& blocks, const Kernel& kernel,
      const KernelArgsPackedArrayBase& args) = 0;

  // Updates the kernel launch node with the given parameters. Will return an
  // error if the given node has not been created as a kernel launch node.
  virtual absl::Status UpdateKernelNode(
      GraphNodeHandle node_handle, const ThreadDim& threads,
      const BlockDim& blocks, const Kernel& kernel,
      const KernelArgsPackedArrayBase& args) = 0;
};

}  // namespace stream_executor::gpu

#endif  // XLA_STREAM_EXECUTOR_GPU_GPU_COMMAND_BUFFER_H_
