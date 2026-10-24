#include "grabber/ProjectDefinition.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

namespace {

using ldm::grabber::ApplyTemplate;
using ldm::grabber::DefinitionToJson;
using ldm::grabber::ParseDefinition;
using ldm::model::GrabberProjectDefinition;
using ldm::model::GrabberTemplate;

GrabberProjectDefinition Sample()
{
    GrabberProjectDefinition d;
    d.startPage = "https://example.com/gallery/";
    d.templateKind = GrabberTemplate::Custom;
    d.saveMode = ldm::model::GrabberSaveMode::OneFolder;
    d.folder = "C:\\Downloads\\Gallery";
    d.keepSiteFolders = true;
    d.savePages = true;
    d.convertLinks = true;
    d.overwrite = true;
    d.addToMainListOnClose = true;
    d.explore.baseSiteDepth = std::nullopt;
    d.explore.otherSitesDepth = 2;
    d.explore.noParentDirectories = false;
    d.explore.mainDomainSites = true;
    d.explore.includePaths = {"/gallery/"};
    d.explore.excludePaths = {"/gallery/private/*", "/logout"};
    d.files.includeTypes = {"*.jpg"};
    d.files.excludeTypes = {"thumb_*"};
    d.files.minBytes = 1024;
    d.files.maxBytes = 50'000'000;
    d.files.includePaths = {"/img/"};
    d.files.excludePaths = {"/ads/"};
    d.files.thisSiteOnly = true;
    d.files.hideDuplicates = false;
    d.files.downloadImmediately = true;
    d.exploreConcurrency = 7;
    d.downloadConcurrency = 2;
    d.linkTextAsDescription = false;
    d.useBrowserCookies = true;
    return d;
}

TEST(ProjectDefinition, RoundTrips)
{
    const GrabberProjectDefinition d = Sample();
    const auto parsed = ParseDefinition(DefinitionToJson(d));
    ASSERT_TRUE(parsed) << parsed.GetError().message;
    EXPECT_EQ(parsed.Value(), d);

    GrabberProjectDefinition byCategory;
    byCategory.startPage = "http://example.com/";
    byCategory.saveMode = ldm::model::GrabberSaveMode::OneCategory;
    byCategory.category = ldm::model::CategoryId{4};
    const auto parsedCategory = ParseDefinition(DefinitionToJson(byCategory));
    ASSERT_TRUE(parsedCategory);
    EXPECT_EQ(parsedCategory.Value(), byCategory);
}

TEST(ProjectDefinition, FillsMissingFieldsWithDefaults)
{
    const auto parsed = ParseDefinition(R"({"version":1,"startPage":"http://example.com/"})");
    ASSERT_TRUE(parsed);
    GrabberProjectDefinition expected;
    expected.startPage = "http://example.com/";
    EXPECT_EQ(parsed.Value(), expected);
}

TEST(ProjectDefinition, RejectsMalformedDefinitions)
{
    const std::string bad[] = {
        "",
        "not json",
        "[]",
        R"({"startPage":"http://example.com/"})",
        R"({"version":2,"startPage":"http://example.com/"})",
        R"({"version":1})",
        R"({"version":1,"startPage":"ftp://example.com/"})",
        R"({"version":1,"startPage":"javascript:void"})",
        R"({"version":1,"startPage":"http://example.com/","template":"everything"})",
        R"({"version":1,"startPage":"http://example.com/","template":3})",
        R"({"version":1,"startPage":"http://example.com/","exploreConcurrency":0})",
        R"({"version":1,"startPage":"http://example.com/","downloadConcurrency":11})",
        R"({"version":1,"startPage":"http://example.com/","explore":{"baseSiteDepth":100}})",
        R"({"version":1,"startPage":"http://example.com/","explore":{"baseSiteDepth":-1}})",
        R"({"version":1,"startPage":"http://example.com/","explore":{"otherSitesDepth":"1"}})",
        R"({"version":1,"startPage":"http://example.com/","explore":[]})",
        R"({"version":1,"startPage":"http://example.com/","explore":{"includePaths":"/a/"}})",
        R"({"version":1,"startPage":"http://example.com/","explore":{"includePaths":[""]}})",
        R"({"version":1,"startPage":"http://example.com/","explore":{"includePaths":["a\nb"]}})",
        R"({"version":1,"startPage":"http://example.com/","files":{"minBytes":10,"maxBytes":5}})",
        R"({"version":1,"startPage":"http://example.com/","files":{"minBytes":-1}})",
        R"({"version":1,"startPage":"http://example.com/","files":{"thisSiteOnly":1}})",
        R"({"version":1,"startPage":"http://example.com/","save":{"mode":"one_category"}})",
        R"({"version":1,"startPage":"http://example.com/","save":{"mode":"one_folder","folder":"relative"}})",
        R"({"version":1,"startPage":"http://example.com/","save":{"mode":"one_folder"}})",
        R"({"version":1,"startPage":"http://example.com/","save":{"category":0}})",
    };
    for (const std::string& json : bad) {
        EXPECT_FALSE(ParseDefinition(json)) << json;
    }

    std::string tooMany = R"({"version":1,"startPage":"http://example.com/","files":{"includeTypes":[)";
    for (int i = 0; i < 65; ++i) {
        tooMany += std::string(i ? "," : "") + "\"*.x" + std::to_string(i) + "\"";
    }
    tooMany += "]}}";
    EXPECT_FALSE(ParseDefinition(tooMany));
    EXPECT_FALSE(ParseDefinition(std::string(ldm::grabber::kMaxDefinitionBytes + 1, ' ')));
}

TEST(ProjectDefinition, TemplatesPresetTheFilters)
{
    GrabberProjectDefinition d = Sample();
    const GrabberProjectDefinition custom = ApplyTemplate(d, GrabberTemplate::Custom);
    EXPECT_EQ(custom, d);

    const GrabberProjectDefinition pictures = ApplyTemplate(d, GrabberTemplate::Pictures);
    EXPECT_EQ(pictures.templateKind, GrabberTemplate::Pictures);
    EXPECT_EQ(pictures.startPage, d.startPage);
    EXPECT_EQ(pictures.folder, d.folder);
    EXPECT_EQ(pictures.exploreConcurrency, d.exploreConcurrency);
    EXPECT_FALSE(pictures.explore.baseSiteDepth);
    EXPECT_EQ(pictures.files.includeTypes, ldm::grabber::PictureMasks());
    EXPECT_FALSE(pictures.savePages);

    const GrabberProjectDefinition audio = ApplyTemplate(d, GrabberTemplate::Audio);
    EXPECT_EQ(audio.explore.baseSiteDepth, 1);
    EXPECT_EQ(audio.files.includeTypes, ldm::grabber::AudioMasks());

    const GrabberProjectDefinition video = ApplyTemplate(d, GrabberTemplate::Video);
    EXPECT_EQ(video.files.includeTypes, ldm::grabber::VideoMasks());

    const GrabberProjectDefinition all = ApplyTemplate(d, GrabberTemplate::AllFiles);
    EXPECT_TRUE(all.files.includeTypes.empty());
    EXPECT_EQ(all.explore.baseSiteDepth, 1);

    const GrabberProjectDefinition site = ApplyTemplate(d, GrabberTemplate::WholeSite);
    EXPECT_FALSE(site.explore.baseSiteDepth);
    EXPECT_TRUE(site.savePages && site.convertLinks && site.keepSiteFolders && site.files.thisSiteOnly);

    const GrabberProjectDefinition page = ApplyTemplate(d, GrabberTemplate::StartPageOffline);
    EXPECT_EQ(page.explore.baseSiteDepth, 0);
    EXPECT_TRUE(page.savePages && page.convertLinks);
    EXPECT_NE(std::find(page.files.includeTypes.begin(), page.files.includeTypes.end(), "*.css"),
              page.files.includeTypes.end());

    for (const GrabberProjectDefinition& preset : {pictures, audio, video, all, site, page}) {
        EXPECT_TRUE(ParseDefinition(DefinitionToJson(preset)));
    }
}

}  // namespace
