#include "base/Base64.h"

#include <gtest/gtest.h>

#include <string>

namespace {

TEST(Base64, DecodesStandardPaddedText)
{
    EXPECT_EQ(ldm::base::DecodeBase64(""), "");
    EXPECT_EQ(ldm::base::DecodeBase64("YQ=="), "a");
    EXPECT_EQ(ldm::base::DecodeBase64("YWI="), "ab");
    EXPECT_EQ(ldm::base::DecodeBase64("YWJj"), "abc");
    EXPECT_EQ(ldm::base::DecodeBase64("AP8="), std::string("\x00\xff", 2));
    EXPECT_FALSE(ldm::base::DecodeBase64("YQ="));
    EXPECT_FALSE(ldm::base::DecodeBase64("Y==="));
    EXPECT_FALSE(ldm::base::DecodeBase64("YQ==YWJj")) << "padding only at the end";
    EXPECT_FALSE(ldm::base::DecodeBase64("YW Jj"));
}

}  // namespace
