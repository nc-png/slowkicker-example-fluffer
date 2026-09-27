// The MIT License (MIT)
//
// SlowKicker v0.3 Copyright (c) 2014 Biohazard
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <cstring>
#include <stdint.h>
#include <cerrno>
#include <ctime>
#include <csignal>
#include <cstdarg>
#include <string>
#include <sstream>
#include <deque>
#include <sys/time.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fnmatch.h>

// fluffer's ONLINE row: the glftpd 2.x layout (32-bit timevals, pack 4),
// 904 bytes.  Embedded so the kicker builds without glftpd sources.
#pragma pack(push, 4)
struct timeval32 { int32_t tv_sec; int32_t tv_usec; };
struct ONLINE
{
    char      tagline[64];
    char      username[24];
    char      status[256];
    int16_t   ssl_flag;
    char      host[256];        // "ident@ip" at login, bare data-peer IP while transferring
    char      currentdir[256];  // points at the file while transferring
    int32_t   groupid;
    int32_t   login_time;
    timeval32 tstart;
    timeval32 txfer;
    uint64_t  bytes_xfer;
    uint64_t  bytes_txfer;
    int32_t   procid;           // transfer thread TID while transferring
};
#pragma pack(pop)
static_assert(sizeof(ONLINE) == 904, "ONLINE layout mismatch");

struct Directory
{
    const char* mask;
    double minSpeed;
    std::time_t minDuration;
    int maxKicks;
};

const char*       GLFTPD_ROOT   = "/glftpd";
const char*       LOG_FILE      = "/glftpd/ftp-data/logs/slowkicker.log";
const char*       LOCK_FILE     = "/glftpd/tmp/slowkicker.lock";
const key_t       IPC_KEY       = 0x0000DEAD;  // ipc_key in fluffer.conf
// SIGRTMIN aborts the transfer (session survives), SIGRTMIN+1 aborts and
// closes the session.  fluffer ignores anything that isn't a tgkill.
const int         KICK_SIGNAL   = SIGRTMIN;
bool              ONCE_ONLY     = true;
Directory         DIRECTORIES[] = {
    { "/site/iso/*",        125,    /* kB/s */    10,    /* seconds */  3 },
    { "/site/mp3/*",        125,    /* kB/s */    10,    /* seconds */  3 },
    { "/site/0day/*",       125,    /* kB/s */    10,    /* seconds */  3 }
};

struct KickInfo
{
    int32_t procid;
    std::string username;
    std::string groupname;
    std::string path;
    std::string sourceAddress;
    double speed;
};

struct History
{
    std::string username;
    std::string path;
    int numKicks;
};

std::deque<History> history;
const std::size_t maxHistory = 1000;

// The daemon is one process with one shared fd table, so /proc/<pid>/fd
// can't attribute a socket to a transfer.  fluffer publishes the data
// peer in the host field instead: a bare IP while transferring, the
// "ident@ip" login form until the data connection is established.
std::string lookupSourceAddress(const ONLINE& online)
{
    std::string host(online.host, strnlen(online.host, sizeof(online.host)));
    std::size_t at = host.find('@');
    return at == std::string::npos ? host : host.substr(at + 1);
}

pid_t tgid;  // daemon PID, from shm_cpid

int tgkill(pid_t tid, int sig)
{
    return syscall(SYS_tgkill, tgid, tid, sig);
}

History* getHistory(const std::string& username, const std::string& path)
{
    for (std::size_t i = 0; i < history.size(); ++i) {
        if (history[i].username == username && history[i].path == path) {
            return &history[i];
        }
    }

    return NULL;
}

int getNumKicks(const std::string& username, const std::string& path)
{
    History* entry = getHistory(username, path);
    if (entry == NULL) {
        return 0;
    }

    return entry->numKicks;
}

void incrNumKicks(const std::string& username, const std::string& path)
{
    History* entry = getHistory(username, path);
    if (entry == NULL) {
        History entry = { username, path, 1 };
        history.push_front(entry);
        if (history.size() >= maxHistory) {
            history.pop_back();
        }
        return;
    }

    ++entry->numKicks;
}

const Directory* getDirectory(const std::string& path)
{
    static const std::size_t numDirectories = sizeof(DIRECTORIES) / sizeof(Directory);

    for (std::size_t i = 0; i < numDirectories; ++i) {
        if (!fnmatch(DIRECTORIES[i].mask, path.c_str(), 0)) {
            return &DIRECTORIES[i];
        }
    }

    return NULL;
}

