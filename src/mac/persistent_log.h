#pragma once

#import <Foundation/Foundation.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

// Persistent stdout/stderr capture for the signed macOS app. The app's existing
// native and JavaScript diagnostics already converge on these streams, so one
// early tee captures them without changing every call site. The original stderr
// remains a sink as well, preserving Xcode's live console while debugging.
namespace maclog {

constexpr off_t kMaxLogBytes = 5 * 1024 * 1024;

struct State {
    int readFd = -1;
    int originalStdout = -1;
    int originalStderr = -1;
    int fileFd = -1;
    int flushRequestReadFd = -1;
    int flushRequestWriteFd = -1;
    int flushAckReadFd = -1;
    int flushAckWriteFd = -1;
    off_t fileBytes = 0;
    pthread_t pumpThread{};
    bool pumpStarted = false;
    std::string currentPath;
    std::string firstArchivePath;
    std::string secondArchivePath;
};

static State* gState = nullptr;
static NSString* gDirectoryPath = nil;
static NSString* gCurrentFilePath = nil;

static bool writeFully(int fd, const void* bytes, size_t length) {
    const char* cursor = static_cast<const char*>(bytes);
    while (length > 0) {
        const ssize_t written = ::write(fd, cursor, length);
        if (written > 0) {
            cursor += written;
            length -= static_cast<size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

static void rotate(State* state) {
    if (state->fileFd >= 0) {
        ::close(state->fileFd);
        state->fileFd = -1;
    }
    ::unlink(state->secondArchivePath.c_str());
    ::rename(state->firstArchivePath.c_str(), state->secondArchivePath.c_str());
    ::rename(state->currentPath.c_str(), state->firstArchivePath.c_str());
    state->fileFd = ::open(state->currentPath.c_str(),
                           O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    state->fileBytes = 0;
}

static bool drainAvailable(State* state) {
    bool reachedEnd = false;
    char buffer[16 * 1024];
    for (;;) {
        const ssize_t count = ::read(state->readFd, buffer, sizeof(buffer));
        if (count > 0) {
            if (state->fileFd >= 0 && state->fileBytes > 0 &&
                state->fileBytes + count > kMaxLogBytes) {
                rotate(state);
            }
            if (state->fileFd >= 0 && writeFully(state->fileFd, buffer, static_cast<size_t>(count))) {
                state->fileBytes += count;
            }
            const int console = state->originalStderr >= 0
                ? state->originalStderr
                : state->originalStdout;
            if (console >= 0) writeFully(console, buffer, static_cast<size_t>(count));
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count == 0) reachedEnd = true;
        break;  // EAGAIN means the pipe is drained for now.
    }
    return reachedEnd;
}

static void* pump(void* opaque) {
    State* state = static_cast<State*>(opaque);
    bool running = true;
    while (running) {
        pollfd descriptors[] = {
            {state->readFd, POLLIN | POLLHUP, 0},
            {state->flushRequestReadFd, POLLIN | POLLHUP, 0},
        };
        const int ready = ::poll(descriptors, 2, -1);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (drainAvailable(state)) running = false;
        }
        if (descriptors[1].revents & POLLIN) {
            unsigned char requests[32];
            const ssize_t requestCount = ::read(state->flushRequestReadFd,
                                                requests, sizeof(requests));
            if (requestCount > 0) {
                // A request is written only after fflush(). Drain the data pipe
                // before acknowledging so applicationWillTerminate can safely
                // let AppKit exit without dropping its final diagnostics.
                drainAvailable(state);
                if (state->fileFd >= 0) ::fsync(state->fileFd);
                for (ssize_t i = 0; i < requestCount; ++i) {
                    const unsigned char acknowledgement = 1;
                    writeFully(state->flushAckWriteFd, &acknowledgement, 1);
                }
            }
        }
    }
    if (state->fileFd >= 0) {
        ::fsync(state->fileFd);
        ::close(state->fileFd);
        state->fileFd = -1;
    }
    ::close(state->readFd);
    ::close(state->flushRequestReadFd);
    ::close(state->flushAckWriteFd);
    state->readFd = -1;
    return nullptr;
}

static bool isAppBundle() {
    NSString* extension = NSBundle.mainBundle.bundlePath.pathExtension;
    return [extension caseInsensitiveCompare:@"app"] == NSOrderedSame;
}

static bool install() {
    if (gState || !isAppBundle()) return gState != nullptr;

    NSFileManager* files = NSFileManager.defaultManager;
    NSError* error = nil;
    NSURL* library = [files URLForDirectory:NSLibraryDirectory
                                   inDomain:NSUserDomainMask
                          appropriateForURL:nil
                                     create:YES
                                      error:&error];
    NSURL* directory = [library URLByAppendingPathComponent:@"Logs/Pixi Native"
                                                isDirectory:YES];
    if (!library || ![files createDirectoryAtURL:directory
                      withIntermediateDirectories:YES
                                       attributes:nil
                                            error:&error]) {
        fprintf(stderr, "[log/mac] cannot create persistent log directory: %s\n",
                error.localizedDescription.UTF8String ?: "unknown error");
        return false;
    }

    gDirectoryPath = [directory.path copy];
    gCurrentFilePath = [[directory URLByAppendingPathComponent:@"runtime.log"].path copy];
    State* state = new State();
    state->currentPath = gCurrentFilePath.fileSystemRepresentation;
    state->firstArchivePath = [[directory URLByAppendingPathComponent:@"runtime.1.log"].path fileSystemRepresentation];
    state->secondArchivePath = [[directory URLByAppendingPathComponent:@"runtime.2.log"].path fileSystemRepresentation];

    struct stat info{};
    if (::stat(state->currentPath.c_str(), &info) == 0 && info.st_size >= kMaxLogBytes) {
        rotate(state);
    } else {
        state->fileFd = ::open(state->currentPath.c_str(),
                               O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (state->fileFd >= 0 && ::fstat(state->fileFd, &info) == 0) {
            state->fileBytes = info.st_size;
        }
    }
    if (state->fileFd < 0) {
        fprintf(stderr, "[log/mac] cannot open persistent log file %s: %s\n",
                state->currentPath.c_str(), std::strerror(errno));
        delete state;
        return false;
    }

    int descriptors[2] = {-1, -1};
    if (::pipe(descriptors) != 0) {
        fprintf(stderr, "[log/mac] cannot create log pipe: %s\n", std::strerror(errno));
        ::close(state->fileFd);
        delete state;
        return false;
    }
    int flushRequests[2] = {-1, -1};
    int flushAcknowledgements[2] = {-1, -1};
    if (::pipe(flushRequests) != 0 || ::pipe(flushAcknowledgements) != 0) {
        fprintf(stderr, "[log/mac] cannot create log flush pipe: %s\n", std::strerror(errno));
        ::close(descriptors[0]);
        ::close(descriptors[1]);
        if (flushRequests[0] >= 0) ::close(flushRequests[0]);
        if (flushRequests[1] >= 0) ::close(flushRequests[1]);
        if (flushAcknowledgements[0] >= 0) ::close(flushAcknowledgements[0]);
        if (flushAcknowledgements[1] >= 0) ::close(flushAcknowledgements[1]);
        ::close(state->fileFd);
        delete state;
        return false;
    }
    state->readFd = descriptors[0];
    state->flushRequestReadFd = flushRequests[0];
    state->flushRequestWriteFd = flushRequests[1];
    state->flushAckReadFd = flushAcknowledgements[0];
    state->flushAckWriteFd = flushAcknowledgements[1];
    state->originalStdout = ::dup(STDOUT_FILENO);
    state->originalStderr = ::dup(STDERR_FILENO);
    ::fcntl(state->readFd, F_SETFD, FD_CLOEXEC);
    ::fcntl(state->readFd, F_SETFL, ::fcntl(state->readFd, F_GETFL) | O_NONBLOCK);
    ::fcntl(descriptors[1], F_SETFD, FD_CLOEXEC);
    ::fcntl(state->flushRequestReadFd, F_SETFD, FD_CLOEXEC);
    ::fcntl(state->flushRequestWriteFd, F_SETFD, FD_CLOEXEC);
    ::fcntl(state->flushAckReadFd, F_SETFD, FD_CLOEXEC);
    ::fcntl(state->flushAckWriteFd, F_SETFD, FD_CLOEXEC);
#ifdef F_SETNOSIGPIPE
    if (state->originalStdout >= 0) ::fcntl(state->originalStdout, F_SETNOSIGPIPE, 1);
    if (state->originalStderr >= 0) ::fcntl(state->originalStderr, F_SETNOSIGPIPE, 1);
#endif

    if (::pthread_create(&state->pumpThread, nullptr, pump, state) != 0) {
        fprintf(stderr, "[log/mac] cannot start persistent log writer\n");
        ::close(descriptors[0]);
        ::close(descriptors[1]);
        ::close(state->flushRequestReadFd);
        ::close(state->flushRequestWriteFd);
        ::close(state->flushAckReadFd);
        ::close(state->flushAckWriteFd);
        ::close(state->fileFd);
        if (state->originalStdout >= 0) ::close(state->originalStdout);
        if (state->originalStderr >= 0) ::close(state->originalStderr);
        delete state;
        return false;
    }
    state->pumpStarted = true;

    ::fflush(nullptr);
    if (::dup2(descriptors[1], STDOUT_FILENO) < 0 ||
        ::dup2(descriptors[1], STDERR_FILENO) < 0) {
        // dup2 failures are extremely rare, but do not leave the pump waiting on
        // a descriptor that the application will never use.
        if (state->originalStdout >= 0) ::dup2(state->originalStdout, STDOUT_FILENO);
        else ::close(STDOUT_FILENO);
        if (state->originalStderr >= 0) ::dup2(state->originalStderr, STDERR_FILENO);
        else ::close(STDERR_FILENO);
        ::close(descriptors[1]);
        ::close(state->flushRequestWriteFd);
        ::pthread_join(state->pumpThread, nullptr);
        ::close(state->flushAckReadFd);
        if (state->originalStdout >= 0) ::close(state->originalStdout);
        if (state->originalStderr >= 0) ::close(state->originalStderr);
        delete state;
        return false;
    }
    ::close(descriptors[1]);
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    gState = state;

    NSDateFormatter* formatter = [[NSDateFormatter alloc] init];
    formatter.locale = [[NSLocale alloc] initWithLocaleIdentifier:@"en_US_POSIX"];
    formatter.dateFormat = @"yyyy-MM-dd HH:mm:ss Z";
    NSString* version = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"?";
    NSString* build = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"] ?: @"?";
    fprintf(stderr,
            "\n=== Pixi Native session %s pid=%d version=%s (%s) ===\n"
            "[log/mac] persistent log=%s (5 MB x 3 files)\n",
            [formatter stringFromDate:NSDate.date].UTF8String,
            NSProcessInfo.processInfo.processIdentifier,
            version.UTF8String,
            build.UTF8String,
            state->currentPath.c_str());
    return true;
}

static NSString* directoryPath() { return gDirectoryPath; }
static NSString* currentFilePath() { return gCurrentFilePath; }

static bool flush() {
    State* state = gState;
    if (!state) return false;
    ::fflush(nullptr);
    const unsigned char request = 1;
    if (!writeFully(state->flushRequestWriteFd, &request, 1)) return false;
    pollfd acknowledgement{state->flushAckReadFd, POLLIN | POLLHUP, 0};
    int result;
    do {
        result = ::poll(&acknowledgement, 1, 1500);
    } while (result < 0 && errno == EINTR);
    if (result <= 0 || !(acknowledgement.revents & POLLIN)) return false;
    unsigned char value = 0;
    return ::read(state->flushAckReadFd, &value, 1) == 1;
}

static void shutdown() {
    State* state = gState;
    if (!state) return;
    flush();

    // Replacing both pipe-backed standard descriptors closes the last writers.
    // The pump can then drain, fsync, and exit without dropping the final lines.
    if (state->originalStdout >= 0) ::dup2(state->originalStdout, STDOUT_FILENO);
    else ::close(STDOUT_FILENO);
    if (state->originalStderr >= 0) ::dup2(state->originalStderr, STDERR_FILENO);
    else ::close(STDERR_FILENO);
    if (state->pumpStarted) ::pthread_join(state->pumpThread, nullptr);
    ::close(state->flushRequestWriteFd);
    ::close(state->flushAckReadFd);
    if (state->originalStdout >= 0) ::close(state->originalStdout);
    if (state->originalStderr >= 0) ::close(state->originalStderr);
    gState = nullptr;
    delete state;
}

}  // namespace maclog
