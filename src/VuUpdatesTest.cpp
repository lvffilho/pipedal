// Copyright (c) Robin E. R. Davies
// MIT license; see LICENSE in the repository root.

#include "pch.h"
#include "catch.hpp"
#include "VuUpdate.hpp"
#include "json.hpp"
#include <sstream>
#include <thread>
#include <vector>

using namespace pipedal;

static VuUpdateX MakeUpdate(int64_t id)
{
    VuUpdateX v;
    v.instanceId_ = id;
    v.outputMaxValueL_ = 0.5f;
    return v;
}

TEST_CASE("VU batch contains only subscribed meters", "[vu_updates]")
{
    std::vector<VuUpdateX> updates{MakeUpdate(1), MakeUpdate(2), MakeUpdate(3)};
    auto batch = FilterVuUpdates(updates, [](int64_t id) { return id != 2; });
    REQUIRE(batch.size() == 2);
    REQUIRE(batch[0].instanceId_ == 1);
    REQUIRE(batch[1].instanceId_ == 3);

    auto none = FilterVuUpdates(updates, [](int64_t) { return false; });
    REQUIRE(none.empty());
}

TEST_CASE("VU batch serializes as a single JSON array", "[vu_updates]")
{
    std::vector<VuUpdateX> updates{MakeUpdate(1), MakeUpdate(7)};
    std::stringstream s;
    json_writer writer(s, true);
    writer.write(updates);
    std::string json = s.str();
    REQUIRE(json.front() == '[');
    REQUIRE(json.find("\"instanceId\"") != std::string::npos);
    size_t count = 0, pos = 0;
    while ((pos = json.find("\"instanceId\"", pos)) != std::string::npos) { ++count; ++pos; }
    REQUIRE(count == 2);
}

TEST_CASE("VU flow control never sticks", "[vu_updates]")
{
    VuFlowControl fc;
    REQUIRE(fc.TryBeginSend());
    REQUIRE_FALSE(fc.TryBeginSend()); // throttled while outstanding
    fc.EndSend();
    REQUIRE(fc.TryBeginSend());
    fc.EndSend();
    fc.EndSend(); // spurious extra release must not go negative / stick
    REQUIRE_FALSE(fc.IsOutstanding());

    // concurrent begin/end from several threads always ends at zero.
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
    {
        threads.emplace_back([&fc]() {
            for (int i = 0; i < 10000; ++i)
            {
                if (fc.TryBeginSend())
                    fc.EndSend();
            }
        });
    }
    for (auto &t : threads) t.join();
    REQUIRE_FALSE(fc.IsOutstanding());
    REQUIRE(fc.TryBeginSend());
}
