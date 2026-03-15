#include <set>
#include <vector>
#include <memory>
#include <array>
#include <unordered_map>
#include <string_view>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <android/asset_manager.h>
// #define LOG_TAG "ArcDarkModule"
// #include "android/log_macros.h"

#include "zygisk.hpp"

#include "lsplt/include/lsplt.hpp"

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

constexpr const char *kRuntimeClass = "java/lang/Runtime";

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

static const std::set<std::string_view> target_pkgs = {
    "moe.inf.arc",
    "moe.low.arc",
    "moe.low.mes", // My custom package
};

static const std::unordered_map<std::string_view, std::string_view> asset_replacements = {
    { "track.png", "track_dark.png" },
    { "track_rei.png", "track_dark.png" },
    { "track_extralane_light.png", "track_extralane_dark.png" },
    // { "track_finale.png", "track_dark.png" },
    // { "track_arcana.png", "track_dark.png" },
    // { "track_black.png", "track_dark.png" },
    // { "track_pentiment.png", "track_dark.png" },
    // { "track_tempestissimo.png", "track_dark.png" },
};

typedef AAsset* (*AAssetManager_open_t)(AAssetManager*, const char*, int);
static AAssetManager_open_t AAssetManager_open_backup = nullptr;

static AAsset* AAssetManager_open_hook(AAssetManager *mgr, const char *filename, int mode) {
    if (filename) {
        std::string_view file_sv(filename);

        auto last_slash = file_sv.find_last_of('/'); // We can always find the last slash, so no more checks
        std::string_view base_name = file_sv.substr(last_slash + 1);

        auto it = asset_replacements.find(base_name);
        if (it != asset_replacements.end()) {
            std::string new_path{};
            if (last_slash != std::string_view::npos) {
                new_path.append(file_sv.substr(0, last_slash + 1));
            }
            new_path.append(it->second);
            return AAssetManager_open_backup(mgr, new_path.c_str(), mode);
        }
    }
    return AAssetManager_open_backup(mgr, filename, mode);
}

JNINativeMethod jniMethodHooks[1] = {
    "nativeLoad", 
    "(Ljava/lang/String;Ljava/lang/ClassLoader;Ljava/lang/Class;)Ljava/lang/String;", 
    reinterpret_cast<void *>(+[] [[clang::no_stack_protector]] (JNIEnv *env, jclass ignored, jstring javaFileName, jobject javaLoader, jobject caller) -> jstring {
#define ORIG() reinterpret_cast<jstring(*)(JNIEnv*, jclass, jstring, jobject, jobject)>(jniMethodHooks[0].fnPtr)(env, ignored, javaFileName, javaLoader, caller);
        
        if (!javaFileName) return ORIG();
        auto lib_name = env->GetStringUTFChars(javaFileName, nullptr);
        if (lib_name == nullptr)
            return ORIG();

        bool is_target = std::strstr(lib_name, "libcocos2dcpp.so") != nullptr;
        if (is_target) {
            jclass cls = env->FindClass(kRuntimeClass);
            env->RegisterNatives(cls, jniMethodHooks, 1);
            env->DeleteLocalRef(cls);
        }

        auto ret = ORIG();
        if (ret != nullptr) return ret; // nativeLoad failed

        if (is_target) {
            dev_t dev = 0;
            ino_t ino = 0;
            for (auto &m : lsplt::MapInfo::Scan()) {
                if (m.path.ends_with("libcocos2dcpp.so")) {
                    dev = m.dev;
                    ino = m.inode;
                    break;
                }
            }

            lsplt::RegisterHook(dev, ino, "AAssetManager_open", 
                                reinterpret_cast<void*>(AAssetManager_open_hook), 
                                reinterpret_cast<void**>(&AAssetManager_open_backup));
            lsplt::CommitHook();
        }
        
        env->ReleaseStringUTFChars(javaFileName, lib_name);
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

        const char* package_name = env->GetStringUTFChars(args->nice_name, nullptr);
        if (package_name) {
            if (target_pkgs.find(package_name) != target_pkgs.end()) {
                enable_module = true;
            }
            env->ReleaseStringUTFChars(args->nice_name, package_name);
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
