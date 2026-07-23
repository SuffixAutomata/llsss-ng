#include "indexed_executor.hpp"

#include <omp.h>

#include <cstdint>
#include <stdexcept>

namespace rlife::llsss {

void execute_indexed_tasks(std::size_t task_count, int requested_workers,
                           void* context, IndexedTaskFunction function) {
    if (requested_workers <= 0) {
        throw std::invalid_argument("indexed executor needs at least one worker");
    }
    if (task_count == 0) return;

    omp_set_dynamic(0);
#pragma omp parallel num_threads(requested_workers)
    {
        const auto worker = static_cast<std::size_t>(omp_get_thread_num());
#pragma omp for schedule(dynamic, 1)
        for (std::uint64_t task = 0; task < task_count; ++task) {
            function(context, static_cast<std::size_t>(task), worker);
        }
    }
}

} // namespace rlife::llsss
