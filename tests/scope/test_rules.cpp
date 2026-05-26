// Unit tests for the per-app scope module: FNV-1a hash, fixed-capacity
// open-addressing rule table, and the text-format rule loader.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

#include "config/profile.hpp"
#include "presets/preset.hpp"
#include "scope/app_hash.hpp"
#include "scope/rules.hpp"
#include "scope/rules_loader.hpp"

namespace ls = loginext::scope;
namespace lp = loginext::presets;
namespace lc = loginext::config;

// ─────────────────────────────────────────────────────────────────────
// FNV-1a hash
// ─────────────────────────────────────────────────────────────────────

TEST(AppHash, CaseInsensitive) {
    // The hash is lower-cased so the focus listener doesn't have to care
    // whether the WM class is "Firefox", "firefox", or "FIREFOX".
    EXPECT_EQ(ls::hash_app("firefox"),  ls::hash_app("Firefox"));
    EXPECT_EQ(ls::hash_app("firefox"),  ls::hash_app("FIREFOX"));
    EXPECT_EQ(ls::hash_app("VS Code"),  ls::hash_app("vs code"));
}

TEST(AppHash, DistinctAppsDiffer) {
    EXPECT_NE(ls::hash_app("firefox"),  ls::hash_app("chromium"));
    EXPECT_NE(ls::hash_app("code"),     ls::hash_app("gimp"));
}

TEST(AppHash, NeverProducesZeroSentinel) {
    // hash_app re-rolls to fnv_prime if the natural FNV output is 0, so
    // the "global rule" sentinel never collides with a real app. We can't
    // easily force the all-zeros internal state from a string input, but
    // we can sanity-check that the public constants and a wide sample of
    // inputs all yield non-zero hashes.
    static_assert(ls::fnv_prime != 0, "fnv_prime must be non-zero");
    const char* samples[] = {
        "firefox", "chromium", "code", "gimp", "inkscape",
        "blender", "kitty", "alacritty", "obs", "discord",
        "", "a", "A", "/", "\xff",
    };
    for (const char* s : samples) {
        EXPECT_NE(ls::hash_app(s, std::strlen(s)), 0u) << "input: " << s;
    }
}

TEST(AppHash, EmptyStringIsStable) {
    // Empty input is a fixed value (the offset basis), not undefined.
    EXPECT_EQ(ls::hash_app("", 0), ls::hash_app("", 0));
}

// ─────────────────────────────────────────────────────────────────────
// Rule table: insert / lookup
// ─────────────────────────────────────────────────────────────────────

TEST(RuleTable, InsertAndLookup) {
    ls::RuleTable t;
    ls::clear(t);

    const uint32_t h = ls::hash_app("firefox");
    ASSERT_TRUE(ls::insert(t, h, lp::PresetId::TabNav,
                           lc::SensitivityMode::High,
                           ls::invert_on));

    ls::AppRule out{};
    ASSERT_TRUE(ls::lookup(t, h, out));
    EXPECT_EQ(out.app_hash, h);
    EXPECT_EQ(out.preset,   lp::PresetId::TabNav);
    EXPECT_EQ(out.mode,     lc::SensitivityMode::High);
    EXPECT_EQ(out.invert,   ls::invert_on);
}

TEST(RuleTable, LookupMissReturnsFalse) {
    ls::RuleTable t;
    ls::clear(t);
    ls::AppRule out{};
    EXPECT_FALSE(ls::lookup(t, ls::hash_app("ghost"), out));
}

TEST(RuleTable, ZeroHashSentinelRejected) {
    // Inserting hash 0 is forbidden (it's the global sentinel) — insert
    // must reject it, lookup of 0 must always miss.
    ls::RuleTable t;
    ls::clear(t);
    EXPECT_FALSE(ls::insert(t, 0, lp::PresetId::TabNav));
    ls::AppRule out{};
    EXPECT_FALSE(ls::lookup(t, 0, out));
}

