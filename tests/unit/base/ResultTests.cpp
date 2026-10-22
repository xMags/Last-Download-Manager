#include "base/Result.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace {

using ldm::base::ErrorCode;
using ldm::base::MakeError;
using ldm::base::Result;
using ldm::base::Status;

TEST(Result, HoldsValue)
{
    Result<int> result = 42;
    ASSERT_TRUE(result.Ok());
    EXPECT_EQ(result.Value(), 42);
}

TEST(Result, HoldsError)
{
    Result<int> result = MakeError(ErrorCode::NotFound, "missing");
    ASSERT_FALSE(result.Ok());
    EXPECT_EQ(result.GetError().code, ErrorCode::NotFound);
    EXPECT_EQ(result.GetError().message, "missing");
}

TEST(Result, MovesOutMoveOnlyValue)
{
    Result<std::unique_ptr<std::string>> result = std::make_unique<std::string>("payload");
    ASSERT_TRUE(result.Ok());
    std::unique_ptr<std::string> value = std::move(result).Value();
    EXPECT_EQ(*value, "payload");
}

TEST(Status, DefaultIsSuccess)
{
    Status status;
    EXPECT_TRUE(status.Ok());
}

TEST(Status, CarriesError)
{
    Status status = MakeError(ErrorCode::Io, "disk");
    ASSERT_FALSE(status.Ok());
    EXPECT_EQ(status.GetError().code, ErrorCode::Io);
}

}  // namespace
