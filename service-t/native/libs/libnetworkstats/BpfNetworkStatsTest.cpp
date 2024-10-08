/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <inttypes.h>
#include <linux/inet_diag.h>
#include <linux/sock_diag.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <android-base/stringprintf.h>
#include <android-base/strings.h>

#define BPF_MAP_MAKE_VISIBLE_FOR_TESTING
#include "bpf/BpfMap.h"
#include "bpf/BpfUtils.h"
#include "netdbpf/BpfNetworkStats.h"

using ::testing::Test;

namespace android {
namespace bpf {

using base::Result;
using base::unique_fd;

constexpr int TEST_MAP_SIZE = 10;
constexpr uid_t TEST_UID1 = 10086;
constexpr uid_t TEST_UID2 = 12345;
constexpr uint32_t TEST_TAG = 42;
constexpr int TEST_COUNTERSET0 = 0;
constexpr int TEST_COUNTERSET1 = 1;
constexpr uint64_t TEST_BYTES0 = 1000;
constexpr uint64_t TEST_BYTES1 = 2000;
constexpr uint64_t TEST_PACKET0 = 100;
constexpr uint64_t TEST_PACKET1 = 200;
constexpr const char IFACE_NAME1[] = "lo";
constexpr const char IFACE_NAME2[] = "wlan0";
constexpr const char IFACE_NAME3[] = "rmnet_data0";
// A iface name that the size is bigger than IFNAMSIZ
constexpr const char LONG_IFACE_NAME[] = "wlanWithALongName";
constexpr const char TRUNCATED_IFACE_NAME[] = "wlanWithALongNa";
constexpr uint32_t IFACE_INDEX1 = 1;
constexpr uint32_t IFACE_INDEX2 = 2;
constexpr uint32_t IFACE_INDEX3 = 3;
constexpr uint32_t IFACE_INDEX4 = 4;
constexpr uint32_t UNKNOWN_IFACE = 0;

class BpfNetworkStatsHelperTest : public testing::Test {
  protected:
    BpfNetworkStatsHelperTest() {}
    BpfMap<uint64_t, UidTagValue> mFakeCookieTagMap;
    BpfMap<uint32_t, StatsValue> mFakeAppUidStatsMap;
    BpfMap<StatsKey, StatsValue> mFakeStatsMap;
    BpfMap<uint32_t, IfaceValue> mFakeIfaceIndexNameMap;
    BpfMap<uint32_t, StatsValue> mFakeIfaceStatsMap;

    IfIndexToNameFunc mIfIndex2Name = [this](const uint32_t ifindex){
        return mFakeIfaceIndexNameMap.readValue(ifindex);
    };

    void SetUp() {
        ASSERT_EQ(0, setrlimitForTest());

        mFakeCookieTagMap.resetMap(BPF_MAP_TYPE_HASH, TEST_MAP_SIZE);
        ASSERT_TRUE(mFakeCookieTagMap.isValid());

        mFakeAppUidStatsMap.resetMap(BPF_MAP_TYPE_HASH, TEST_MAP_SIZE);
        ASSERT_TRUE(mFakeAppUidStatsMap.isValid());

        mFakeStatsMap.resetMap(BPF_MAP_TYPE_HASH, TEST_MAP_SIZE);
        ASSERT_TRUE(mFakeStatsMap.isValid());

        mFakeIfaceIndexNameMap.resetMap(BPF_MAP_TYPE_HASH, TEST_MAP_SIZE);
        ASSERT_TRUE(mFakeIfaceIndexNameMap.isValid());

        mFakeIfaceStatsMap.resetMap(BPF_MAP_TYPE_HASH, TEST_MAP_SIZE);
        ASSERT_TRUE(mFakeIfaceStatsMap.isValid());
    }

    void expectUidTag(uint64_t cookie, uid_t uid, uint32_t tag) {
        auto tagResult = mFakeCookieTagMap.readValue(cookie);
        EXPECT_RESULT_OK(tagResult);
        EXPECT_EQ(uid, tagResult.value().uid);
        EXPECT_EQ(tag, tagResult.value().tag);
    }