TEST(RuleTable, DuplicateInsertOverwrites) {
    // Re-inserting the same app_hash must overwrite (not duplicate) the
    // existing slot. Used by config reload to update existing rules.
    ls::RuleTable t;
    ls::clear(t);
    const uint32_t h = ls::hash_app("code");
    ASSERT_TRUE(ls::insert(t, h, lp::PresetId::TabNav));
    const uint8_t count_after_first = t.count;
    ASSERT_TRUE(ls::insert(t, h, lp::PresetId::Zoom,
                           lc::SensitivityMode::High));
    EXPECT_EQ(t.count, count_after_first);  // no new slot consumed

    ls::AppRule out{};
    ASSERT_TRUE(ls::lookup(t, h, out));
    EXPECT_EQ(out.preset, lp::PresetId::Zoom);
    EXPECT_EQ(out.mode,   lc::SensitivityMode::High);
}

TEST(RuleTable, RespectsLoadFactorCap) {
    // Per rules.cpp the table refuses inserts once count reaches
    // rule_capacity/2 to keep the open-addressing load factor < 0.5.
    // After that point new inserts must fail rather than evict.
    ls::RuleTable t;
    ls::clear(t);
    const std::size_t cap = ls::rule_capacity / 2;  // 32
    std::size_t inserted = 0;
    for (std::size_t i = 0; i < ls::rule_capacity; ++i) {
        char name[16];
        std::snprintf(name, sizeof(name), "app%03zu", i);
        if (ls::insert(t, ls::hash_app(name, std::strlen(name)),
                       lp::PresetId::TabNav)) {
            ++inserted;
        }
    }
    EXPECT_EQ(inserted, cap);
    EXPECT_EQ(t.count,  cap);
}

TEST(RuleTable, ClearEmptiesEverything) {
    ls::RuleTable t;
    ls::clear(t);
    ASSERT_TRUE(ls::insert(t, ls::hash_app("firefox"), lp::PresetId::TabNav));
    ASSERT_GT(t.count, 0u);
    ls::clear(t);
    EXPECT_EQ(t.count, 0u);
    ls::AppRule out{};
    EXPECT_FALSE(ls::lookup(t, ls::hash_app("firefox"), out));
}

// ─────────────────────────────────────────────────────────────────────
// Rule loader (text-format parser)
// ─────────────────────────────────────────────────────────────────────

namespace {

// RAII temp file for loader tests. Uses mkstemp so two parallel test
// executables don't race on a fixed path.
class TempRuleFile {
public:
    explicit TempRuleFile(const std::string& contents) {
        char tmpl[] = "/tmp/loginext-test-rules.XXXXXX";
        fd_ = ::mkstemp(tmpl);
        if (fd_ < 0) {
            ADD_FAILURE() << "mkstemp failed";
            return;
        }
        path_ = tmpl;
        ssize_t n = ::write(fd_, contents.data(), contents.size());
        EXPECT_EQ(static_cast<size_t>(n), contents.size());
        ::close(fd_);
        fd_ = -1;
    }
    ~TempRuleFile() {
        if (!path_.empty()) ::unlink(path_.c_str());
    }
    TempRuleFile(const TempRuleFile&)            = delete;
    TempRuleFile& operator=(const TempRuleFile&) = delete;

    const std::string& path() const noexcept { return path_; }

private:
    std::string path_;
    int         fd_ = -1;
};

} // namespace

TEST(RulesLoader, ParsesSimpleRules) {
    TempRuleFile f(
        "# comment line — ignored\n"
        "\n"
        "firefox=tab_nav\n"
        "code=zoom\n"
    );

    ls::RuleTable t;
    EXPECT_TRUE(ls::load_rules(f.path(), t));
    EXPECT_EQ(t.count, 2u);

    ls::AppRule out{};
    ASSERT_TRUE(ls::lookup(t, ls::hash_app("firefox"), out));
    EXPECT_EQ(out.preset, lp::PresetId::TabNav);
    EXPECT_EQ(out.mode,   lc::SensitivityMode::Inherit);
    EXPECT_EQ(out.invert, ls::invert_inherit);

    ASSERT_TRUE(ls::lookup(t, ls::hash_app("code"), out));
    EXPECT_EQ(out.preset, lp::PresetId::Zoom);
}

