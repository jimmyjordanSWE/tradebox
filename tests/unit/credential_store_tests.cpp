#include "tradebox/platform/credentials.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <ranges>
#include <string>

namespace {

TEST(CredentialStore, SeparatesPaperAndLiveCredentialsForOneSlot) {
    const std::string slot =
        "tradebox-test-" +
        std::to_string(std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count());
    std::string error;
    ASSERT_TRUE(CredentialStore::Delete(slot, true, error));
    ASSERT_TRUE(CredentialStore::Delete(slot, false, error));

    AlpacaCredentials paper("paper-key", "paper-secret", true);
    AlpacaCredentials live("live-key", "live-secret", false);
    ASSERT_TRUE(CredentialStore::Save(slot, paper, error)) << error;
    ASSERT_TRUE(CredentialStore::Save(slot, live, error)) << error;
    EXPECT_TRUE(CredentialStore::Exists(slot, true));
    EXPECT_TRUE(CredentialStore::Exists(slot, false));

    AlpacaCredentials loaded_paper;
    AlpacaCredentials loaded_live;
    ASSERT_TRUE(CredentialStore::Load(slot, true, loaded_paper, error))
        << error;
    ASSERT_TRUE(CredentialStore::Load(slot, false, loaded_live, error))
        << error;
    EXPECT_EQ(loaded_paper.key, "paper-key");
    EXPECT_EQ(loaded_paper.secret, "paper-secret");
    EXPECT_TRUE(loaded_paper.paper);
    EXPECT_EQ(loaded_live.key, "live-key");
    EXPECT_EQ(loaded_live.secret, "live-secret");
    EXPECT_FALSE(loaded_live.paper);

    const auto listed = CredentialStore::List();
    ASSERT_TRUE(listed) << listed.error();
    const auto paper_entry = std::ranges::find_if(
        *listed, [&slot](const CredentialStore::Descriptor& descriptor) {
            return descriptor.slot == slot && descriptor.paper;
        });
    ASSERT_NE(paper_entry, listed->end());
    EXPECT_EQ(paper_entry->api_key_id, "paper-key");
    const auto live_entry = std::ranges::find_if(
        *listed, [&slot](const CredentialStore::Descriptor& descriptor) {
            return descriptor.slot == slot && !descriptor.paper;
        });
    ASSERT_NE(live_entry, listed->end());
    EXPECT_EQ(live_entry->api_key_id, "live-key");

    const std::string renamed_slot = slot + "-renamed";
    ASSERT_TRUE(CredentialStore::Rename(
        slot, renamed_slot, true, error)) << error;
    EXPECT_FALSE(CredentialStore::Exists(slot, true));
    EXPECT_TRUE(CredentialStore::Exists(renamed_slot, true));

    ASSERT_TRUE(CredentialStore::Delete(renamed_slot, true, error)) << error;
    ASSERT_TRUE(CredentialStore::Delete(slot, false, error)) << error;
    EXPECT_FALSE(CredentialStore::Exists(slot, true));
    EXPECT_FALSE(CredentialStore::Exists(slot, false));
}

}  // namespace
