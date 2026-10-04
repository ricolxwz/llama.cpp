#include "ggml-backend-impl.h"
#include "ggml-backend.h"
#include "ggml-backend-dl.h"
#include "ggml-impl.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>
#include <cctype>

// 其他 .cpp 文件通过 ggml_backend_reg_* 和 ggml_backend_dev_* 等公开 API 访问全局注册表.
// get_reg() 仅在本文件内可用, 每次返回同一个注册表实例的引用.

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#elif defined(__APPLE__)
#    include <mach-o/dyld.h>
#    include <dlfcn.h>
#else
#    include <dlfcn.h>
#    include <unistd.h>
#endif

// Backend registry
#ifdef GGML_USE_CPU
#include "ggml-cpu.h"
#endif

#ifdef GGML_USE_CUDA
#include "ggml-cuda.h"
#endif

#ifdef GGML_USE_METAL
#include "ggml-metal.h"
#endif

#ifdef GGML_USE_SYCL
#include "ggml-sycl.h"
#endif

#ifdef GGML_USE_VULKAN
#include "ggml-vulkan.h"
#endif

#ifdef GGML_USE_WEBGPU
#include "ggml-webgpu.h"
#endif

#ifdef GGML_USE_ZDNN
#include "ggml-zdnn.h"
#endif

#ifdef GGML_USE_OPENCL
#include "ggml-opencl.h"
#endif

#ifdef GGML_USE_HEXAGON
#include "ggml-hexagon.h"
#endif

#ifdef GGML_USE_BLAS
#include "ggml-blas.h"
#endif

#ifdef GGML_USE_RPC
#include "ggml-rpc.h"
#endif

#ifdef GGML_USE_VIRTGPU_FRONTEND
#include "ggml-virtgpu.h"
#endif

#ifdef GGML_USE_CANN
#include "ggml-cann.h"
#endif

#ifdef GGML_USE_ZENDNN
#include "ggml-zendnn.h"
#endif

#ifdef GGML_USE_OPENVINO
#include "ggml-openvino.h"
#endif

#ifdef GGML_USE_ET
#include "ggml-et.h"
#endif

namespace fs = std::filesystem;

// 将文件系统路径fs::path转为保存UTF-i字节的std::string, 同时兼容C++17和C++20
static std::string path_str(const fs::path & path) {
    try {
#if defined(__cpp_lib_char8_t)
        // C++20 and later: u8string() returns std::u8string
        const std::u8string u8str = path.u8string();
        return std::string(reinterpret_cast<const char *>(u8str.data()), u8str.size());
#else
        // C++17: u8string() returns std::string
        return path.u8string();
#endif
    } catch (...) {
        return std::string();
    }
}

struct ggml_backend_reg_entry {
    ggml_backend_reg_t reg;
    dl_handle_ptr handle;
};

// 全局注册表定义
struct ggml_backend_registry {
    std::vector<ggml_backend_reg_entry> backends;  // 保存所有已经注册的后端
    std::vector<ggml_backend_dev_t> devices;  // 汇总这些后端提供的设备

    // 后端加入列表, 设备加入列表; 这里初始化的时候调用register_backend说明是编译的时候已经动态链接了, 而不是运行时动态加载
    ggml_backend_registry() {
#ifdef GGML_USE_CUDA
        register_backend(ggml_backend_cuda_reg());
#endif
#ifdef GGML_USE_METAL
        register_backend(ggml_backend_metal_reg());
#endif
#ifdef GGML_USE_SYCL
        register_backend(ggml_backend_sycl_reg());
#endif
#ifdef GGML_USE_VULKAN
    // Add runtime disable check
    if (getenv("GGML_DISABLE_VULKAN") == nullptr) {
        register_backend(ggml_backend_vk_reg());
    } else {
        GGML_LOG_DEBUG("Vulkan backend disabled by GGML_DISABLE_VULKAN environment variable\n");
    }
#endif
#ifdef GGML_USE_WEBGPU
        register_backend(ggml_backend_webgpu_reg());
#endif
#ifdef GGML_USE_ZDNN
        register_backend(ggml_backend_zdnn_reg());
#endif
#ifdef GGML_USE_VIRTGPU_FRONTEND
        register_backend(ggml_backend_virtgpu_reg());
#endif

#ifdef GGML_USE_OPENCL
        register_backend(ggml_backend_opencl_reg());
#endif
#ifdef GGML_USE_ZENDNN
        register_backend(ggml_backend_zendnn_reg());
#endif
#ifdef GGML_USE_HEXAGON
        register_backend(ggml_backend_hexagon_reg());
#endif
#ifdef GGML_USE_CANN
        register_backend(ggml_backend_cann_reg());
#endif
#ifdef GGML_USE_BLAS
        register_backend(ggml_backend_blas_reg());
#endif
#ifdef GGML_USE_RPC
        register_backend(ggml_backend_rpc_reg());
#endif
#ifdef GGML_USE_OPENVINO
        register_backend(ggml_backend_openvino_reg());
#endif
#ifdef GGML_USE_ET
        register_backend(ggml_backend_et_reg());
#endif
#ifdef GGML_USE_CPU
        register_backend(ggml_backend_cpu_reg());
#endif
    }

