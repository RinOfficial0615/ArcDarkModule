#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sysmacros.h>

#include <android/asset_manager.h>
#include <android/log.h>

#include <nlohmann/json.hpp>

#define LOG_TAG "ArcDarkModule"
#define AD_LOGD(...) ((void)__android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__))
#define AD_LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__))
#define AD_LOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__))

#include "zygisk.hpp"
#include "lsplt.hpp"

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

constexpr const char *kRuntimeClass = "java/lang/Runtime";

class ScopedUtfChars {
public:
    ScopedUtfChars(JNIEnv *env, jstring str)
        : env_(env), str_(str), chars_(nullptr) {
        if (env_ && str_) {
            chars_ = env_->GetStringUTFChars(str_, nullptr);
        }
    }

    ~ScopedUtfChars() {
        if (env_ && str_ && chars_) {
            env_->ReleaseStringUTFChars(str_, chars_);
        }
    }

    ScopedUtfChars(const ScopedUtfChars &) = delete;
    ScopedUtfChars &operator=(const ScopedUtfChars &) = delete;

    const char *get() const {
        return chars_;
    }

    explicit operator bool() const {
        return chars_ != nullptr;
    }

private:
    JNIEnv *env_;
    jstring str_;
    const char *chars_;
};

static bool IsModuleDisabled(Api *api) {
    if (!api) return false;
    const int dirfd = api->getModuleDir();
    if (dirfd < 0) return false;

    auto exists = [&](const char *name) -> bool {
        const int fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        close(fd);
        return true;
    };

    const bool disabled = exists("disable") || exists("remove");
    close(dirfd);
    return disabled;
}

static constexpr std::array<const char *, 3> kDefaultPackages = {
    "moe.inf.arc", "moe.low.arc", "moe.low.mes"};

static std::string TrimScope(std::string_view value) {
    size_t begin = 0, end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return std::string(value.substr(begin, end - begin));
}

static bool ScopeContains(std::string_view text, std::string_view package_name) {
    size_t begin = 0;
    while (begin <= text.size()) {
        size_t end = text.find('\n', begin);
        if (end == std::string_view::npos) end = text.size();
        std::string line = TrimScope(text.substr(begin, end - begin));
        if (const size_t comment = line.find('#'); comment != std::string::npos) {
            line.resize(comment);
            line = TrimScope(line);
        }
        if (!line.empty() && line == package_name) return true;
        if (end == text.size()) break;
        begin = end + 1;
    }
    return false;
}

static bool IsTargetPackage(Api *api, const char *package_name) {
    if (!api || !package_name || package_name[0] == '\0') return false;
    const int dirfd = api->getModuleDir();
    if (dirfd < 0) return false;
    char buffer[16 * 1024];
    const int fd = openat(dirfd, "scope.txt", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        close(dirfd);
        for (const char *fallback : kDefaultPackages)
            if (std::string_view(package_name) == fallback) return true;
        return false;
    }
    const ssize_t size = read(fd, buffer, sizeof(buffer));
    close(fd);
    close(dirfd);
    if (size < 0) {
        for (const char *fallback : kDefaultPackages)
            if (std::string_view(package_name) == fallback) return true;
        return false;
    }
    return ScopeContains(std::string_view(buffer, static_cast<size_t>(size)), package_name);
}

dev_t target_dev = 0;
ino_t target_ino = 0;