std::string formatTimestamp()
{
    const std::time_t now = std::time(NULL);
    char timestamp[26];
    std::strftime(timestamp, sizeof(timestamp), "%a %b %e %T %Y", std::localtime(&now));
    return timestamp;
}

std::string lookupGroup(int32_t gid)
{
    std::string group = "NoGroup";
    std::string path = std::string(GLFTPD_ROOT) + "/etc/group";
    FILE* f = std::fopen(path.c_str(), "r");
    if (f != NULL) {
        char buf[1024];
        while (std::fgets(buf, sizeof(buf), f)) {
            char* p = std::strtok(buf, ":");
            if (p == NULL) {
                continue;
            }

            std::string currentGroup = p;
            p = std::strtok(NULL, ":");
            if (p == NULL) {
                continue;
            }

            p = std::strtok(NULL, ":");
            if (p == NULL) {
                continue;
            }

            char* pEnd;
            int32_t currentGid = std::strtol(p, &pEnd, 10);
            if (*pEnd != '\0' || currentGid != gid) {
                continue;
            }

            group = currentGroup;
            break;
        }
        std::fclose(f);
    }
    return group;
}

enum GlftpdLogTag
{
    GLTSlow,
    GLTZeroByte,
    GLTStalled
};

void gllog(GlftpdLogTag tag, const KickInfo& info)
{
    std::string logPath = GLFTPD_ROOT + std::string("/ftp-data/logs/glftpd.log");
    FILE* f = std::fopen(logPath.c_str(), "a");
    if (f == NULL) {
        return;
    }

    switch (tag) {
        case GLTSlow :
            std::fprintf(f, "%s SLOW: \"%s\" \"%s\" \"%s\" \"%s\" \"%.0f\"\n",
                         formatTimestamp().c_str(),
                         info.path.c_str(),
                         info.username.c_str(),
                         info.groupname.c_str(),
                         info.sourceAddress.c_str(),
                         info.speed);
            break;

        case GLTZeroByte :
            std::fprintf(f, "%s ZEROBYTE: \"%s\" \"%s\" \"%s\" \"%s\"\n",
                         formatTimestamp().c_str(),
                         info.path.c_str(),
                         info.username.c_str(),
                         info.groupname.c_str(),
                         info.sourceAddress.c_str());
            break;

        case GLTStalled :
            std::fprintf(f, "%s STALLED: \"%s\" \"%s\" \"%s\" \"%s\"\n",
                         formatTimestamp().c_str(),
                         info.path.c_str(),
                         info.username.c_str(),
                         info.groupname.c_str(),
                         info.sourceAddress.c_str());
            break;
    }

    std::fclose(f);
}

void log(const char* format,...)
{
    FILE* f = std::fopen(LOG_FILE, "a");
    if (f == NULL) {
        return;
    }

    std::fprintf(f, "%s ", formatTimestamp().c_str());

    va_list args;
    va_start(args, format);
    vfprintf(f, format, args);
    va_end(args);

    std::fprintf(f, "\n");
    std::fclose(f);
}

// A row is transferring iff status is "STOR <file>" and currentdir ends
// with that file.  Anything else is a finished or not-yet-started transfer.
std::string buildPath(const ONLINE& online)
{
    std::string filename(online.status + 5);
    while (!filename.empty() && !std::isprint(filename[filename.size() - 1])) {
        filename.resize(filename.size() - 1);
    }
    if (filename.empty()) {
        return "";
    }

    std::string path(online.currentdir, strnlen(online.currentdir, sizeof(online.currentdir)));
    if (path.size() <= filename.size() ||
        path.compare(path.size() - filename.size() - 1, std::string::npos, "/" + filename) != 0) {
        return "";
    }

    return path;
}

bool needsKicking(const ONLINE& online, KickInfo& info)
{
    if (online.procid == 0 ||
        strncasecmp(online.status, "STOR ", 5) != 0 ||
        tgkill(online.procid, 0) < 0) {

        return false;
    }

    info.username = online.username;
    info.path = buildPath(online);
    if (info.path.empty()) {
        return false;
    }

    const Directory* directory = getDirectory(info.path);
    if (directory == NULL) {
        return false;
    }

    struct timeval now;
    gettimeofday(&now, NULL);

    double duration = (now.tv_sec - online.tstart.tv_sec) +
                      ((now.tv_usec - online.tstart.tv_usec) / 1000000.0);
    info.speed = (duration == 0 ? online.bytes_xfer
                                : online.bytes_xfer / duration) / 1024.0;
    if (duration < directory->minDuration || info.speed >= directory->minSpeed) {
        return false;
    }

    if (getNumKicks(info.username, info.path) >= directory->maxKicks) {
        return false;
    }

    info.groupname = lookupGroup(online.groupid);
    info.procid = online.procid;
    info.sourceAddress = lookupSourceAddress(online);

    return true;
}

