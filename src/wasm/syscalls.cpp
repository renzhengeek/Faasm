#include "wasm.h"

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>

#include <WAVM/Runtime/Runtime.h>
#include <WAVM/Runtime/RuntimeData.h>
#include <WAVM/Runtime/Intrinsics.h>

using namespace WAVM;

/**
 * USEFUL NOTES
 *
 * Any syscalls here must be included in the musl port at:
 *   - musl/arch/wasm32/syscall_arch.h and
 *   - musl/arch/wasm32/libc.imports
 *
 * Any standard library functions can be found in files appropriately named
 * in musl/src. E.g. getaddrinfo is defined in musl/src/network/getaddinfo.c
 *
 * Once implemented there, they will be referenced in the wasm files, and stubbed at runtime if they
 * don't exist in here.
 *
 * Any structs passed as arguments must be re-implemented here with the following mappings
 * (respecting signed/ unsigned):
 *
 * short = I16/U16
 * long = I32/U32
 * int = I32/U32
 * char = U8
 * pointers = Uptr
 * size_t = I32
 *
 */

namespace wasm {
    DEFINE_INTRINSIC_MODULE(env)

    static const char *HOSTS_FILE = "/usr/share/faasm/net/hosts";
    static const char *RESOLV_FILE = "/usr/share/faasm/net/resolv.conf";

    // Thread-local variables to isolate bits of environment
    static thread_local I32 dummyClock = 0;
    static thread_local std::set<int> openFds;

    // ------------------------
    // I/O - supported
    // ------------------------

    void checkThreadOwnsFd(int fd) {
        if (openFds.find(fd) == openFds.end()) {
            printf("Fd not owned by this thread (%i)", fd);
            throwException(Runtime::Exception::calledUnimplementedIntrinsicType);
        }
    }

    /** Whitelist specific files to allow open and read-only */
    DEFINE_INTRINSIC_FUNCTION(env, "__syscall_open", I32, __syscall_open, I32 pathPtr, I32 flags, I32 mode) {
        printf("SYSCALL - open %i %i %i\n", pathPtr, flags, mode);

        if (mode != 0) {
            printf("Attempt to open in non-read-only mode (%i)", mode);
            throwException(Runtime::Exception::calledUnimplementedIntrinsicType);
        }

        // Get the path
        Runtime::MemoryInstance *memoryPtr = getModuleMemory();
        char *path = &Runtime::memoryRef<char>(memoryPtr, (Uptr) pathPtr);

        // Check if this is a valid path
        int fd = -1;
        if (strcmp(path, "/etc/hosts") == 0) {
            printf("Opening dummy /etc/hosts\n");
            fd = open(HOSTS_FILE, 0, 0);
        } else if (strcmp(path, "/etc/resolv.conf") == 0) {
            printf("Opening dummy /etc/resolv.conf\n");
            fd = open(RESOLV_FILE, 0, 0);
        }

        if (fd > 0) {
            openFds.insert(fd);
            return (I32) fd;
        }

        // Bomb out if not successful
        printf("Trying to open blocked path (%s) \n", path);
        throwException(Runtime::Exception::calledUnimplementedIntrinsicType);
    }

    /** Dummy fcntl implementation, many operations are irrelevant */
    DEFINE_INTRINSIC_FUNCTION(env, "__syscall_fcntl64", I32, __syscall_fcntl64,
                              I32 fd, I32 cmd, I32 c) {
        printf("SYSCALL - fcntl64 %i %i %i\n", fd, cmd, c);

        checkThreadOwnsFd(fd);

        return 0;
    }

    DEFINE_INTRINSIC_FUNCTION(env, "__syscall_read", I32, __syscall_read,
                              I32 fd, I32 bufPtr, I32 count) {
        printf("SYSCALL - read %i %i %i\n", fd, bufPtr, count);

        // Provided the thread owns the fd, we allow reading.
        checkThreadOwnsFd(fd);

        // Get the buffer etc.
        Runtime::MemoryInstance *memoryPtr = getModuleMemory();
        U8 *buf = Runtime::memoryArrayPtr<U8>(memoryPtr, (Uptr) bufPtr, (Uptr) count);

        // Do the actual read
        ssize_t bytesRead = read(fd, buf, (size_t) count);

        return (I32) bytesRead;
    }