    ~ggml_backend_registry() {
        // FIXME: backends cannot be safely unloaded without a function to destroy all the backend resources,
        // since backend threads may still be running and accessing resources from the dynamic library
        for (auto & entry : backends) {
            if (entry.handle) {
                entry.handle.release(); // NOLINT
            }
        }
    }

    // 将后端加入到全局后端列表, 再把它提供的设备加入全局设备列表
    void register_backend(ggml_backend_reg_t reg, dl_handle_ptr handle = nullptr) {
        if (!reg) {
            return;
        }

        for (auto & entry : backends) {
            if (entry.reg == reg) {
                return;
            }
        }

#ifndef NDEBUG
        GGML_LOG_DEBUG("%s: registered backend %s (%zu devices)\n",
            __func__, ggml_backend_reg_name(reg), ggml_backend_reg_dev_count(reg));
#endif
        backends.push_back({ reg, std::move(handle) });
        for (size_t i = 0; i < ggml_backend_reg_dev_count(reg); i++) {
            register_device(ggml_backend_reg_dev_get(reg, i));
        }
    }

    // 将设备加入全局设备列表
    void register_device(ggml_backend_dev_t device) {
        for (auto & dev : devices) {
            if (dev == device) {
                return;
            }
        }

#ifndef NDEBUG
        GGML_LOG_DEBUG("%s: registered device %s (%s)\n", __func__, ggml_backend_dev_name(device), ggml_backend_dev_description(device));
#endif
        devices.push_back(device);
    }

    ggml_backend_reg_t load_backend(const fs::path & path, bool silent) {
        dl_handle_ptr handle { dl_load_library(path) };  // 获取动态库句柄
        if (!handle) {
            if (!silent) {
                GGML_LOG_ERROR("%s: failed to load %s: %s\n", __func__, path_str(path).c_str(), dl_error());
            }
            return nullptr;
        }

        auto score_fn = (ggml_backend_score_t) dl_get_sym(handle.get(), "ggml_backend_score");  // ggml_backend_load_best()中是临时加载, 是为了比较不同的版本, 选出得分最高的库. 这里是第二次加载的句柄, 目的是检查是否支持当前的机器, 如果库提供的评分函数返回0, 就拒绝加载. 为什么不能省掉第二次检查, 因为load_backend()不一定经过load_best()才被调用, 用户可以直接ggml_backend_load(path)指定动态库路径加载, 这种情况下根本没有第一次打分, 因此load_backend()需要自己完成检查. 这里做了一个C风格的类型转换, ggml_backend_score_t是一个函数指针类型, typedef int (*ggml_backend_score_t)(void). 
        if (score_fn && score_fn() == 0) {
            if (!silent) {
                GGML_LOG_INFO("%s: backend %s is not supported on this system\n", __func__, path_str(path).c_str());
            }
            return nullptr;
        }

        auto backend_init_fn = (ggml_backend_init_t) dl_get_sym(handle.get(), "ggml_backend_init");  // 找到后端的初始化函数
        if (!backend_init_fn) {
            if (!silent) {
                GGML_LOG_ERROR("%s: failed to find ggml_backend_init in %s\n", __func__, path_str(path).c_str());
            }
            return nullptr;
        }

        ggml_backend_reg_t reg = backend_init_fn();
        if (!reg || reg->api_version != GGML_BACKEND_API_VERSION) {
            if (!silent) {
                if (!reg) {
                    GGML_LOG_ERROR("%s: failed to initialize backend from %s: ggml_backend_init returned NULL\n",
                        __func__, path_str(path).c_str());
                } else {
                    GGML_LOG_ERROR("%s: failed to initialize backend from %s: incompatible API version (backend: %d, current: %d)\n",
                        __func__, path_str(path).c_str(), reg->api_version, GGML_BACKEND_API_VERSION);
                }
            }
            return nullptr;
        }

        GGML_LOG_INFO("%s: loaded %s backend from %s\n", __func__, ggml_backend_reg_name(reg), path_str(path).c_str());

        register_backend(reg, std::move(handle));  // 将句柄转移到全局后端注册表中

        return reg;
    }