bool kick(const KickInfo& info)
{
    const std::string realPath = GLFTPD_ROOT + info.path;

    struct stat st;
    if (stat(realPath.c_str(), &st) < 0) {
        if (errno != ENOENT) {
            log("Unable to stat path: %s: %s", realPath.c_str(), strerror(errno));
        }
        return false;
    }

    // Never kill(): the TID belongs to the daemon's thread group, so a
    // process-directed signal would hit the whole daemon.  The file is
    // not deleted here either -- post_check decides its fate ($4=1).
    if (tgkill(info.procid, KICK_SIGNAL) < 0) {
        if (errno != ESRCH) {
            log("Unable to signal transfer: %ld: %s", (long) info.procid, strerror(errno));
        }
        return false;
    }

    const char *reason;
    GlftpdLogTag tag;
    if (st.st_size == 0) {
        reason = "zero byte";
        tag = GLTZeroByte;
    }
    else if (info.speed == 0) {
        reason = "stalling upload";
        tag = GLTStalled;
    }
    else {
        reason = "slow uploading";
        tag = GLTSlow;
    }

    log("Kicked user for %s: %s: %s: %.0fkB/s: %s",
        reason, info.username.c_str(), info.sourceAddress.c_str(),
        info.speed, info.path.c_str());
    gllog(tag, info);

    return true;
}

// fluffer removes and recreates the segment on every start, so a new
// shmid (or IPC_STAT failing on the cached one) means the daemon
// restarted: drop the old mapping and attach the new segment.
ONLINE* onlineUsers = NULL;
std::size_t numOnline = 0;
int shmid = -1;

void detach_online()
{
    if (onlineUsers != NULL) {
        shmdt(onlineUsers);
    }
    onlineUsers = NULL;
    numOnline = 0;
    shmid = -1;
}

bool open_online()
{
    int id = shmget(IPC_KEY, 0, 0);
    if (id < 0) {
        if (errno != ENOENT) {
            log("Unable to open online users: shmget: %s", strerror(errno));
        }
        detach_online();
        return false;
    }

    struct shmid_ds stat;
    if (shmctl(id, IPC_STAT, &stat) < 0) {
        log("Unable to open online users: shmctl: %s", strerror(errno));
        detach_online();
        return false;
    }

    if (id == shmid && tgid == stat.shm_cpid) {
        return true;
    }

    detach_online();
    ONLINE* p = (ONLINE*) shmat(id, NULL, SHM_RDONLY);
    if (p == (ONLINE*) -1) {
        log("Unable to open online users: shmat: %s", strerror(errno));
        return false;
    }

    onlineUsers = p;
    shmid = id;
    tgid = stat.shm_cpid;
    numOnline = stat.shm_segsz / sizeof(ONLINE);
    log("Attached online users: shmid %d, daemon pid %ld, %zu slots",
        shmid, (long) tgid, numOnline);
    return true;
}

void check()
{
    if (!open_online()) {
        return;
    }

    for (std::size_t i = 0; i < numOnline; ++i) {
        KickInfo info;
        if (!needsKicking(onlineUsers[i], info)) {
            continue;
        }

        if (kick(info)) {
            incrNumKicks(info.username, info.path);
        }
    }
}

bool acquireLock()
{
    int fd = open(LOCK_FILE, O_CREAT | O_WRONLY, 0600);
    if (fd < 0) {
        std::cerr << "Unable to create/open lock file: " << strerror(errno) << std::endl;
        return false;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        if (errno == EWOULDBLOCK) {
            std::cerr << "Slowkicker is already running." << std::endl;
            return false;
        }

        std::cerr << "Unable to acquire exclusive lock: " << strerror(errno) << std::endl;
        return false;
    }

    return true;
}

int main()
{
    if (!acquireLock()) {
        return 1;
    }

    if (!fork()) {
        while (true) {
            check();
            sleep(1);
        }
    }
}