    DEFINE_INTRINSIC_FUNCTION(env, "__syscall_close", I32, __syscall_close, I32 fd) {
        printf("SYSCALL - close %i\n", fd);

        // Provided the thread owns the fd, we allow closing.
        checkThreadOwnsFd(fd);

        openFds.erase(fd);
        close(fd);

        return 0;
    }

    struct wasm_pollfd {
        I32 fd;
        I16 events;
        I16 revents;
    };

    /** Poll is annoying as it passes an array of structs. */
    DEFINE_INTRINSIC_FUNCTION(env, "__syscall_poll", I32, __syscall_poll, I32 fdsPtr, I32 nfds, I32 timeout) {
        printf("SYSCALL - poll %i %i %i\n", fdsPtr, nfds, timeout);

        if (nfds != 1) {
            printf("Trying to poll %i fds. Only single fd supported", nfds);
            throwException(Runtime::Exception::calledUnimplementedIntrinsicType);
        }

        auto *fds = Runtime::memoryArrayPtr<pollfd>(getModuleMemory(), (Uptr) fdsPtr, 1);

        // Check this thread has permission to poll
        pollfd fd = fds[0];
        checkThreadOwnsFd(fd.fd);

        int pollRes = poll(fds, (size_t) nfds, timeout);
        return pollRes;
    }

    DEFINE_INTRINSIC_FUNCTION(env, "ioctl", I32, ioctl, I32 a, I32 b, I32 c) {
        printf("INTRINSIC - ioctl %i %i %i\n", a, b, c);

        return 0;
    }

    DEFINE_INTRINSIC_FUNCTION(env, "__syscall_ioctl", I32, __syscall_ioctl,
                              I32 fd, I32 request, I32 argPtr, I32 d, I32 e, I32 f) {
        printf("SYSCALL - ioctl %i %i %i %i %i %i\n", fd, request, argPtr, d, e, f);

        return 0;
    }

    DEFINE_INTRINSIC_FUNCTION(env, "puts", I32, puts, I32 strPtr) {
        Runtime::MemoryInstance *memoryPtr = getModuleMemory();
        char *string = &Runtime::memoryRef<char>(memoryPtr, (Uptr) strPtr);

        printf("INTRINSIC - puts %s\n", string);

        return 0;
    }

    DEFINE_INTRINSIC_FUNCTION(env, "__syscall_writev", I32, __syscall_writev, I32 fd, I32 iov, I32 iovcnt) {
        printf("SYSCALL - writev %i %i %i\n", fd, iov, iovcnt);

        struct iovec *native_iovec = new(alloca(sizeof(iovec) * iovcnt)) struct iovec[iovcnt];
        for (U32 i = 0; i < iovcnt; i++) {
            Runtime::MemoryInstance *memoryPtr = getModuleMemory();
            U32 base = Runtime::memoryRef<U32>(memoryPtr, iov + i * 8);
            U32 len = Runtime::memoryRef<U32>(memoryPtr, iov + i * 8 + 4);

            native_iovec[i].iov_base = Runtime::memoryArrayPtr<U8>(memoryPtr, base, len);
            native_iovec[i].iov_len = len;
        }

        Iptr count = writev(fileno(stdout), native_iovec, iovcnt);

        return (I32) count;
    }

    // ------------------------
    // I/O - unsupported
    // ------------------------

    DEFINE_INTRINSIC_FUNCTION(env, "__syscall_readv", I32, __syscall_readv,
                              I32 a, I32 b, I32 c) {
        printf("SYSCALL - readv %i %i %i \n", a, b, c);
        throwException(Runtime::Exception::calledUnimplementedIntrinsicType);
    }