    void unload_backend(ggml_backend_reg_t reg, bool silent) {
        auto it = std::find_if(backends.begin(), backends.end(),
                               [reg](const ggml_backend_reg_entry & entry) { return entry.reg == reg; });

        if (it == backends.end()) {
            if (!silent) {
                GGML_LOG_ERROR("%s: backend not found\n", __func__);
            }
            return;
        }

        if (!silent) {
            GGML_LOG_DEBUG("%s: unloading %s backend\n", __func__, ggml_backend_reg_name(reg));
        }

        // remove devices
        devices.erase(
            std::remove_if(devices.begin(), devices.end(),
                            [reg](ggml_backend_dev_t dev) { return ggml_backend_dev_backend_reg(dev) == reg; }),
            devices.end());

        // remove backend
        backends.erase(it);
    }
};

// 返回一个单例的全局后端注册表
static ggml_backend_registry & get_reg() {
    static ggml_backend_registry reg;
    return reg;
}

// Internal API
void ggml_backend_register(ggml_backend_reg_t reg) {
    get_reg().register_backend(reg);
}

void ggml_backend_device_register(ggml_backend_dev_t device) {
    get_reg().register_device(device);
}

// Backend (reg) enumeration
static bool striequals(const char * a, const char * b) {
    for (; *a && *b; a++, b++) {
        if (std::tolower(*a) != std::tolower(*b)) {
            return false;
        }
    }
    return *a == *b;
}

// 返回已注册的后端数量
size_t ggml_backend_reg_count() {
    return get_reg().backends.size();
}

// 从全局后端注册表中, 按照索引取出一个已经注册的后端, 返回它的ggml_backend_reg_t句柄. 
ggml_backend_reg_t ggml_backend_reg_get(size_t index) {
    GGML_ASSERT(index < ggml_backend_reg_count());
    return get_reg().backends[index].reg;
}

// 根据后端名字查找后端注册表, 返回对应的ggml_backend_reg_t句柄. 如果没有找到, 返回nullptr.
ggml_backend_reg_t ggml_backend_reg_by_name(const char * name) {
    for (size_t i = 0; i < ggml_backend_reg_count(); i++) {
        ggml_backend_reg_t reg = ggml_backend_reg_get(i);
        if (striequals(ggml_backend_reg_name(reg), name)) {
            return reg;
        }
    }
    return nullptr;
}

// 返回已注册的设备数量
size_t ggml_backend_dev_count() {
    return get_reg().devices.size();
}

// 从全局后端注册表中, 按照索引取出一个已经注册的设备, 返回它的ggml_backend_dev_t句柄.
ggml_backend_dev_t ggml_backend_dev_get(size_t index) {
    GGML_ASSERT(index < ggml_backend_dev_count());
    return get_reg().devices[index];
}

// 根据设备名字查找设备注册表, 返回对应的ggml_backend_dev_t句柄. 
ggml_backend_dev_t ggml_backend_dev_by_name(const char * name) {
    for (size_t i = 0; i < ggml_backend_dev_count(); i++) {
        ggml_backend_dev_t dev = ggml_backend_dev_get(i);
        if (striequals(ggml_backend_dev_name(dev), name)) {
            return dev;
        }
    }
    return nullptr;
}

// 根据设备类型查找设备注册表, 返回对应的ggml_backend_dev_t句柄. 设备类型见 enum ggml_backend_dev_type, 例如GGML_BACKEND_DEVICE_TYPE_CPU, GGML_BACKEND_DEVICE_TYPE_GPU等.
ggml_backend_dev_t ggml_backend_dev_by_type(enum ggml_backend_dev_type type) {
    for (size_t i = 0; i < ggml_backend_dev_count(); i++) {
        ggml_backend_dev_t dev = ggml_backend_dev_get(i);
        if (ggml_backend_dev_type(dev) == type) {
            return dev;
        }
    }
    return nullptr;
}

