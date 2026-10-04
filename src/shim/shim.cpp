#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DYLD_INTERPOSE(_replacement, _replacee)                                                   \
    __attribute__((used)) static struct {                                                         \
        const void* replacement;                                                                  \
        const void* replacee;                                                                     \
    } _interpose_##_replacee __attribute__((section("__DATA,__interpose"))) = {                 \
        reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(&_replacement)),               \
        reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(&_replacee))};

namespace {

struct Mapping {
    char from[512];
    char to[512];
    size_t from_len;
    size_t to_len;
};

#define MAX_MAPPINGS 16
Mapping g_mappings[MAX_MAPPINGS];
int g_mapping_count = -1;

void init_mappings() {
    if (g_mapping_count >= 0) return;
    g_mapping_count = 0;

    const char* map_str = getenv("ISOLATE_REDIRECT_MAP");
    if (map_str && *map_str) {
        char buf[2048];
        strncpy(buf, map_str, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        char* token = strtok(buf, ":");
        while (token && g_mapping_count < MAX_MAPPINGS) {
            char* eq = strchr(token, '=');
            if (eq) {
                *eq = '\0';
                const char* src = token;
                const char* dst = eq + 1;
                if (*src && *dst) {
                    Mapping& m = g_mappings[g_mapping_count++];
                    strncpy(m.from, src, sizeof(m.from) - 1);
                    m.from[sizeof(m.from) - 1] = '\0';
                    strncpy(m.to, dst, sizeof(m.to) - 1);
                    m.to[sizeof(m.to) - 1] = '\0';
                    m.from_len = strlen(m.from);
                    m.to_len = strlen(m.to);
                }
            }
            token = strtok(nullptr, ":");
        }
    }

    const char* from_env = getenv("ISOLATE_REDIRECT_FROM");
    const char* to_env = getenv("ISOLATE_REDIRECT_TO");
    if (from_env && to_env && *from_env && *to_env && g_mapping_count < MAX_MAPPINGS) {
        Mapping& m = g_mappings[g_mapping_count++];
        strncpy(m.from, from_env, sizeof(m.from) - 1);
        m.from[sizeof(m.from) - 1] = '\0';
        strncpy(m.to, to_env, sizeof(m.to) - 1);
        m.to[sizeof(m.to) - 1] = '\0';
        m.from_len = strlen(m.from);
        m.to_len = strlen(m.to);
    }
}

const char* rewrite_path(const char* in, char* out, size_t out_len) {
    if (!in || !*in) return in;
    init_mappings();

    for (int i = 0; i < g_mapping_count; ++i) {
        const Mapping& m = g_mappings[i];
        if (strncmp(in, m.from, m.from_len) == 0) {
            char next = in[m.from_len];
            if (next == '\0' || next == '/') {
                size_t remainder_len = strlen(in + m.from_len);
                if (m.to_len + remainder_len + 1 < out_len) {
                    memcpy(out, m.to, m.to_len);
                    memcpy(out + m.to_len, in + m.from_len, remainder_len + 1);
                    return out;
                }
            }
        }
    }
    return in;
}

} // namespace

extern "C" {

int shim_open(const char* path, int oflag, ...) {
    mode_t mode = 0;
    if (oflag & O_CREAT) {
        va_list ap;
        va_start(ap, oflag);
        mode = static_cast<mode_t>(va_arg(ap, int));
        va_end(ap);
    }
    char buf[1024];
    const char* target = rewrite_path(path, buf, sizeof(buf));
    return open(target, oflag, mode);
}
DYLD_INTERPOSE(shim_open, open);

int shim_openat(int fd, const char* path, int oflag, ...) {
    mode_t mode = 0;
    if (oflag & O_CREAT) {
        va_list ap;
        va_start(ap, oflag);
        mode = static_cast<mode_t>(va_arg(ap, int));
        va_end(ap);
    }
    char buf[1024];
    const char* target = rewrite_path(path, buf, sizeof(buf));
    return openat(fd, target, oflag, mode);
}
DYLD_INTERPOSE(shim_openat, openat);

int shim_stat(const char* path, struct stat* buf) {
    char pbuf[1024];
    const char* target = rewrite_path(path, pbuf, sizeof(pbuf));
    return stat(target, buf);
}
DYLD_INTERPOSE(shim_stat, stat);

int shim_lstat(const char* path, struct stat* buf) {
    char pbuf[1024];
    const char* target = rewrite_path(path, pbuf, sizeof(pbuf));
    return lstat(target, buf);
}
DYLD_INTERPOSE(shim_lstat, lstat);

int shim_access(const char* path, int mode) {
    char buf[1024];
    const char* target = rewrite_path(path, buf, sizeof(buf));
    return access(target, mode);
}
DYLD_INTERPOSE(shim_access, access);

int shim_faccessat(int fd, const char* path, int mode, int flag) {
    char buf[1024];
    const char* target = rewrite_path(path, buf, sizeof(buf));
    return faccessat(fd, target, mode, flag);
}
DYLD_INTERPOSE(shim_faccessat, faccessat);

int shim_unlink(const char* path) {
    char buf[1024];
    const char* target = rewrite_path(path, buf, sizeof(buf));
    return unlink(target);
}
DYLD_INTERPOSE(shim_unlink, unlink);

int shim_unlinkat(int fd, const char* path, int flag) {
    char buf[1024];
    const char* target = rewrite_path(path, buf, sizeof(buf));
    return unlinkat(fd, target, flag);
}
DYLD_INTERPOSE(shim_unlinkat, unlinkat);

int shim_rename(const char* oldpath, const char* newpath) {
    char b1[1024];
    char b2[1024];
    const char* t1 = rewrite_path(oldpath, b1, sizeof(b1));
    const char* t2 = rewrite_path(newpath, b2, sizeof(b2));
    return rename(t1, t2);
}
DYLD_INTERPOSE(shim_rename, rename);

int shim_renameat(int fromfd, const char* from, int tofd, const char* to) {
    char b1[1024];
    char b2[1024];
    const char* t1 = rewrite_path(from, b1, sizeof(b1));
    const char* t2 = rewrite_path(to, b2, sizeof(b2));
    return renameat(fromfd, t1, tofd, t2);
}
DYLD_INTERPOSE(shim_renameat, renameat);

DIR* shim_opendir(const char* path) {
    char buf[1024];
    const char* target = rewrite_path(path, buf, sizeof(buf));
    return opendir(target);
}
DYLD_INTERPOSE(shim_opendir, opendir);

int shim_chdir(const char* path) {
    char buf[1024];
    const char* target = rewrite_path(path, buf, sizeof(buf));
    return chdir(target);
}
DYLD_INTERPOSE(shim_chdir, chdir);

int shim_execve(const char* path, char* const argv[], char* const envp[]) {
    char buf[1024];
    const char* target = rewrite_path(path, buf, sizeof(buf));
    return execve(target, argv, envp);
}
DYLD_INTERPOSE(shim_execve, execve);

} // extern "C"