    DEFINE_INTRINSIC_FUNCTION(env, "__syscall_llseek", I32, __syscall_llseek, I32 a, I32 b, I32 c, I32 d, I32 e) {
        printf("SYSCALL - llseek %i %i %i %i %i\n", a, b, c, d, e);
        throwException(Runtime::Exception::calledUnimplementedIntrinsicType);
    }

    DEFINE_INTRINSIC_FUNCTION(env, "__syscall_futex", I32, __syscall_futex,
                              I32 a, I32 b, I32 c, I32 d, I32 e, I32 f) {
        printf("SYSCALL - futex %i %i %i %i %i %i\n", a, b, c, d, e, f);
        throwException(Runtime::Exception::calledUnimplementedIntrinsicType);
    }

    DEFINE_INTRINSIC_FUNCTION(env, "__syscall_fstat64", I32, __syscall_fstat64, I32 a, I32 b) {
        printf("SYSCALL - fstat64 %i %i \n", a, b);
        throwException(Runtime::Exception::calledUnimplementedIntrinsicType);
    }

    DEFINE_INTRINSIC_FUNCTION(env, "__syscall_stat64", I32, __syscall_stat64, I32 a, I32 b) {
        printf("SYSCALL - stat64 %i %i \n", a, b);
        throwException(Runtime::Exception::calledUnimplementedIntrinsicType);
    }

    DEFINE_INTRINSIC_FUNCTION(env, "__syscall_access", I32, __syscall_access, I32 a, I32 b) {
        printf("SYSCALL - access %i %i \n", a, b);
        throwException(Runtime::Exception::calledUnimplementedIntrinsicType);
    }

    // ------------------------
    // Sockets/ network
    // ------------------------

    enum SocketCalls : U32 {
        sc_socket = 1,
        sc_bind,
        sc_connect,
        sc_listen,
        sc_accept,
        sc_getsockname,
        sc_getpeername,
        sc_socketpair,
        sc_send,
        sc_recv,
        sc_sendto,
        sc_recvfrom,
        sc_shutdown,
        sc_setsockopt,
        sc_getsockopt,
        sc_sendmsg,
        sc_recvmsg,
        sc_accept4,
        sc_recvmmsg,
        sc_sendmmsg,
    };

    /** Socket-related structs (see https://beej.us/guide/bgnet/html/multi/sockaddr_inman.html) */
    struct wasm_sockaddr {
        U16 sa_family;
        U8 sa_data[14];
    };

    /** Translates a wasm sockaddr into a native sockaddr */
    sockaddr getSockAddr(I32 addrPtr) {
        auto addr = &Runtime::memoryRef<wasm_sockaddr>(getModuleMemory(), (Uptr) addrPtr);

        sockaddr sa = {.sa_family = addr->sa_family};

        std::copy(addr->sa_data, addr->sa_data + 14, sa.sa_data);
        return sa;
    }

    /** Writes changes to a native sockaddr back to a wasm sockaddr. This is important in several
     * networking syscalls that receive responses and modify arguments in place */
    void setSockAddr(sockaddr nativeSockAddr, I32 addrPtr) {
        // Get native pointer to wasm address
        wasm_sockaddr *wasmAddrPtr = &Runtime::memoryRef<wasm_sockaddr>(getModuleMemory(), (Uptr) addrPtr);

        // Modify in place
        wasmAddrPtr->sa_family = nativeSockAddr.sa_family;
        std::copy(nativeSockAddr.sa_data, nativeSockAddr.sa_data + 14, wasmAddrPtr->sa_data);
    }

    void setSockLen(socklen_t nativeValue, I32 wasmPtr) {
        // Get native pointer to wasm address
        I32 *wasmAddrPtr = &Runtime::memoryRef<I32>(getModuleMemory(), (Uptr) wasmPtr);
        std::copy(&nativeValue, &nativeValue + 1, wasmAddrPtr);
    }