// ggml_backend_dev_t代表设备, ggml_backend_t代表在该设备上创建的计算执行实例. 流程为: 先通过ggml_backend_dev_by_name()或者ggml_backend_dev_by_type()获取设备句柄, 然后调用ggml_backend_dev_init()创建计算执行实例. 也可以直接通过ggml_backend_init_by_name()或者ggml_backend_init_by_type()一步到位创建计算执行实例.
ggml_backend_t ggml_backend_init_by_name(const char * name, const char * params) {
    ggml_backend_dev_t dev = ggml_backend_dev_by_name(name);
    if (!dev) {
        return nullptr;
    }
    return ggml_backend_dev_init(dev, params);
}

ggml_backend_t ggml_backend_init_by_type(enum ggml_backend_dev_type type, const char * params) {
    ggml_backend_dev_t dev = ggml_backend_dev_by_type(type);
    if (!dev) {
        return nullptr;
    }
    return ggml_backend_dev_init(dev, params);
}

ggml_backend_t ggml_backend_init_best(void) {
    ggml_backend_dev_t dev = ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_GPU);
    dev = dev ? dev : ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_IGPU);
    dev = dev ? dev : ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_CPU);
    if (!dev) {
        return nullptr;
    }
    return ggml_backend_dev_init(dev, nullptr);
}

// Dynamic loading
ggml_backend_reg_t ggml_backend_load(const char * path) {
    return get_reg().load_backend(path, false);
}

void ggml_backend_unload(ggml_backend_reg_t reg) {
    get_reg().unload_backend(reg, true);
}

static fs::path get_executable_path() {
#if defined(__APPLE__)
    // get executable path
    std::vector<char> path;
    uint32_t size;
    while (true) {
        size = path.size();
        if (_NSGetExecutablePath(path.data(), &size) == 0) {
            break;
        }
        path.resize(size);
    }
    std::string base_path(path.data(), size);
    // remove executable name
    auto last_slash = base_path.find_last_of('/');
    if (last_slash != std::string::npos) {
        base_path = base_path.substr(0, last_slash);
    }
    return base_path + "/";
#elif defined(__linux__) || defined(__FreeBSD__)
    std::string base_path = ".";
    std::vector<char> path(1024);
    while (true) {
        // get executable path
#    if defined(__linux__)
        ssize_t len = readlink("/proc/self/exe", path.data(), path.size());
#    elif defined(__FreeBSD__)
        ssize_t len = readlink("/proc/curproc/file", path.data(), path.size());
#    endif
        if (len == -1) {
            break;
        }
        if (len < (ssize_t) path.size()) {
            base_path = std::string(path.data(), len);
            // remove executable name
            auto last_slash = base_path.find_last_of('/');
            if (last_slash != std::string::npos) {
                base_path = base_path.substr(0, last_slash);
            }
            break;
        }
        path.resize(path.size() * 2);
    }

    return base_path + "/";
#elif defined(_WIN32)
    std::vector<wchar_t> path(MAX_PATH);
    DWORD len = GetModuleFileNameW(NULL, path.data(), path.size());
    if (len == 0) {
        return {};
    }
    std::wstring base_path(path.data(), len);
    // remove executable name
    auto last_slash = base_path.find_last_of('\\');
    if (last_slash != std::string::npos) {
        base_path = base_path.substr(0, last_slash);
    }
    return base_path + L"\\";
#else
    return {};
#endif
}

static fs::path backend_filename_prefix() {
#ifdef _WIN32
    return fs::u8path("ggml-");
#else
    return fs::u8path("libggml-");
#endif
}

static fs::path backend_filename_extension() {
#ifdef _WIN32
    return fs::u8path(".dll");
#else
    return fs::u8path(".so");
#endif
}

