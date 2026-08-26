#pragma once

#include <cstddef>

namespace rlife::llsss {

using IndexedTaskFunction = void (*)(void* context, std::size_t task_index, std::size_t worker_index);

// The callback is entered once per coarse task, not once per pair-tree state.
// A process-lifetime worker team amortizes dependent multi-phase operations
// such as slice-tree reification.
void execute_indexed_tasks(std::size_t task_count, int requested_workers, void* context, IndexedTaskFunction function);

} // namespace rlife::llsss
