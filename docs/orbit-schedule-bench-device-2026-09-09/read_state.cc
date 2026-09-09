#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include "service_schedule_storage.h"

int main(int argc, char** argv) {
    using namespace orbit::service_schedule;
    if (argc != 2) return 1;
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) return 2;
    storage::Bytes bytes((std::istreambuf_iterator<char>(input)), {});
    Scope scope{"00000000-0000-4000-8000-000000000385",
                "00000000-0000-4000-8000-000000000386"};
    FacePersistentState saved;
    if (storage::Decode(bytes.data(), bytes.size(), scope, saved) != storage::CodecResult::Accepted)
        return 3;
    FaceModel restored(scope);
    if (!restored.RestoreState(saved)) return 4;
    const std::set<std::string> expected{"Rice", "Sauce", "Bread", "Stock", "Pasta", "Fish"};
    if (saved.schedule.snapshot.timers.size() != 6 || saved.schedule.items.size() != 6)
        return 5;
    for (const auto& timer : saved.schedule.snapshot.timers)
        if (!expected.count(timer.label)) return 6;
    std::cout << "{\"actual_frozen_decoder_accepted\":true,\"bytes\":" << bytes.size()
              << ",\"restored_clock_awaiting_fresh_time\":"
              << (restored.scheduler().clock_state() == ClockState::AwaitingFreshTime ? "true" : "false")
              << ",\"due_unacknowledged\":" << restored.due().size()
              << ",\"pending_count\":" << saved.pending.size() << ",\"timers\":[";
    bool comma = false;
    for (const auto& timer : saved.schedule.snapshot.timers) {
        const ItemState* state = nullptr;
        for (const auto& item : saved.schedule.items)
            if (item.key.id == timer.id) state = &item;
        if (!state) return 7;
        if (comma) std::cout << ',';
        comma = true;
        std::cout << "{\"name\":\"" << timer.label << "\",\"id\":\"" << timer.id
                  << "\",\"revision\":" << timer.revision << ",\"due\":"
                  << (state->due ? "true" : "false") << ",\"acknowledged\":"
                  << (state->acknowledged ? "true" : "false") << '}';
    }
    std::cout << "],\"pending_ids\":[";
    comma = false;
    for (const auto& key : saved.pending) {
        if (comma) std::cout << ',';
        comma = true;
        std::cout << '"' << key.id << '"';
    }
    std::cout << "]}\n";
}