// 在同一种backend的多个动态库版本里面, 选出最合适当前机器的那个并加载. 对每个候选backend调用它提供的ggml_backend_score(), 分数越高, 说明越适合当前的机器. 0表示当前机器不支持这个backend, 最后加载最高分的那个. 
static ggml_backend_reg_t ggml_backend_load_best(const char * name, bool silent, const char * user_search_path) {
    // enumerate all the files that match [lib]ggml-name-*.[so|dll] in the search paths
    const fs::path name_path = fs::u8path(name);  // 将后端传入的名字, 如"cuda", "metal"转为std::filesystem::path
    const fs::path file_prefix = backend_filename_prefix().native() + name_path.native() + fs::u8path("-").native();  // 拼接出文件名的前缀, .native()是把path转为平台原生的字符串类型, 方便直接用+拼接. std::filesystem::path没有二元运算符operator+, 如果写backend_filename_prefix()/name_path, 才会按照路径层级拼接
    const fs::path file_extension = backend_filename_extension();  // 获取文件后缀, windows上是.dll; 其他是.so

    std::vector<fs::path> search_paths;
    if (user_search_path == nullptr) {
#ifdef GGML_BACKEND_DIR  // 如果编译的时候定义了这个宏, 那么就直接加入到搜索路径里面
        search_paths.push_back(fs::u8path(GGML_BACKEND_DIR));
#endif
        // 默认搜索路径: 可执行文件目录和当前工作目录
        // default search paths: executable directory, current directory
        search_paths.push_back(get_executable_path());  // 获取当前可执行文件所在目录的内部辅助函数, 例如返回/home/user/llama.cpp/build/bin/
        std::error_code cwd_ec;
        const fs::path cwd = fs::current_path(cwd_ec);  // 获取当前工作目录, 然后保存到cwd, 如果调用失败, 不会抛出异常, 而是将错误写入cwd_ec, 后续的代码会检查; 该函数接受一个error_code的引用
        if (cwd_ec) {
            GGML_LOG_DEBUG("%s: current_path() failure, error-message: %s\n", __func__, cwd_ec.message().c_str());
        } else {
            search_paths.push_back(cwd);
        }
    } else {
        // 否则就是用户提供的路径
        search_paths.push_back(fs::u8path(user_search_path));
    }

    int best_score = 0;
    fs::path best_path;
    std::error_code ec;

    for (const auto & search_path : search_paths) {
        if (!fs::exists(search_path, ec)) {
            if (ec) {  // 查询文件时失败了, 常见原因有Permission denied, I/O error, 文件系统异常等
                GGML_LOG_DEBUG("%s: posix_stat(%s) failure, error-message: %s\n", __func__, path_str(search_path).c_str(), ec.message().c_str());
            } else {  // 路径不存在
                GGML_LOG_DEBUG("%s: search path %s does not exist\n", __func__, path_str(search_path).c_str());
            }
            continue;
        }
        std::error_code dir_ec;
        fs::directory_iterator dir_it(search_path, fs::directory_options::skip_permission_denied, dir_ec);  // 使用()初始化
        if (dir_ec) {
            GGML_LOG_DEBUG("%s: failed to enumerate %s: %s\n", __func__, path_str(search_path).c_str(), dir_ec.message().c_str());
            continue;
        }
        for (const fs::directory_iterator end; dir_it != end; dir_it.increment(dir_ec)) {  // 让dir_it移动到下一个目录项, 如果出错就写入dir_ec. 
            const auto & entry = *dir_it;  // 从目录迭代器中读出当前文件条目
            if (entry.is_regular_file(ec)) {  // 判断是否为普通文件
                auto filename = entry.path().filename();  // 文件名
                auto ext = entry.path().extension();  // 扩展名
                if (filename.native().find(file_prefix) == 0 && ext == file_extension) {  // 文件名以file_prefix开头, 例如libggml-cuda-; 扩展名等于file_extension, 例如.so或者.dll只有满足这两个条件的文件才会被处理, 所以像libggml-cuda-12.so这种带后缀的都会被扫描到 
                    dl_handle_ptr handle { dl_load_library(entry) };  // 尝试动态加载这个共享库. 这是一个带有自定义删除器的智能指针. 它的析构函数里面会调用dlclse, 所以当这句所在的作用域结束的时候(也就是每次for循环迭代结束, 准备处理下一个文件的时候), handle就会自动析构, 对应的动态库会被dlclose掉. 这里只是临时加载用来调用ggml_backend_score()打分, 打分完就立刻卸载. 真正长期加载最优后端的动作, 是在函数的最后: load_backend
                    if (!handle && !silent) {
                        GGML_LOG_ERROR("%s: failed to load %s: %s\n", __func__, path_str(entry.path()).c_str(), dl_error());
                    }
                    if (handle) {
                        auto score_fn = (ggml_backend_score_t) dl_get_sym(handle.get(), "ggml_backend_score");  // 加载成功后, 尝试获取库中的符号ggml_backend_score
                        if (score_fn) {
                            int s = score_fn();  // 如果找到了评分函数, 就调用它, 得到当前后端的评分s
#ifndef NDEBUG
                            GGML_LOG_DEBUG("%s: %s score: %d\n", __func__, path_str(entry.path()).c_str(), s);
#endif
                            if (s > best_score) {
                                best_score = s;
                                best_path = entry.path();
                            }  // 如果当前库的得分比之前记录的最高分还高, 就更新best_score和best_path. 
                        } else {
                            if (!silent) {
                                GGML_LOG_INFO("%s: failed to find ggml_backend_score in %s\n", __func__, path_str(entry.path()).c_str());
                            }
                        }
                    }
                }
            }
        }
    }

    if (best_score == 0) {
        // 如果最高分为0(说明没有找到任何带评分函数的后端, 或者全部得分为0), 就退而求其次, 尝试加载基础班的后端(不带版本号/额外后缀的那个); 如带评分的后端是libggml-cuda-12.so; 基础后端是libggml-cuda.so
        // try to load the base backend
        for (const auto & search_path : search_paths) {
            fs::path filename = backend_filename_prefix().native() + name_path.native() + backend_filename_extension().native();
            fs::path path = search_path / filename;
            if (std::error_code ec; fs::exists(path, ec)) {  // 如果基础文件存在, 就直接加载它
                return get_reg().load_backend(path, silent);
            } else {
                if (ec) {
                    GGML_LOG_DEBUG("%s: posix_stat(%s) failure, error-message: %s\n", __func__, path_str(path).c_str(), ec.message().c_str());
                }
            }
        }
        return nullptr;
    }

    return get_reg().load_backend(best_path, silent);  // get_reg()首次调用会构造ggml_backend_registry
}

