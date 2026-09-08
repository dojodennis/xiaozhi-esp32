#include "sdkconfig.h"
#if CONFIG_PROVISIONS_SCHEDULE_BENCH_DEMO
#include "service_schedule_demo.h"
#include "service_schedule_demo_fixture.h"
#include "service_schedule_wire.h"

#include <algorithm>
#include <cstdio>

namespace orbit::service_schedule {
namespace {
// This policy admits only the unchanged synthetic fixture. It is not an IANA
// database, Unicode implementation, enrollment or trusted-time provider.
bool FixtureLabel(std::string_view label) {
    return label == "Rice" || label == "Sauce" || label == "Bread" || label == "Setup" ||
           label == "Fixed reminder";
}
bool FixtureZone(std::string_view zone) { return zone == "Europe/Brussels"; }
}  // namespace

ScheduleDemo::ScheduleDemo() : model_({demo_fixture::kAssignment, demo_fixture::kDevice}) {
    valid_ = ApplyFixture(0, 0);
    model_.SetConnected(valid_);
    if (valid_)
        stage_ = "17:59 / dinner 19:00, setup 18:00";
}

bool ScheduleDemo::ApplyFixture(int index, int64_t monotonic_ms) {
    Snapshot decoded;
    const wire::Context context{demo_fixture::kSession,
                                {demo_fixture::kAssignment, demo_fixture::kDevice},
                                demo_fixture::kOccurrence};
    if (wire::Decode(demo_fixture::kFrames[index], context, {FixtureLabel, FixtureZone}, decoded) !=
        wire::Result::Accepted)
        return false;
    // Freshness is synthetic and explicit in this disconnected fixture harness.
    monotonic_ms_ = std::max(monotonic_ms_, monotonic_ms);
    return model_.ApplyVerified(decoded, monotonic_ms_) == ApplyResult::Applied;
}

void ScheduleDemo::MoveTo(int64_t checkpoint_ms) {
    monotonic_ms_ = std::max(monotonic_ms_, checkpoint_ms);
    model_.Tick(monotonic_ms_);
}

void ScheduleDemo::Elapse(int64_t delta_ms) {
    if (!valid_ || delta_ms < 0 || delta_ms > kMaximumEpochMs - monotonic_ms_)
        return;
    monotonic_ms_ += delta_ms;
    model_.Tick(monotonic_ms_);
}

bool ScheduleDemo::Advance() {
    if (!valid_)
        return false;
    switch (step_++) {
        case 0:
            valid_ = ApplyFixture(1, 30'000);
            stage_ = "17:59:30 / dinner 19:30, setup 18:30; cooking unchanged";
            break;
        case 1:
            MoveTo(31'000);
            model_.SetConnected(false);
            stage_ = "Disconnected / cached timers continue";
            break;
        case 2:
            MoveTo(60'000);
            stage_ = "18:00 / fixed reminder due; blue acknowledges one alert";
            break;
        case 3:
            MoveTo(120'000);
            stage_ = "18:01 / Rice due offline";
            break;
        case 4:
            MoveTo(180'000);
            stage_ = "18:02 / Sauce due offline";
            break;
        case 5:
            MoveTo(240'000);
            stage_ = "18:03 / Bread due offline";
            break;
        case 6:
            model_.SetConnected(true);
            stage_ = "Reconnected / local acknowledgements still pending";
            break;
        case 7:
            if (!model_.pending().empty()) {
                const auto key = model_.pending().front();
                model_.AcceptReceipt(key);
            }
            stage_ = "One matching MOCK receipt; remaining acknowledgements stay pending";
            break;
        case 8: {
            Snapshot old;
            const wire::Context context{demo_fixture::kSession,
                                        {demo_fixture::kAssignment, demo_fixture::kDevice},
                                        demo_fixture::kOccurrence};
            valid_ = wire::Decode(demo_fixture::kFrames[0], context, {FixtureLabel, FixtureZone},
                                  old) == wire::Result::Accepted &&
                     model_.ApplyVerified(old, std::max(monotonic_ms_, int64_t{241'000})) ==
                         ApplyResult::StaleRevision;
            stage_ = "Old snapshot rejected / no reset, replay or duplicate alert";
            break;
        }
        case 9:
            MoveTo(1'860'000);
            stage_ = "18:30 / linked setup now due";
            break;
        case 10:
            model_.Tick(100);
            stage_ = "Clock rollback / CHECK TIME; no fabricated countdown";
            break;
        default:
            --step_;
            stage_ = "Demo complete / restart local runner to repeat";
            return false;
    }
    return valid_;
}

std::string ScheduleDemo::FixtureTime(int64_t epoch_ms) {
    // Fixture date is 2026-09-08, Europe/Brussels UTC+02. Refuse other dates.
    if (epoch_ms < 1788818400000LL || epoch_ms >= 1788904800000LL)
        return "--:--";
    const int minutes = static_cast<int>(((epoch_ms / 60'000) + 120) % (24 * 60));
    char result[6];
    std::snprintf(result, sizeof(result), "%02d:%02d", minutes / 60, minutes % 60);
    return result;
}
}  // namespace orbit::service_schedule
#endif