    struct wasm_in_addr {
        U32 s_addr;
    };

    struct wasm_sockaddr_in {
        U8 sin_family;
        U8 sin_port;
        struct wasm_in_addr sin_addr;
        U8 sin_zero[8];
    };

    /**
     * When properly isolated, functions will run in their own network namespace,
     * therefore we can be relatively comfortable passing some of the syscalls
     * straight through.
     */
    DEFINE_INTRINSIC_FUNCTION(env, "__syscall_socketcall", I32, __syscall_socketcall, I32 call, I32 argsPtr) {
        Runtime::MemoryInstance *memoryPtr = getModuleMemory();

        // NOTE
        // We don't want to support server-side socket syscalls as we expect functions
        // only to be clients

        switch (call) {
            // ----------------------------
            // Suppported
            // ----------------------------

            case (SocketCalls::sc_socket): {
                U32 *subCallArgs = Runtime::memoryArrayPtr<U32>(memoryPtr, argsPtr, 3);
                U32 domain = subCallArgs[0];
                U32 type = subCallArgs[1];
                U32 protocol = subCallArgs[2];

                printf("SYSCALL - socket %i %i %i\n", domain, type, protocol);
                int sock = (int) syscall(SYS_socket, domain, type, protocol);

                openFds.insert(sock);

                return sock;
            }

            case (SocketCalls::sc_connect): {
                U32 *subCallArgs = Runtime::memoryArrayPtr<U32>(memoryPtr, argsPtr, 3);

                I32 sockfd = subCallArgs[0];
                I32 addrPtr = subCallArgs[1];
                I32 addrLen = subCallArgs[2];

                printf("SYSCALL - connect - %i %i %i\n", sockfd, addrPtr, addrLen);

                // Allow connecting if thread owns socket
                checkThreadOwnsFd(sockfd);

                sockaddr addr = getSockAddr(addrPtr);
                int result = connect(sockfd, &addr, sizeof(sockaddr));

                return result;
            }

            case (SocketCalls::sc_recv):
            case (SocketCalls::sc_recvfrom):
            case (SocketCalls::sc_sendto):
            case (SocketCalls::sc_send): {
                int argCount = 4;

                if (call == SocketCalls::sc_sendto || call == SocketCalls::sc_recvfrom) {
                    argCount = 6;
                }

                // Pull out arguments
                U32 *subCallArgs = Runtime::memoryArrayPtr<U32>(memoryPtr, (Uptr) argsPtr, (Uptr) argCount);
                I32 sockfd = subCallArgs[0];
                Uptr bufPtr = subCallArgs[1];
                size_t bufLen = subCallArgs[2];
                I32 flags = subCallArgs[3];

                // Make sure thread owns this socket
                checkThreadOwnsFd(sockfd);

                // Set up buffer
                U8 *buf = Runtime::memoryArrayPtr<U8>(memoryPtr, bufPtr, bufLen);

                ssize_t result = 0;
                if (call == SocketCalls::sc_send) {
                    printf("SYSCALL - send %i %li %li %i \n", sockfd, bufPtr, bufLen, flags);

                    result = send(sockfd, buf, bufLen, flags);

                } else if (call == SocketCalls::sc_recv) {
                    printf("SYSCALL - recv %i %li %li %i \n", sockfd, bufPtr, bufLen, flags);

                    result = recv(sockfd, buf, bufLen, flags);

                } else {
                    I32 sockAddrPtr = subCallArgs[4];
                    sockaddr sockAddr = getSockAddr(sockAddrPtr);
                    socklen_t nativeAddrLen = sizeof(sockAddr);
                    socklen_t addrLen = subCallArgs[5];

                    if (call == SocketCalls::sc_sendto) {
                        printf("SYSCALL - sendto %i %li %li %i %i %i \n", sockfd, bufPtr, bufLen, flags, sockAddrPtr,
                               addrLen);

                        result = sendto(sockfd, buf, bufLen, flags, &sockAddr, nativeAddrLen);

                    } else {
                        // Note, addrLen here is actually a pointer
                        printf("SYSCALL - recvfrom %i %li %li %i %i %i \n", sockfd, bufPtr, bufLen, flags, sockAddrPtr,
                               addrLen);

                        // Make the native call
                        result = recvfrom(sockfd, buf, bufLen, flags, &sockAddr, &nativeAddrLen);

                        // Note, recvfrom will modify the sockaddr and addrlen in place with the details returned
                        // from the host, therefore we must also modify the original wasm object
                        setSockAddr(sockAddr, sockAddrPtr);
                        setSockLen(nativeAddrLen, addrLen);
                    }
                }

                return (I32) result;
            }

            case (SocketCalls::sc_bind): {
                U32 *subCallArgs = Runtime::memoryArrayPtr<U32>(memoryPtr, argsPtr, 3);
                I32 sockfd = subCallArgs[0];

                I32 addrPtr = subCallArgs[1];
                sockaddr addr = getSockAddr(addrPtr);

                I32 addrLen = subCallArgs[2];

                printf("SYSCALL - bind %i %i %i \n", sockfd, addrPtr, addrLen);

                // If thread owns fd, we can bind
                checkThreadOwnsFd(sockfd);
                int bindResult = bind(sockfd, &addr, sizeof(addr));

                return (I32) bindResult;
            }

            case (SocketCalls::sc_getsockname): {
                U32 *subCallArgs = Runtime::memoryArrayPtr<U32>(memoryPtr, argsPtr, 3);
                I32 sockfd = subCallArgs[0];
                I32 addrPtr = subCallArgs[1];
                I32 addrLenPtr = subCallArgs[2];

                printf("SYSCALL - getsockname %i %i %i\n", sockfd, addrPtr, addrLenPtr);

                checkThreadOwnsFd(sockfd);

                sockaddr nativeAddr = getSockAddr(addrPtr);
                socklen_t nativeAddrLen = sizeof(nativeAddr);

                int result = getsockname(sockfd, &nativeAddr, &nativeAddrLen);

                // Make sure we write any results back to the wasm objects
                setSockAddr(nativeAddr, addrPtr);
                setSockLen(nativeAddrLen, addrLenPtr);

                return result;
            }

                // ----------------------------
                // Unfinished
                // ----------------------------

            case (SocketCalls::sc_getpeername): {
                printf("SYSCALL - getpeername %i %i\n", call, argsPtr);
                return 0;
            }

            case (SocketCalls::sc_socketpair): {
                printf("SYSCALL - socketpair %i %i\n", call, argsPtr);
                return 0;
            }

            case (SocketCalls::sc_shutdown): {
                printf("SYSCALL - shutdown %i %i\n", call, argsPtr);
                return 0;
            }

            case (SocketCalls::sc_setsockopt): {
                printf("SYSCALL - setsockopt %i %i\n", call, argsPtr);
                return 0;
            }
            case (SocketCalls::sc_getsockopt): {
                printf("SYSCALL - getsockopt %i %i\n", call, argsPtr);
                return 0;
            }

            case (SocketCalls::sc_sendmsg): {
                printf("SYSCALL - sendmsg %i %i\n", call, argsPtr);
                return 0;
            }

            case (SocketCalls::sc_recvmsg): {
                printf("SYSCALL - recvmsg %i %i\n", call, argsPtr);
                return 0;
            }

            case (SocketCalls::sc_accept4): {
                printf("SYSCALL - accept4 %i %i\n", call, argsPtr);
                return 0;
            }

            case (SocketCalls::sc_recvmmsg): {
                printf("SYSCALL - recvmmsg %i %i\n", call, argsPtr);
                return 0;
            }

            case (SocketCalls::sc_sendmmsg): {
                printf("SYSCALL - sendmmsg %i %i\n", call, argsPtr);
                return 0;
            }

                // ----------------------------
                // Not supported
                // ----------------------------

            case (SocketCalls::sc_accept):
                // Server-side
                printf("SYSCALL - accept %i %i\n", call, argsPtr);

                throwException(Runtime::Exception::calledUnimplementedIntrinsicType);

            case (SocketCalls::sc_listen): {
                // Server-side
                printf("SYSCALL - listen %i %i\n", call, argsPtr);

                throwException(Runtime::Exception::calledUnimplementedIntrinsicType);
            }

            default: {
                printf("Unrecognised socketcall %i\n", call);
                return 0;
            }
        }

        return 0;
    }