void ggml_backend_load_all() {
    ggml_backend_load_all_from_path(nullptr);
}

void ggml_backend_load_all_from_path(const char * dir_path) {
#ifdef NDEBUG
    bool silent = true;
#else
    bool silent = false;
#endif

    // 在同一种backend的多个动态库版本里面, 选出最合适当前机器的那个并加载; silent表示是否静默加载; 它要去磁盘上找动态库, 所以需要一个目录告诉它"去哪里找", 比如说windows下面可能有C:\llama\backends\ggml-cpu.dll; ggml-cuda.dll; ggml-vulkan.dll, 函数会在这个目录里面搜索对应的backends. 注意, 这里是CUDA内部选score最高的版本->加载; CPU内部选score最高的版本->加载...
    ggml_backend_load_best("blas", silent, dir_path);  // 主要给CPU用, 通用矩阵运算库接口
    ggml_backend_load_best("zendnn", silent, dir_path);  // 主要给AMD CPU用, AMD专门针对Zen系列CPU做的深度学习优化库
    ggml_backend_load_best("cann", silent, dir_path);  // 主要给华为昇腾GPU用
    ggml_backend_load_best("cuda", silent, dir_path);
    ggml_backend_load_best("hip", silent, dir_path);  // 主要给AMD GPU用, AMD版的CUDA体系, 主要跑Radeon/Instinct
    ggml_backend_load_best("metal", silent, dir_path);
    ggml_backend_load_best("rpc", silent, dir_path);  // 把计算通过网络扔给另一台机器执行
    ggml_backend_load_best("sycl", silent, dir_path);  // 主要给Intel GPU用
    ggml_backend_load_best("vulkan", silent, dir_path);  // 给各类GPU用, 通用GPU图形/计算API, NVIDIA/AMD/Intel都可能支持
    ggml_backend_load_best("virtgpu", silent, dir_path);  // 虚拟GPU/远程GPU接口
    ggml_backend_load_best("opencl", silent, dir_path);  // 通用GPU计算接口
    ggml_backend_load_best("hexagon", silent, dir_path);  // 主要给骁龙NPU用
    ggml_backend_load_best("musa", silent, dir_path);  // 主要给摩尔线程GPU用
    ggml_backend_load_best("openvino", silent, dir_path);  // 主要给Intel CPU/GPU/NPU用
    ggml_backend_load_best("cpu", silent, dir_path);  // 最基础的CPU计算后端
    // check the environment variable GGML_BACKEND_PATH to load an out-of-tree backend
    const char * backend_path = std::getenv("GGML_BACKEND_PATH");
    if (backend_path) {
        ggml_backend_load(backend_path);  // 如果用户设置了这个环境变量, 那么, 额外加载这个指定的动态库
    }
}
