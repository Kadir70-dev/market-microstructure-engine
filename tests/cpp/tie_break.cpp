#include <algorithm>
#include <array>
#include <iostream>

#include "feed/recorder.hpp"

int main() {
    std::array<core::FixedEvent, 4> events{};
    events[0].header = {4,0,0,100,0,9,0,0,0,2,0,0};
    events[1].header = {3,0,0,99,0,7,0,0,0,4,0,0};
    events[2].header = {2,0,0,100,0,8,0,0,0,1,0,0};
    events[3].header = {1,0,0,100,0,3,0,0,0,1,0,0};
    std::sort(events.begin(), events.end(), feed::EventTotalOrder{});
    if (events[0].header.seq_global != 3 || events[1].header.seq_global != 1 ||
        events[2].header.seq_global != 2 || events[3].header.seq_global != 4) return 1;
    std::cout << "phase2c_total_order=pass\n";
    return 0;
}