namespace {

constexpr const char *kSonglistPath = "songs/songlist";

bool IsLightSide(int side) {
    return side == 0 || side == 2 || side == 3;
}

bool IsDarkSide(int side) {
    return side == 1 || side == 4;
}

// Highest chart of a song, following Arcaea's ratingClass semantics:
// 0 Past / 1 Present / 2 Future / 3 Beyond or Inscribed (ratingClassAlias==1)
// and 4 Eternal. The tag doubles as part of the pack grouping key.
char HighestDifficultyTag(const nlohmann::json &song) {
    int best = -1;
    bool inscribed = false;
    const auto difficulties = song.find("difficulties");
    if (difficulties == song.end() || !difficulties->is_array()) return '?';
    for (const auto &difficulty : *difficulties) {
        if (!difficulty.is_object()) continue;
        const auto rating_class = difficulty.find("ratingClass");
        if (rating_class == difficulty.end() || !rating_class->is_number_integer()) continue;
        const int value = rating_class->get<int>();
        if (value < 0 || value > 4 || value <= best) continue;
        best = value;
        const auto alias = difficulty.find("ratingClassAlias");
        inscribed = value == 3 && alias != difficulty.end() &&
                    alias->is_number_integer() && alias->get<int>() == 1;
    }
    switch (best) {
        case 0: return 'P';
        case 1: return 'R';
        case 2: return 'F';
        case 3: return inscribed ? 'I' : 'B';
        case 4: return 'E';
        default: return '?';
    }
}

std::string SongGroupKey(const nlohmann::json &song, char tag) {
    std::string key;
    const auto set = song.find("set");
    if (set != song.end() && set->is_string()) key = set->get<std::string>();
    key.push_back('\x1f');
    key.push_back(tag);
    return key;
}

int SongSide(const nlohmann::json &song) {
    const auto side = song.find("side");
    if (side == song.end() || !side->is_number_integer()) return -1;
    return side->get<int>();
}

// The game loads the songlist through a fixed-size buffer taken from
// AAsset_getLength and expects AAsset_read to fill it completely, so the
// rewritten content must keep the exact byte length. JSON is parsed only to
// compute each song's target conflict side; the edit itself swaps the digit
// literal of "side": N in place at its original offset.
bool PlanSonglistEdits(std::string_view input, std::vector<std::pair<size_t, char>> &edits) {
    // AD_LOGD("songlist content: %.*s", static_cast<int>(input.size()), input.data());

    // allow_exceptions=false keeps parse errors silent under -fno-exceptions
    nlohmann::json root = nlohmann::json::parse(input.begin(), input.end(), nullptr, false, true);
    if (root.is_discarded()) {
        AD_LOGE("Failed to parse songlist JSON");
        return false;
    }
    const auto songs_it = root.find("songs");
    if (songs_it == root.end() || !songs_it->is_array()) {
        AD_LOGE("Invalid songlist JSON: missing or invalid 'songs' array");
        return false;
    }

    auto &songs = *songs_it;

    std::unordered_map<std::string, unsigned> dark_mask_by_group;
    std::vector<char> tags(songs.size(), '?');
    for (size_t i = 0; i < songs.size(); ++i) {
        if (!songs[i].is_object()) continue;
        tags[i] = HighestDifficultyTag(songs[i]);
        const int side = SongSide(songs[i]);
        if (IsDarkSide(side)) {
            dark_mask_by_group[SongGroupKey(songs[i], tags[i])] |= 1u << side;
        }
    }

    constexpr std::string_view kSideKey = "\"side\"";

    // collect document-order "side": <digit> value offsets from the raw text;
    // a string state machine keeps matches inside JSON string values out
    struct SideToken {
        size_t value_pos;
        int value;
    };
    std::vector<SideToken> tokens;
    bool in_string = false;
    bool escaped = false;
    for (size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];
        if (in_string) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c != '"') continue;
        if (input.compare(i, kSideKey.size(), kSideKey) == 0) {
            size_t pos = i + kSideKey.size();
            while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos]))) ++pos;
            if (pos < input.size() && input[pos] == ':') {
                do { ++pos; } while (pos < input.size() &&
                                     std::isspace(static_cast<unsigned char>(input[pos])));
                if (pos < input.size() && std::isdigit(static_cast<unsigned char>(input[pos]))) {
                    tokens.push_back({pos, static_cast<int>(input[pos]) - '0'});
                }
            }
        }
        in_string = true;  // the key itself is a string; walk to its closing quote
        escaped = false;
    }

    // every song with a side field pairs with exactly one token, in order
    std::vector<const nlohmann::json *> sided_songs;
    for (size_t i = 0; i < songs.size(); ++i) {
        if (!songs[i].is_object()) continue;
        if (SongSide(songs[i]) >= 0) sided_songs.push_back(&songs[i]);
    }
    if (sided_songs.size() != tokens.size()) {
        AD_LOGE("songlist rewrite skipped: %zu songs with side but %zu tokens found",
                sided_songs.size(), tokens.size());
        return false;
    }

    for (size_t i = 0, j = 0; i < songs.size(); ++i) {
        if (!songs[i].is_object()) continue;
        const int side = SongSide(songs[i]);
        if (side < 0) continue;  // side-less entries (e.g. deleted songs) own no token
        const SideToken &token = tokens[j++];
        if (token.value != side) {
            AD_LOGE("songlist rewrite skipped: side mismatch for song at index %zu", i);
            return false;
        }

        unsigned mask = 0;
        const auto group = dark_mask_by_group.find(SongGroupKey(songs[i], tags[i]));
        if (group != dark_mask_by_group.end()) mask = group->second;

        if (!IsLightSide(side)) continue;
        // packs without their own conflict songs fall back to side 1
        edits.emplace_back(token.value_pos, mask == (1u << 4) ? '4' : '1');
    }
    return true;
}

} // namespace

using AAssetManager_open_t = AAsset *(*)(AAssetManager *, const char *, int);
using AAsset_read_t = int (*)(AAsset *, void *, size_t);