TEST(RulesLoader, ParsesFullCsvForm) {
    // `code=zoom,high,true` — full three-field form.
    TempRuleFile f("code=zoom,high,true\n");
    ls::RuleTable t;
    EXPECT_TRUE(ls::load_rules(f.path(), t));

    ls::AppRule out{};
    ASSERT_TRUE(ls::lookup(t, ls::hash_app("code"), out));
    EXPECT_EQ(out.preset, lp::PresetId::Zoom);
    EXPECT_EQ(out.mode,   lc::SensitivityMode::High);
    EXPECT_EQ(out.invert, ls::invert_on);
}

TEST(RulesLoader, ParsesPartialCsvWithEmptyMode) {
    // `gimp=zoom,,false` — preset + invert, mode left empty → Inherit.
    TempRuleFile f("gimp=zoom,,false\n");
    ls::RuleTable t;
    EXPECT_TRUE(ls::load_rules(f.path(), t));

    ls::AppRule out{};
    ASSERT_TRUE(ls::lookup(t, ls::hash_app("gimp"), out));
    EXPECT_EQ(out.preset, lp::PresetId::Zoom);
    EXPECT_EQ(out.mode,   lc::SensitivityMode::Inherit);
    EXPECT_EQ(out.invert, ls::invert_off);
}

TEST(RulesLoader, TrackedOnlyLineSkipsTableInsert) {
    // `inkscape=` is "tracked-only" per rules_loader.cpp — the UI keeps
    // the chip, the daemon adds no rule.
    TempRuleFile f(
        "inkscape=\n"
        "firefox=tab_nav\n"
    );
    ls::RuleTable t;
    EXPECT_TRUE(ls::load_rules(f.path(), t));

    EXPECT_EQ(t.count, 1u);
    ls::AppRule out{};
    EXPECT_FALSE(ls::lookup(t, ls::hash_app("inkscape"), out));
    EXPECT_TRUE (ls::lookup(t, ls::hash_app("firefox"),  out));
}

TEST(RulesLoader, MissingFileIsNotAnError) {
    // Per the contract: missing file → cleared table, returns true so the
    // daemon falls back to the global preset for everything.
    ls::RuleTable t;
    ls::clear(t);
    // Pre-populate to verify load_rules clears even when the file is absent.
    ASSERT_TRUE(ls::insert(t, ls::hash_app("stale"), lp::PresetId::TabNav));
    ASSERT_EQ(t.count, 1u);

    EXPECT_TRUE(ls::load_rules("/tmp/loginext-does-not-exist-xyz.txt", t));
    EXPECT_EQ(t.count, 0u);
}

TEST(RulesLoader, MalformedLineReportsFailure) {
    // A line without '=' is malformed; load_rules logs a warning and
    // returns false. Valid lines on either side still parse cleanly.
    TempRuleFile f(
        "firefox=tab_nav\n"
        "no-equals-here\n"
        "code=zoom\n"
    );
    ls::RuleTable t;
    EXPECT_FALSE(ls::load_rules(f.path(), t));
    EXPECT_EQ(t.count, 2u);
    ls::AppRule out{};
    EXPECT_TRUE(ls::lookup(t, ls::hash_app("firefox"), out));
    EXPECT_TRUE(ls::lookup(t, ls::hash_app("code"),    out));
}

TEST(RulesLoader, UnknownPresetIgnored) {
    // `foo=banana` — unknown preset name; the line is dropped, load
    // returns false, but other valid lines still apply.
    TempRuleFile f(
        "foo=banana\n"
        "firefox=tab_nav\n"
    );
    ls::RuleTable t;
    EXPECT_FALSE(ls::load_rules(f.path(), t));
    EXPECT_EQ(t.count, 1u);
    ls::AppRule out{};
    EXPECT_TRUE(ls::lookup(t, ls::hash_app("firefox"), out));
}