    DEFINE_INTRINSIC_FUNCTION(env, "_gethostbyname", I32, _gethostbyname, I32 hostnamePtr) {
        auto hostname = &Runtime::memoryRef<char>(getModuleMemory(), (Uptr) hostnamePtr);

        printf("INTRINSIC - gethostbyname %s\n", hostname);

        return 0;
    }

    // ------------------------
    // Timing
    // ------------------------

    /** Struct to fake 32-bit time in wasm modules */
    struct wasm_timespec {
        I32 tv_sec;
        I32 tv_nsec;
    };

    DEFINE_INTRINSIC_FUNCTION(env, "__syscall_clock_gettime", I32, __syscall_clock_gettime,
                              I32 clockId, I32 resultAddress) {
        printf("INTRINSIC - clock_gettime %i %i\n", clockId, resultAddress);

        auto result = &Runtime::memoryRef<wasm_timespec>(getModuleMemory(), (Uptr) resultAddress);

        timespec actual;
        int res = clock_gettime(clockId, &actual);
        if (res != 0) {
            printf("Failed clock %i", res);
        }

        result->tv_sec = (I32) actual.tv_sec;
        result->tv_nsec = (I32) actual.tv_nsec;

        dummyClock++;

        return 0;
    }

    DEFINE_INTRINSIC_FUNCTION(env, "__syscall_gettimeofday", I32, __syscall_gettimeofday,
                              I32 a, I32 b) {
        printf("SYSCALL - gettimeofday %i %i\n", a, b);
        throwException(Runtime::Exception::calledUnimplementedIntrinsicType);
    }