static AAssetManager_open_t AAssetManager_open_backup = nullptr;
static AAsset_read_t AAsset_read_backup = nullptr;

std::atomic<AAsset *> target_songlist_asset{nullptr};
std::atomic<bool> songlist_patched{false};

// Only two hooks are needed: IDA confirms libcocos2dcpp.so reads assets via a
// single FileUtilsAndroid::getData call path (open -> getLength -> one full
// read -> close) and imports no seek/64-bit/remaining-length variants.
static AAsset *AAssetManager_open_hook(AAssetManager *mgr, const char *filename, int mode) {
    AAsset *asset = AAssetManager_open_backup(mgr, filename, mode);
    if (filename && asset && std::strcmp(filename, kSonglistPath) == 0) {
        target_songlist_asset.store(asset, std::memory_order_relaxed);
    }
    return asset;
}

// The hook chain is never unwound. Only the very first songlist read is
// rewritten; every later read (including integrity validation re-reads)
// returns orig() untouched, so the digest sees the pristine bytes.
static int AAsset_read_hook(AAsset *asset, void *buf, size_t count) {
    const int result = AAsset_read_backup ? AAsset_read_backup(asset, buf, count) : -1;
    if (result <= 0 || !buf) return result;
    if (target_songlist_asset.load(std::memory_order_relaxed) != asset) return result;
    if (songlist_patched.exchange(true, std::memory_order_relaxed)) return result;

    std::vector<std::pair<size_t, char>> edits;
    if (!PlanSonglistEdits({static_cast<char *>(buf), static_cast<size_t>(result)}, edits)) {
        AD_LOGE("songlist rewrite skipped: layout mismatch");
        return result;
    }
    for (const auto &edit : edits) {
        static_cast<char *>(buf)[edit.first] = edit.second;
    }
    AD_LOGI("darkened %zu song sides", edits.size());
    return result;
}

JNINativeMethod jniMethodHooks[1] = {
    "nativeLoad",
    "(Ljava/lang/String;Ljava/lang/ClassLoader;Ljava/lang/Class;)Ljava/lang/String;",
    reinterpret_cast<void *>(+[] [[clang::no_stack_protector]] (JNIEnv *env, jclass ignored, jstring javaFileName, jobject javaLoader, jobject caller) -> jstring {
#define ORIG() reinterpret_cast<jstring(*)(JNIEnv*, jclass, jstring, jobject, jobject)>(jniMethodHooks[0].fnPtr)(env, ignored, javaFileName, javaLoader, caller);

        if (!javaFileName) return ORIG();
        ScopedUtfChars lib_name(env, javaFileName);
        if (!lib_name) return ORIG();

        bool is_target = std::strstr(lib_name.get(), "libcocos2dcpp.so") != nullptr;
        if (is_target) {
            jclass cls = env->FindClass(kRuntimeClass);
            env->RegisterNatives(cls, jniMethodHooks, 1);
            env->DeleteLocalRef(cls);
        }

        auto ret = ORIG();
        if (ret != nullptr) return ret; // nativeLoad failed

        if (is_target) {
            for (auto &m : lsplt::MapInfo::Scan()) {
                if (m.path.ends_with("libcocos2dcpp.so")) {
                    target_dev = m.dev;
                    target_ino = m.inode;
                    break;
                }
            }

            lsplt::RegisterHook(target_dev, target_ino, "AAssetManager_open",
                                reinterpret_cast<void*>(AAssetManager_open_hook),
                                reinterpret_cast<void**>(&AAssetManager_open_backup));
            lsplt::RegisterHook(target_dev, target_ino, "AAsset_read",
                                reinterpret_cast<void*>(AAsset_read_hook),
                                reinterpret_cast<void**>(&AAsset_read_backup));
            lsplt::CommitHook();
        }

        return ret;

#undef ORIG
    })
};

class ArcDarkModule : public zygisk::ModuleBase {
public:
    void onLoad(Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
    }

    void preServerSpecialize([[maybe_unused]] ServerSpecializeArgs *args) override {
        if (IsModuleDisabled(api)) {
            api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
    }

    void preAppSpecialize(AppSpecializeArgs *args) override {
        if (IsModuleDisabled(api)) {
            api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        bool enable_module = false;

        ScopedUtfChars package_name(env, args->nice_name);
        if (package_name) {
            if (IsTargetPackage(api, package_name.get())) {
                enable_module = true;
            }
        }

        if (!enable_module) {
            api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        api->hookJniNativeMethods(env, kRuntimeClass, jniMethodHooks, 1);
    }

private:
    Api *api;
    JNIEnv *env;
};

REGISTER_ZYGISK_MODULE(ArcDarkModule)
