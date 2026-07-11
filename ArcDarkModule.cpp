#include <unordered_set>
#include <cstring>
#include <string_view>
#include <atomic>

#include <fcntl.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <android/asset_manager.h>

#define LOG_TAG "ArcDarkModule"
#include "android/log_macros.h"

#include "zygisk.hpp"
#include "lsplt/include/lsplt.hpp"

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

static const std::unordered_set<std::string_view> target_pkgs = {
    "moe.inf.arc",
    "moe.low.arc",
    "moe.low.mes", // My custom package
};


dev_t target_dev = 0;
ino_t target_ino = 0;

std::atomic<AAsset*> target_songlist_asset{nullptr};

typedef AAsset* (*AAssetManager_open_t)(AAssetManager*, const char*, int);
static AAssetManager_open_t AAssetManager_open_backup = nullptr;

static AAsset* AAssetManager_open_hook(AAssetManager *mgr, const char *filename, int mode) {
    auto result = AAssetManager_open_backup(mgr, filename, mode);
    if (filename) {
        // ALOGI("AAssetManager_open_hook: %s", filename);
        if (std::strcmp(filename, "songs/songlist") == 0) {
            target_songlist_asset.store(result, std::memory_order_relaxed);
        }
    }
    return result;
}

typedef int (*AAsset_read_t)(AAsset*, void*, size_t);
static AAsset_read_t AAsset_read_backup = nullptr;

// The songlist will only be read for 2 times, at the startup screen.
// The first read is for the actual songlist, and the second read is to hash the songlist.
// So we only need to modify the first read, and simply unhook.
static int AAsset_read_hook(AAsset *asset, void *buf, size_t count) {
    auto result = AAsset_read_backup(asset, buf, count);
    if (result <= 0) return result;

    if (target_songlist_asset.load(std::memory_order_relaxed) == asset) {
        char *ptr = static_cast<char *>(buf);
        char *current = ptr;
        char *end = ptr + result;

        while (current < end - 8) {
            void *found = std::memchr(current, '"', end - current - 8);
            if (!found) break;

            char *pos = static_cast<char *>(found);

            if (std::memcmp(pos, "\"side\": ", 8) == 0) {
                char *val_ptr = pos + 8;
                if (*val_ptr == '0' || *val_ptr == '2' || *val_ptr == '3') {
                    *val_ptr = '1';
                    current = val_ptr + 1;
                } else {
                    current = pos + 1;
                }
            } else {
                current = pos + 1;
            }
        }

        lsplt::RegisterHook(target_dev, target_ino, "AAssetManager_open", 
                            reinterpret_cast<void*>(AAssetManager_open_backup), 
                            nullptr);
        lsplt::RegisterHook(target_dev, target_ino, "AAsset_read", 
                            reinterpret_cast<void*>(AAsset_read_backup),
                            nullptr);
        lsplt::CommitHook();
    }
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
            if (target_pkgs.find(package_name.get()) != target_pkgs.end()) {
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