    // ------------------------
    // Misc
    // ------------------------

    DEFINE_INTRINSIC_FUNCTION(env, "__unsupported_syscall", I32, __unsupported_syscall,
                              I32 a, I32 b, I32 c, I32 d, I32 e, I32 f, I32 g) {
        printf("SYSCALL - UNSUPPORTED %i %i %i %i %i %i %i \n", a, b, c, d, e, f, g);
        throwException(Runtime::Exception::calledUnimplementedIntrinsicType);
    }

    DEFINE_INTRINSIC_FUNCTION(env, "__syscall_exit_group", I32, __syscall_exit_group, I32 a) {
        printf("SYSCALL - exit_group %i\n", a);
        throwException(Runtime::Exception::calledUnimplementedIntrinsicType);
    }

    DEFINE_INTRINSIC_FUNCTION(env, "__syscall_exit", I32, __syscall_exit, I32 a) {
        printf("SYSCALL - exit %i\n", a);
        throwException(Runtime::Exception::calledUnimplementedIntrinsicType);
    }

    DEFINE_INTRINSIC_FUNCTION(env, "__syscall_gettid", I32, __syscall_gettid, I32 a) {
        printf("SYSCALL - gettid %i\n", a);
        throwException(Runtime::Exception::calledUnimplementedIntrinsicType);
    }

    DEFINE_INTRINSIC_FUNCTION(env, "__syscall_tkill", I32, __syscall_tkill, I32 a, I32 b) {
        printf("SYSCALL - tkill %i %i\n", a, b);
        throwException(Runtime::Exception::calledUnimplementedIntrinsicType);
    }

    DEFINE_INTRINSIC_FUNCTION(env, "__syscall_rt_sigprocmask", I32, __syscall_rt_sigprocmask, I32 a, I32 b, I32 c) {
        printf("SYSCALL - rt_sigprocmask %i %i %i\n", a, b, c);
        throwException(Runtime::Exception::calledUnimplementedIntrinsicType);
    }

