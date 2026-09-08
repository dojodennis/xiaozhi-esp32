#include "service_schedule_wire.h"
#include "service_schedule_fixture.h"

#include <cassert>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>

using namespace orbit::service_schedule;

// Host fixture policy only, not a production Unicode/IANA validator. This callback
// deliberately rejects a valid-UTF8 format-control label to exercise fail-closed policy.
bool LabelPolicy(std::string_view label) {
    const std::string_view nbsp = "\xc2\xa0";
    return label.find("\xc2\xad") == std::string_view::npos &&
           label.substr(0, nbsp.size()) != nbsp &&
           (label.size() < nbsp.size() || label.substr(label.size() - nbsp.size()) != nbsp);
}
bool ZonePolicy(std::string_view zone) { return zone == "Europe/Brussels" || zone == "UTC"; }
bool Reject(std::string_view) { return false; }

std::string Fingerprint(const Snapshot& s) {
    std::ostringstream out;
    out << s.version << s.scope.assignment_id << s.scope.device_id << s.service_occurrence_id
        << s.service_revision << s.snapshot_revision << s.service_at_ms << s.timezone
        << s.server_now_ms;
    for (const auto& cue : s.cues)
        out << cue.id << cue.revision << cue.label << static_cast<int>(cue.kind) << cue.deadline_ms
            << cue.offset_ms;
    out << s.cues.size();
    for (const auto& timer : s.timers)
        out << timer.id << timer.revision << timer.label << timer.deadline_ms;
    out << s.timers.size();
    return out.str();
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const auto expected = Fixture0();
    wire::Context context{"00000000-0000-4000-8000-000000000004", expected.scope,
                          expected.service_occurrence_id};
    wire::Validators validators{LabelPolicy, ZonePolicy};
    const std::string mode = argv[1];
    if (mode == "missing_label")
        validators.label = nullptr;
    if (mode == "missing_zone")
        validators.timezone = nullptr;
    if (mode == "reject_label")
        validators.label = Reject;
    if (mode == "reject_zone")
        validators.timezone = Reject;
    if (mode == "wrong_session")
        context.websocket_session_id = "00000000-0000-4000-8000-000000000099";
    if (mode == "wrong_assignment")
        context.enrolled_scope.assignment_id = "00000000-0000-4000-8000-000000000099";
    if (mode == "wrong_device")
        context.enrolled_scope.device_id = "00000000-0000-4000-8000-000000000099";
    if (mode == "wrong_occurrence")
        context.service_occurrence_id = "00000000-0000-4000-8000-000000000099";
    if (mode == "empty_context")
        context = {};

    const std::string bytes((std::istreambuf_iterator<char>(std::cin)),
                            std::istreambuf_iterator<char>());
    auto output = Fixture0();
    output.snapshot_revision = 777;
    const auto before = Fingerprint(output);
    const auto result = wire::Decode(bytes, context, validators, output);
    if (result == wire::Result::Accepted) {
        // Synthetic host delivery with explicit fixture freshness. No network proof.
        Scheduler scheduler(context.enrolled_scope);
        assert(scheduler.Apply(output, 0) == ApplyResult::Applied);
        std::cout << "accepted " << output.snapshot_revision << ' ' << output.cues.size() << ' '
                  << output.timers.size() << ' ' << output.service_at_ms << ' '
                  << (output.cues.empty() ? 0 : output.cues[0].deadline_ms) << ' '
                  << (output.timers.empty() ? 0 : output.timers[0].deadline_ms) << '\n';
    } else {
        assert(Fingerprint(output) == before);
        switch (result) {
            case wire::Result::Malformed:
                std::cout << "malformed\n";
                break;
            case wire::Result::MissingValidators:
                std::cout << "missing_validators\n";
                break;
            case wire::Result::SessionMismatch:
                std::cout << "session_mismatch\n";
                break;
            case wire::Result::ScopeMismatch:
                std::cout << "scope_mismatch\n";
                break;
            case wire::Result::OccurrenceMismatch:
                std::cout << "occurrence_mismatch\n";
                break;
            case wire::Result::Accepted:
                assert(false);
                break;
        }
    }
}
