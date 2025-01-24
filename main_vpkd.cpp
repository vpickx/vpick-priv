#define LOG_TAG "vpkd_main"
#include <time.h>
#include <pthread.h>
#include <cutils/properties.h>

#include <memory>
#include <thread>

#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include "service/VpkdService.h"
//#include "curl/curl.h"
#include <iostream>
#include <fstream>
#include <string>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <android/log.h>
#include <errno.h>


using namespace android;
using namespace std::chrono;


static pthread_t proc_monitor_thread_t;

void* system_prop_check_thread(void *para) {
    int cnt = 0;
    char temp[32] = {};

    ALOGI("system_prop_check begin");
    while(1) {
        cnt = property_get_int32("persist.hello.cnt", 0);
        cnt++;
        ALOGE("persist.hello.cnt=%d", cnt);
        sprintf(temp, "%d", cnt);
        property_set("persist.hello.cnt", temp);
        sleep(5);
    }
    ALOGI("system_prop_check end");
    return NULL;
}


bool is_already_bound(const std::string &source, const std::string &target) {
    std::ifstream mounts("/proc/mounts");
    if (!mounts.is_open()) {
        ALOGI("Failed to open /proc/mounts");
        return false;
    }

    std::string line;
    while (std::getline(mounts, line)) {
        if (line.find(source) != std::string::npos && line.find(target) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool create_source_if_missing(const std::string &source, const std::string &template_path) {
    size_t pos = source.find_last_of('/');
    if (pos == std::string::npos) {
        ALOGE("Invalid source path: %s", source.c_str());
        return false;
    }

    std::string dir = source.substr(0, pos);
    if (access(dir.c_str(), F_OK) != 0) {
        if (mkdir(dir.c_str(), 0755) != 0) {
            ALOGE("Failed to create directory: %s", dir.c_str());
            return false;
        }
        ALOGI("Created directory: %s", dir.c_str());
    }

    if (access(source.c_str(), F_OK) != 0) {
        int fd = open(source.c_str(), O_WRONLY | O_CREAT, 0644);
        if (fd < 0) {
            ALOGE("Failed to create file: %s", source.c_str());
            return false;
        }
        close(fd);

        // Copy content from the template path
        std::ifstream src_file(template_path);
        std::ofstream dst_file(source);
        if (!src_file.is_open() || !dst_file.is_open()) {
            ALOGE("Failed to open source or target for copying: %s", template_path.c_str());
            return false;
        }

        dst_file << src_file.rdbuf();
        ALOGI("Copied content to: %s", source.c_str());
    }
    return true;
}

bool bind_node(const std::string &source, const std::string &target, const std::string &template_path) {
    // Ensure the source file exists and is populated
    if (!create_source_if_missing(source, template_path)) {
        ALOGE("Failed to ensure source exists: %s", source.c_str());
        return false;
    }

    // Check if the target file exists
    struct stat target_stat;
    if (stat(target.c_str(), &target_stat) != 0) {
        ALOGI("Target file does not exist: %s", target.c_str());
        return false;
    }

    // Check if already bound
    if (is_already_bound(source, target)) {
        ALOGI("Already bound: %s -> %s", source.c_str(), target.c_str());
        return true;
    }

    // Attempt to bind mount
    if (mount(source.c_str(), target.c_str(), nullptr, MS_BIND, nullptr) != 0) {
        ALOGE("Failed to bind mount: %s", strerror(errno));
        return false;
    }

    ALOGI("Successfully bound: %s -> %s", source.c_str(), target.c_str());
    return true;
}

void mount_nodes(const std::vector<std::pair<std::string, std::string>> &nodes) {
    for (const auto &node : nodes) {
        const std::string &source = node.first;
        const std::string &target = node.second;
        const std::string template_path = target; // Use target as the template source

        if (!bind_node(source, target, template_path)) {
            ALOGE("Failed to bind node: %s -> %s", source.c_str(), target.c_str());
        }
    }
}

int init_mount() {
    // Check if the mount operation is disabled via the property
    const char *property_value = std::getenv("persist.vpk.mounts.disabled");
    if (property_value && std::string(property_value) == "1") {
        ALOGI("persist.vpk.mounts.disabled=1");
        return 0;
    }

    std::vector<std::pair<std::string, std::string>> nodes = {
        {"/data/local/tmp/plugin/proc/cpuinfo", "/proc/cpuinfo"},
        // {"/data/local/tmp/plugin/proc/meminfo", "/proc/meminfo"},
        {"/data/local/tmp/plugin/proc/version", "/proc/version"}
    };

    // Set permissions for the first member of all nodes to 644
    for (const auto &node : nodes) {
        if (chmod(node.first.c_str(), 0644) != 0) {
            ALOGE("Failed to set permissions for %s", node.first.c_str());
        }
    }

    mount_nodes(nodes);
    return 0;
}


int main(int argc, char* argv[]) {
    init_mount();
    pthread_create(&proc_monitor_thread_t, NULL, system_prop_check_thread, NULL);

    ProcessState::self()->startThreadPool();
    auto sm = defaultServiceManager();

    sp<vpk::VpkdService> vpkdService(new vpk::VpkdService);

    vpkdService->init();

    auto status = sm->addService(String16(vpk::VpkdService::getServiceName()),
                                 vpkdService,
                                 false,
                                 IServiceManager::DUMP_FLAG_PRIORITY_DEFAULT);

    ALOGI("addService:%d", status);
    IPCThreadState::self()->joinThreadPool();

    return 0;
}