    void populateFakeStats(uid_t uid, uint32_t tag, uint32_t ifaceIndex, uint32_t counterSet,
                           StatsValue value, BpfMap<StatsKey, StatsValue>& map) {
        StatsKey key = {
            .uid = (uint32_t)uid, .tag = tag, .counterSet = counterSet, .ifaceIndex = ifaceIndex};
        EXPECT_RESULT_OK(map.writeValue(key, value, BPF_ANY));
    }

    void updateIfaceMap(const char* ifaceName, uint32_t ifaceIndex) {
        IfaceValue iface;
        strlcpy(iface.name, ifaceName, IFNAMSIZ);
        EXPECT_RESULT_OK(mFakeIfaceIndexNameMap.writeValue(ifaceIndex, iface, BPF_ANY));
    }

    void expectStatsEqual(const StatsValue& target, const StatsValue& result) {
        EXPECT_EQ(target.rxPackets, result.rxPackets);
        EXPECT_EQ(target.rxBytes, result.rxBytes);
        EXPECT_EQ(target.txPackets, result.txPackets);
        EXPECT_EQ(target.txBytes, result.txBytes);
    }

    void expectStatsLineEqual(const StatsValue target, const char* iface, uint32_t uid,
                              int counterSet, uint32_t tag, const stats_line& result) {
        EXPECT_EQ(0, strcmp(iface, result.iface));
        EXPECT_EQ(uid, (uint32_t)result.uid);
        EXPECT_EQ((uint32_t) counterSet, result.set);
        EXPECT_EQ(tag, (uint32_t)result.tag);
        EXPECT_EQ(target.rxPackets, (uint64_t)result.rxPackets);
        EXPECT_EQ(target.rxBytes, (uint64_t)result.rxBytes);
        EXPECT_EQ(target.txPackets, (uint64_t)result.txPackets);
        EXPECT_EQ(target.txBytes, (uint64_t)result.txBytes);
    }
};

// TEST to verify the behavior of bpf map when cocurrent deletion happens when
// iterating the same map.
TEST_F(BpfNetworkStatsHelperTest, TestIterateMapWithDeletion) {
    for (int i = 0; i < 5; i++) {
        uint64_t cookie = i + 1;
        UidTagValue tag = {.uid = TEST_UID1, .tag = TEST_TAG};
        EXPECT_RESULT_OK(mFakeCookieTagMap.writeValue(cookie, tag, BPF_ANY));
    }
    uint64_t curCookie = 0;
    auto nextCookie = mFakeCookieTagMap.getNextKey(curCookie);
    EXPECT_RESULT_OK(nextCookie);
    uint64_t headOfMap = nextCookie.value();
    curCookie = nextCookie.value();
    // Find the second entry in the map, then immediately delete it.
    nextCookie = mFakeCookieTagMap.getNextKey(curCookie);
    EXPECT_RESULT_OK(nextCookie);
    EXPECT_RESULT_OK(mFakeCookieTagMap.deleteValue((nextCookie.value())));
    // Find the entry that is now immediately after headOfMap, then delete that.
    nextCookie = mFakeCookieTagMap.getNextKey(curCookie);
    EXPECT_RESULT_OK(nextCookie);
    EXPECT_RESULT_OK(mFakeCookieTagMap.deleteValue((nextCookie.value())));
    // Attempting to read an entry that has been deleted fails with ENOENT.
    curCookie = nextCookie.value();
    auto tagResult = mFakeCookieTagMap.readValue(curCookie);
    EXPECT_EQ(ENOENT, tagResult.error().code());
    // Finding the entry after our deleted entry restarts iteration from the beginning of the map.
    nextCookie = mFakeCookieTagMap.getNextKey(curCookie);
    EXPECT_RESULT_OK(nextCookie);
    EXPECT_EQ(headOfMap, nextCookie.value());
}

TEST_F(BpfNetworkStatsHelperTest, TestBpfIterateMap) {
    for (int i = 0; i < 5; i++) {
        uint64_t cookie = i + 1;
        UidTagValue tag = {.uid = TEST_UID1, .tag = TEST_TAG};
        EXPECT_RESULT_OK(mFakeCookieTagMap.writeValue(cookie, tag, BPF_ANY));
    }
    int totalCount = 0;
    int totalSum = 0;
    const auto iterateWithoutDeletion =
            [&totalCount, &totalSum](const uint64_t& key, const BpfMap<uint64_t, UidTagValue>&) {
                EXPECT_GE((uint64_t)5, key);
                totalCount++;
                totalSum += key;
                return Result<void>();
            };
    EXPECT_RESULT_OK(mFakeCookieTagMap.iterate(iterateWithoutDeletion));
    EXPECT_EQ(5, totalCount);
    EXPECT_EQ(1 + 2 + 3 + 4 + 5, totalSum);
}

TEST_F(BpfNetworkStatsHelperTest, TestUidStatsNoTraffic) {
    StatsValue value1 = {
            .rxPackets = 0,
            .rxBytes = 0,
            .txPackets = 0,
            .txBytes = 0,
    };
    StatsValue result1 = {};
    ASSERT_EQ(0, bpfGetUidStatsInternal(TEST_UID1, &result1, mFakeAppUidStatsMap));
    expectStatsEqual(value1, result1);
}

TEST_F(BpfNetworkStatsHelperTest, TestGetUidStatsTotal) {
    updateIfaceMap(IFACE_NAME1, IFACE_INDEX1);
    updateIfaceMap(IFACE_NAME2, IFACE_INDEX2);
    updateIfaceMap(IFACE_NAME3, IFACE_INDEX3);
    StatsValue value1 = {
            .rxPackets = TEST_PACKET0,
            .rxBytes = TEST_BYTES0,
            .txPackets = TEST_PACKET1,
            .txBytes = TEST_BYTES1,
    };
    StatsValue value2 = {
            .rxPackets = TEST_PACKET0 * 2,
            .rxBytes = TEST_BYTES0 * 2,
            .txPackets = TEST_PACKET1 * 2,
            .txBytes = TEST_BYTES1 * 2,
    };
    ASSERT_RESULT_OK(mFakeAppUidStatsMap.writeValue(TEST_UID1, value1, BPF_ANY));
    ASSERT_RESULT_OK(mFakeAppUidStatsMap.writeValue(TEST_UID2, value2, BPF_ANY));
    StatsValue result1 = {};
    ASSERT_EQ(0, bpfGetUidStatsInternal(TEST_UID1, &result1, mFakeAppUidStatsMap));
    expectStatsEqual(value1, result1);

    StatsValue result2 = {};
    ASSERT_EQ(0, bpfGetUidStatsInternal(TEST_UID2, &result2, mFakeAppUidStatsMap));
    expectStatsEqual(value2, result2);
    std::vector<stats_line> lines;
    populateFakeStats(TEST_UID1, 0, IFACE_INDEX1, TEST_COUNTERSET0, value1, mFakeStatsMap);
    populateFakeStats(TEST_UID1, 0, IFACE_INDEX2, TEST_COUNTERSET1, value1, mFakeStatsMap);
    populateFakeStats(TEST_UID2, 0, IFACE_INDEX3, TEST_COUNTERSET1, value1, mFakeStatsMap);
    ASSERT_EQ(0, parseBpfNetworkStatsDetailInternal(lines, mFakeStatsMap, mIfIndex2Name));
    ASSERT_EQ((unsigned long)3, lines.size());
}

TEST_F(BpfNetworkStatsHelperTest, TestGetIfaceStatsInternal) {
    updateIfaceMap(IFACE_NAME1, IFACE_INDEX1);
    updateIfaceMap(IFACE_NAME2, IFACE_INDEX2);
    updateIfaceMap(IFACE_NAME3, IFACE_INDEX3);
    StatsValue value1 = {
            .rxPackets = TEST_PACKET0,
            .rxBytes = TEST_BYTES0,
            .txPackets = TEST_PACKET1,
            .txBytes = TEST_BYTES1,
    };
    StatsValue value2 = {
            .rxPackets = TEST_PACKET1,
            .rxBytes = TEST_BYTES1,
            .txPackets = TEST_PACKET0,
            .txBytes = TEST_BYTES0,
    };
    uint32_t ifaceStatsKey = IFACE_INDEX1;
    EXPECT_RESULT_OK(mFakeIfaceStatsMap.writeValue(ifaceStatsKey, value1, BPF_ANY));
    ifaceStatsKey = IFACE_INDEX2;
    EXPECT_RESULT_OK(mFakeIfaceStatsMap.writeValue(ifaceStatsKey, value2, BPF_ANY));
    ifaceStatsKey = IFACE_INDEX3;
    EXPECT_RESULT_OK(mFakeIfaceStatsMap.writeValue(ifaceStatsKey, value1, BPF_ANY));

    StatsValue result1 = {};
    ASSERT_EQ(0,
              bpfGetIfaceStatsInternal(IFACE_NAME1, &result1, mFakeIfaceStatsMap, mIfIndex2Name));
    expectStatsEqual(value1, result1);
    StatsValue result2 = {};
    ASSERT_EQ(0,
              bpfGetIfaceStatsInternal(IFACE_NAME2, &result2, mFakeIfaceStatsMap, mIfIndex2Name));
    expectStatsEqual(value2, result2);
    StatsValue totalResult = {};
    ASSERT_EQ(0, bpfGetIfaceStatsInternal(NULL, &totalResult, mFakeIfaceStatsMap, mIfIndex2Name));
    StatsValue totalValue = {
            .rxPackets = TEST_PACKET0 * 2 + TEST_PACKET1,
            .rxBytes = TEST_BYTES0 * 2 + TEST_BYTES1,
            .txPackets = TEST_PACKET1 * 2 + TEST_PACKET0,
            .txBytes = TEST_BYTES1 * 2 + TEST_BYTES0,
    };
    expectStatsEqual(totalValue, totalResult);
}

TEST_F(BpfNetworkStatsHelperTest, TestGetIfIndexStatsInternal) {
    StatsValue value = {
          .rxPackets = TEST_PACKET0,
          .rxBytes = TEST_BYTES0,
          .txPackets = TEST_PACKET1,
          .txBytes = TEST_BYTES1,
    };
    EXPECT_RESULT_OK(mFakeIfaceStatsMap.writeValue(IFACE_INDEX1, value, BPF_ANY));

    StatsValue result = {};
    ASSERT_EQ(0, bpfGetIfIndexStatsInternal(IFACE_INDEX1, &result, mFakeIfaceStatsMap));
    expectStatsEqual(value, result);
}

TEST_F(BpfNetworkStatsHelperTest, TestGetStatsDetail) {
    updateIfaceMap(IFACE_NAME1, IFACE_INDEX1);
    updateIfaceMap(IFACE_NAME2, IFACE_INDEX2);
    StatsValue value1 = {
            .rxPackets = TEST_PACKET0,
            .rxBytes = TEST_BYTES0,
            .txPackets = TEST_PACKET1,
            .txBytes = TEST_BYTES1,
    };
    populateFakeStats(TEST_UID1, TEST_TAG, IFACE_INDEX1, TEST_COUNTERSET0, value1, mFakeStatsMap);
    populateFakeStats(TEST_UID1, TEST_TAG, IFACE_INDEX2, TEST_COUNTERSET0, value1, mFakeStatsMap);
    populateFakeStats(TEST_UID1, TEST_TAG + 1, IFACE_INDEX1, TEST_COUNTERSET0, value1,
                      mFakeStatsMap);
    populateFakeStats(TEST_UID2, TEST_TAG, IFACE_INDEX1, TEST_COUNTERSET0, value1, mFakeStatsMap);
    std::vector<stats_line> lines;
    ASSERT_EQ(0, parseBpfNetworkStatsDetailInternal(lines, mFakeStatsMap, mIfIndex2Name));
    ASSERT_EQ((unsigned long)7, lines.size());
}

TEST_F(BpfNetworkStatsHelperTest, TestGetStatsWithSkippedIface) {
    updateIfaceMap(IFACE_NAME1, IFACE_INDEX1);
    updateIfaceMap(IFACE_NAME2, IFACE_INDEX2);
    StatsValue value1 = {
            .rxPackets = TEST_PACKET0,
            .rxBytes = TEST_BYTES0,
            .txPackets = TEST_PACKET1,
            .txBytes = TEST_BYTES1,
    };
    // next stats entry will be ignored due to ifindex 0 not being present in mFakeIfaceIndexNameMap
    populateFakeStats(0, 0, 0, TEST_COUNTERSET1, value1, mFakeStatsMap);
    populateFakeStats(TEST_UID1, 0, IFACE_INDEX1, TEST_COUNTERSET0, value1, mFakeStatsMap);
    populateFakeStats(TEST_UID1, 0, IFACE_INDEX2, TEST_COUNTERSET0, value1, mFakeStatsMap);
    populateFakeStats(TEST_UID1, 0, IFACE_INDEX1, TEST_COUNTERSET1, value1, mFakeStatsMap);
    populateFakeStats(TEST_UID2, 0, IFACE_INDEX1, TEST_COUNTERSET0, value1, mFakeStatsMap);
    std::vector<stats_line> lines;
    ASSERT_EQ(0, parseBpfNetworkStatsDetailInternal(lines, mFakeStatsMap, mIfIndex2Name));
    ASSERT_EQ((unsigned long)4, lines.size());
}

TEST_F(BpfNetworkStatsHelperTest, TestUnknownIfaceError) {
    updateIfaceMap(IFACE_NAME1, IFACE_INDEX1);
    StatsValue value1 = {
            .rxPackets = TEST_PACKET0,
            .rxBytes = TEST_BYTES0 * 20,
            .txPackets = TEST_PACKET1,
            .txBytes = TEST_BYTES1 * 20,
    };
    uint32_t ifaceIndex = UNKNOWN_IFACE;
    populateFakeStats(TEST_UID1, 0, ifaceIndex, TEST_COUNTERSET0, value1, mFakeStatsMap);
    populateFakeStats(TEST_UID1, 0, IFACE_INDEX1, TEST_COUNTERSET0, value1, mFakeStatsMap);
    StatsValue value2 = {
            .rxPackets = TEST_PACKET0,
            .rxBytes = TEST_BYTES0 * 40,
            .txPackets = TEST_PACKET1,
            .txBytes = TEST_BYTES1 * 40,
    };
    populateFakeStats(TEST_UID1, 0, IFACE_INDEX2, TEST_COUNTERSET0, value2, mFakeStatsMap);
    StatsKey curKey = {
            .uid = TEST_UID1,
            .tag = 0,
            .counterSet = TEST_COUNTERSET0,
            .ifaceIndex = ifaceIndex,
    };
    int64_t unknownIfaceBytesTotal = 0;
    ASSERT_EQ(false, mFakeIfaceIndexNameMap.readValue(ifaceIndex).ok());
    maybeLogUnknownIface(ifaceIndex, mFakeStatsMap, curKey, &unknownIfaceBytesTotal);

    ASSERT_EQ(((int64_t)(TEST_BYTES0 * 20 + TEST_BYTES1 * 20)), unknownIfaceBytesTotal);
    curKey.ifaceIndex = IFACE_INDEX2;

    ASSERT_EQ(false, mFakeIfaceIndexNameMap.readValue(ifaceIndex).ok());
    maybeLogUnknownIface(ifaceIndex, mFakeStatsMap, curKey, &unknownIfaceBytesTotal);

    ASSERT_EQ(-1, unknownIfaceBytesTotal);
    std::vector<stats_line> lines;
    // TODO: find a way to test the total of unknown Iface Bytes go above limit.
    ASSERT_EQ(0, parseBpfNetworkStatsDetailInternal(lines, mFakeStatsMap, mIfIndex2Name));
    ASSERT_EQ((unsigned long)1, lines.size());
    expectStatsLineEqual(value1, IFACE_NAME1, TEST_UID1, TEST_COUNTERSET0, 0, lines.front());
}

TEST_F(BpfNetworkStatsHelperTest, TestGetIfaceStatsDetail) {
    updateIfaceMap(IFACE_NAME1, IFACE_INDEX1);
    updateIfaceMap(IFACE_NAME2, IFACE_INDEX2);
    updateIfaceMap(IFACE_NAME3, IFACE_INDEX3);
    updateIfaceMap(LONG_IFACE_NAME, IFACE_INDEX4);
    StatsValue value1 = {
            .rxPackets = TEST_PACKET0,
            .rxBytes = TEST_BYTES0,
            .txPackets = TEST_PACKET1,
            .txBytes = TEST_BYTES1,
    };
    StatsValue value2 = {
            .rxPackets = TEST_PACKET1,
            .rxBytes = TEST_BYTES1,
            .txPackets = TEST_PACKET0,
            .txBytes = TEST_BYTES0,
    };
    uint32_t ifaceStatsKey = IFACE_INDEX1;
    EXPECT_RESULT_OK(mFakeIfaceStatsMap.writeValue(ifaceStatsKey, value1, BPF_ANY));
    ifaceStatsKey = IFACE_INDEX2;
    EXPECT_RESULT_OK(mFakeIfaceStatsMap.writeValue(ifaceStatsKey, value2, BPF_ANY));
    ifaceStatsKey = IFACE_INDEX3;
    EXPECT_RESULT_OK(mFakeIfaceStatsMap.writeValue(ifaceStatsKey, value1, BPF_ANY));
    ifaceStatsKey = IFACE_INDEX4;
    EXPECT_RESULT_OK(mFakeIfaceStatsMap.writeValue(ifaceStatsKey, value2, BPF_ANY));
    std::vector<stats_line> lines;
    ASSERT_EQ(0, parseBpfNetworkStatsDevInternal(lines, mFakeIfaceStatsMap, mIfIndex2Name));
    ASSERT_EQ((unsigned long)4, lines.size());

    expectStatsLineEqual(value1, IFACE_NAME1, UID_ALL, SET_ALL, TAG_NONE, lines[0]);
    expectStatsLineEqual(value1, IFACE_NAME3, UID_ALL, SET_ALL, TAG_NONE, lines[1]);
    expectStatsLineEqual(value2, IFACE_NAME2, UID_ALL, SET_ALL, TAG_NONE, lines[2]);
    ASSERT_EQ(0, strcmp(TRUNCATED_IFACE_NAME, lines[3].iface));
    expectStatsLineEqual(value2, TRUNCATED_IFACE_NAME, UID_ALL, SET_ALL, TAG_NONE, lines[3]);
}

TEST_F(BpfNetworkStatsHelperTest, TestGetStatsSortedAndGrouped) {
    // Create iface indexes with duplicate iface name.
    updateIfaceMap(IFACE_NAME1, IFACE_INDEX1);
    updateIfaceMap(IFACE_NAME2, IFACE_INDEX2);
    updateIfaceMap(IFACE_NAME1, IFACE_INDEX3);  // Duplicate!

    StatsValue value1 = {
            .rxPackets = TEST_PACKET0,
            .rxBytes = TEST_BYTES0,
            .txPackets = TEST_PACKET1,
            .txBytes = TEST_BYTES1,
    };
    StatsValue value2 = {
            .rxPackets = TEST_PACKET1,
            .rxBytes = TEST_BYTES1,
            .txPackets = TEST_PACKET0,
            .txBytes = TEST_BYTES0,
    };
    StatsValue value3 = {  // value1 *2
            .rxPackets = TEST_PACKET0 * 2,
            .rxBytes = TEST_BYTES0 * 2,
            .txPackets = TEST_PACKET1 * 2,
            .txBytes = TEST_BYTES1 * 2,
    };
    StatsValue value5 = {  // value2 + value3
            .rxPackets = TEST_PACKET1 + TEST_PACKET0 * 2,
            .rxBytes = TEST_BYTES1 + TEST_BYTES0 * 2,
            .txPackets = TEST_PACKET0 + TEST_PACKET1 * 2,
            .txBytes = TEST_BYTES0 + TEST_BYTES1 * 2,
    };

    std::vector<stats_line> lines;

    // Test empty stats.
    ASSERT_EQ(0, parseBpfNetworkStatsDetailInternal(lines, mFakeStatsMap, mIfIndex2Name));
    ASSERT_EQ((size_t) 0, lines.size());
    lines.clear();

    // Test 1 line stats.
    populateFakeStats(TEST_UID1, TEST_TAG, IFACE_INDEX1, TEST_COUNTERSET0, value1, mFakeStatsMap);
    ASSERT_EQ(0, parseBpfNetworkStatsDetailInternal(lines, mFakeStatsMap, mIfIndex2Name));
    ASSERT_EQ((size_t) 2, lines.size());  // TEST_TAG != 0 -> 1 entry becomes 2 lines
    expectStatsLineEqual(value1, IFACE_NAME1, TEST_UID1, TEST_COUNTERSET0, 0, lines[0]);
    expectStatsLineEqual(value1, IFACE_NAME1, TEST_UID1, TEST_COUNTERSET0, TEST_TAG, lines[1]);
    lines.clear();

    // These items should not be grouped.
    populateFakeStats(TEST_UID1, TEST_TAG, IFACE_INDEX2, TEST_COUNTERSET0, value2, mFakeStatsMap);
    populateFakeStats(TEST_UID1, TEST_TAG, IFACE_INDEX3, TEST_COUNTERSET1, value2, mFakeStatsMap);
    populateFakeStats(TEST_UID1, TEST_TAG + 1, IFACE_INDEX1, TEST_COUNTERSET0, value2,
                      mFakeStatsMap);
    populateFakeStats(TEST_UID2, TEST_TAG, IFACE_INDEX1, TEST_COUNTERSET0, value1, mFakeStatsMap);
    ASSERT_EQ(0, parseBpfNetworkStatsDetailInternal(lines, mFakeStatsMap, mIfIndex2Name));
    ASSERT_EQ((size_t) 9, lines.size());
    lines.clear();

    // These items should be grouped.
    populateFakeStats(TEST_UID1, TEST_TAG, IFACE_INDEX3, TEST_COUNTERSET0, value1, mFakeStatsMap);
    populateFakeStats(TEST_UID2, TEST_TAG, IFACE_INDEX3, TEST_COUNTERSET0, value1, mFakeStatsMap);

    ASSERT_EQ(0, parseBpfNetworkStatsDetailInternal(lines, mFakeStatsMap, mIfIndex2Name));
    ASSERT_EQ((size_t) 9, lines.size());

    // Verify Sorted & Grouped.
    expectStatsLineEqual(value5, IFACE_NAME1, TEST_UID1, TEST_COUNTERSET0, 0,            lines[0]);
    expectStatsLineEqual(value2, IFACE_NAME1, TEST_UID1, TEST_COUNTERSET1, 0,            lines[1]);
    expectStatsLineEqual(value3, IFACE_NAME1, TEST_UID1, TEST_COUNTERSET0, TEST_TAG,     lines[2]);
    expectStatsLineEqual(value2, IFACE_NAME1, TEST_UID1, TEST_COUNTERSET1, TEST_TAG,     lines[3]);
    expectStatsLineEqual(value2, IFACE_NAME1, TEST_UID1, TEST_COUNTERSET0, TEST_TAG + 1, lines[4]);
    expectStatsLineEqual(value3, IFACE_NAME1, TEST_UID2, TEST_COUNTERSET0, 0,            lines[5]);
    expectStatsLineEqual(value3, IFACE_NAME1, TEST_UID2, TEST_COUNTERSET0, TEST_TAG,     lines[6]);
    expectStatsLineEqual(value2, IFACE_NAME2, TEST_UID1, TEST_COUNTERSET0, 0,            lines[7]);
    expectStatsLineEqual(value2, IFACE_NAME2, TEST_UID1, TEST_COUNTERSET0, TEST_TAG,     lines[8]);
    lines.clear();

    // Perform test on IfaceStats.
    uint32_t ifaceStatsKey = IFACE_INDEX2;
    EXPECT_RESULT_OK(mFakeIfaceStatsMap.writeValue(ifaceStatsKey, value2, BPF_ANY));
    ifaceStatsKey = IFACE_INDEX1;
    EXPECT_RESULT_OK(mFakeIfaceStatsMap.writeValue(ifaceStatsKey, value1, BPF_ANY));

    // This should be grouped.
    ifaceStatsKey = IFACE_INDEX3;
    EXPECT_RESULT_OK(mFakeIfaceStatsMap.writeValue(ifaceStatsKey, value1, BPF_ANY));

    ASSERT_EQ(0, parseBpfNetworkStatsDevInternal(lines, mFakeIfaceStatsMap, mIfIndex2Name));
    ASSERT_EQ((size_t) 2, lines.size());

    expectStatsLineEqual(value3, IFACE_NAME1, UID_ALL, SET_ALL, TAG_NONE, lines[0]);
    expectStatsLineEqual(value2, IFACE_NAME2, UID_ALL, SET_ALL, TAG_NONE, lines[1]);
    lines.clear();
}

// Test to verify that subtract overflow will not be triggered by the compare function invoked from
// sorting. See http:/b/119193941.
TEST_F(BpfNetworkStatsHelperTest, TestGetStatsSortAndOverflow) {
    updateIfaceMap(IFACE_NAME1, IFACE_INDEX1);

    StatsValue value1 = {
            .rxPackets = TEST_PACKET0,
            .rxBytes = TEST_BYTES0,
            .txPackets = TEST_PACKET1,
            .txBytes = TEST_BYTES1,
    };
    StatsValue value4 = {  // value1 * 4
            .rxPackets = TEST_PACKET0 * 4,
            .rxBytes = TEST_BYTES0 * 4,
            .txPackets = TEST_PACKET1 * 4,
            .txBytes = TEST_BYTES1 * 4,
    };

    // Mutate uid, 0 < TEST_UID1 < INT_MAX < INT_MIN < UINT_MAX.
    populateFakeStats(0,         TEST_TAG, IFACE_INDEX1, TEST_COUNTERSET0, value1, mFakeStatsMap);
    populateFakeStats(UINT_MAX,  TEST_TAG, IFACE_INDEX1, TEST_COUNTERSET0, value1, mFakeStatsMap);
    populateFakeStats(INT_MIN,   TEST_TAG, IFACE_INDEX1, TEST_COUNTERSET0, value1, mFakeStatsMap);
    populateFakeStats(INT_MAX,   TEST_TAG, IFACE_INDEX1, TEST_COUNTERSET0, value1, mFakeStatsMap);

    // Mutate tag, 0 < TEST_TAG < INT_MAX < INT_MIN < UINT_MAX.
    populateFakeStats(TEST_UID1, INT_MAX,  IFACE_INDEX1, TEST_COUNTERSET0, value1, mFakeStatsMap);
    populateFakeStats(TEST_UID1, INT_MIN,  IFACE_INDEX1, TEST_COUNTERSET0, value1, mFakeStatsMap);
    populateFakeStats(TEST_UID1, 0,        IFACE_INDEX1, TEST_COUNTERSET0, value1, mFakeStatsMap);
    populateFakeStats(TEST_UID1, UINT_MAX, IFACE_INDEX1, TEST_COUNTERSET0, value1, mFakeStatsMap);

    // TODO: Mutate counterSet and enlarge TEST_MAP_SIZE if overflow on counterSet is possible.

    std::vector<stats_line> lines;
    ASSERT_EQ(0, parseBpfNetworkStatsDetailInternal(lines, mFakeStatsMap, mIfIndex2Name));
    ASSERT_EQ((size_t) 12, lines.size());

    // Uid 0 first
    expectStatsLineEqual(value1, IFACE_NAME1, 0,         TEST_COUNTERSET0, 0,        lines[0]);
    expectStatsLineEqual(value1, IFACE_NAME1, 0,         TEST_COUNTERSET0, TEST_TAG, lines[1]);

    // Test uid, mutate tag.
    expectStatsLineEqual(value4, IFACE_NAME1, TEST_UID1, TEST_COUNTERSET0, 0,        lines[2]);
    expectStatsLineEqual(value1, IFACE_NAME1, TEST_UID1, TEST_COUNTERSET0, INT_MAX,  lines[3]);
    expectStatsLineEqual(value1, IFACE_NAME1, TEST_UID1, TEST_COUNTERSET0, INT_MIN,  lines[4]);
    expectStatsLineEqual(value1, IFACE_NAME1, TEST_UID1, TEST_COUNTERSET0, UINT_MAX, lines[5]);

    // Mutate uid.
    expectStatsLineEqual(value1, IFACE_NAME1, INT_MAX,   TEST_COUNTERSET0, 0,        lines[6]);
    expectStatsLineEqual(value1, IFACE_NAME1, INT_MAX,   TEST_COUNTERSET0, TEST_TAG, lines[7]);
    expectStatsLineEqual(value1, IFACE_NAME1, INT_MIN,   TEST_COUNTERSET0, 0,        lines[8]);
    expectStatsLineEqual(value1, IFACE_NAME1, INT_MIN,   TEST_COUNTERSET0, TEST_TAG, lines[9]);
    expectStatsLineEqual(value1, IFACE_NAME1, UINT_MAX,  TEST_COUNTERSET0, 0,        lines[10]);
    expectStatsLineEqual(value1, IFACE_NAME1, UINT_MAX,  TEST_COUNTERSET0, TEST_TAG, lines[11]);
}
}  // namespace bpf
}  // namespace android