    // ------------------------
    // Memory - supported
    // ------------------------

    /**
     * With mmap we will ignore the start address and not support file mapping
     */
    DEFINE_INTRINSIC_FUNCTION(env, "__syscall_mmap", I32, __syscall_mmap,
                              U32 addr, U32 length, U32 prot, U32 flags, I32 fd, U32 offset) {
        printf("SYSCALL - mmap %i %i %i %i %i %i\n", addr, length, prot, flags, fd, offset);

        if (fd != -1) {
            throwException(Runtime::Exception::calledUnimplementedIntrinsicType);
        }

        // Grow the module's memory accordingly
        const Uptr numPages = (Uptr(length) + IR::numBytesPerPage - 1) / IR::numBytesPerPage;

        printf("SYSCALL - mmap adding %li pages\n", numPages);
        Iptr basePageIndex = growMemory(getModuleMemory(), numPages);

        return (U32) (Uptr(basePageIndex) * IR::numBytesPerPage);
    }

    /**
     * munmap is fairly straightforward, just unmap the relevant pages
     */
    DEFINE_INTRINSIC_FUNCTION(env, "__syscall_munmap", I32, __syscall_munmap,
                              U32 addr, U32 length) {
        printf("SYSCALL - munmap %i %i\n", addr, length);

        const Uptr basePageIndex = addr / IR::numBytesPerPage;
        const Uptr numPages = (length + IR::numBytesPerPage - 1) / IR::numBytesPerPage;

        Runtime::MemoryInstance *memory = getModuleMemory();

        printf("SYSCALL - munmap %li pages\n", numPages);
        unmapMemoryPages(memory, basePageIndex, numPages);

        return 0;
    }

    /**
     * brk should be fine to run in most cases, need to check limits on the process' memory
     */
    DEFINE_INTRINSIC_FUNCTION(env, "__syscall_brk", I32, __syscall_brk, U32 addr) {
        printf("SYSCALL - brk %i\n", addr);

        // Work out how many pages needed to hit the target address
        const Uptr targetPageCount = addr / IR::numBytesPerPage;

        Runtime::MemoryInstance *memory = getModuleMemory();

        const Uptr currentPageCount = getMemoryNumPages(memory);
        const U32 currentBreak = (U32) (currentPageCount * IR::numBytesPerPage);

        Uptr maxPages = getMemoryMaxPages(memory);
        if (targetPageCount > maxPages) {
            printf("SYSCALL - brk requesting %lu pages (max %lu)\n", targetPageCount, maxPages);
        }

        if (targetPageCount <= currentPageCount) {
            printf("SYSCALL - brk with no effect\n");
            return currentBreak;
        }

        Uptr expansion = targetPageCount - currentPageCount;
        printf("SYSCALL - brk adding %lu pages\n", expansion);

        // Grow memory as required
        growMemory(memory, expansion);

        return currentBreak;
    }

    // ------------------------
    // Memory - unsupported
    // ------------------------

    DEFINE_INTRINSIC_FUNCTION(env, "__syscall_madvise", I32, __syscall_madvise,
                              U32 address, U32 numBytes, U32 advice) {
        printf("SYSCALL - madvise %i %i %i\n", address, numBytes, advice);

        throwException(Runtime::Exception::calledUnimplementedIntrinsicType);
    }

    DEFINE_INTRINSIC_FUNCTION(env, "__syscall_mremap", I32, __syscall_mremap,
                              U32 oldAddress, U32 oldNumBytes, U32 newNumBytes, U32 flags, U32 newAddress) {
        printf("SYSCALL - mremap %i %i %i %i %i\n", oldAddress, oldNumBytes, newNumBytes, flags, newAddress);

        throwException(Runtime::Exception::calledUnimplementedIntrinsicType);
    }
}