/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Copyright (C) 2018 Intel Corporation.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtCore module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qsimd_p.h"
#include "qalgorithms.h"
#include <QByteArray>
#include <stdio.h>

#ifdef Q_OS_LINUX
#  include "../testlib/3rdparty/valgrind_p.h"
#endif

#if defined(Q_OS_WIN)
#  if !defined(Q_CC_GNU)
#    include <intrin.h>
#  endif
#elif defined(Q_OS_LINUX) && (defined(Q_PROCESSOR_ARM) || defined(Q_PROCESSOR_MIPS_32))
#include "private/qcore_unix_p.h"

// the kernel header definitions for HWCAP_*
// (the ones we need/may need anyway)

// copied from <asm/hwcap.h> (ARM)
#define HWCAP_CRUNCH    1024
#define HWCAP_THUMBEE   2048
#define HWCAP_NEON      4096
#define HWCAP_VFPv3     8192
#define HWCAP_VFPv3D16  16384

// copied from <asm/hwcap.h> (ARM):
#define HWCAP2_CRC32 (1 << 4)

// copied from <asm/hwcap.h> (Aarch64)
#define HWCAP_CRC32             (1 << 7)

// copied from <linux/auxvec.h>
#define AT_HWCAP  16    /* arch dependent hints at CPU capabilities */
#define AT_HWCAP2 26    /* extension of AT_HWCAP */

#elif defined(Q_CC_GHS)
#include <INTEGRITY_types.h>
#endif

QT_BEGIN_NAMESPACE

/*
 * Use kdesdk/scripts/generate_string_table.pl to update the table below. Note
 * we remove the terminating -1 that the script adds.
 */

// begin generated
#if defined(Q_PROCESSOR_ARM)
/* Data:
 neon
 crc32
 */
static const char features_string[] =
        " neon\0"
        " crc32\0"
        "\0";
static const int features_indices[] = { 0, 6 };
#elif defined(Q_PROCESSOR_MIPS)
/* Data:
 dsp
 dspr2
*/
static const char features_string[] =
    " dsp\0"
    " dspr2\0"
    "\0";

static const int features_indices[] = {
       0,    5
};
#elif defined(Q_PROCESSOR_X86)
#  include "qsimd_x86.cpp"                  // generated by util/x86simdgen
#else
static const char features_string[] = "";
static const int features_indices[] = { };
#endif
// end generated

#if defined (Q_OS_NACL)
static inline uint detectProcessorFeatures()
{
    return 0;
}
#elif defined(Q_PROCESSOR_ARM)
static inline quint64 detectProcessorFeatures()
{
    quint64 features = 0;

#if defined(Q_OS_LINUX)
#  if defined(Q_PROCESSOR_ARM_V8) && defined(Q_PROCESSOR_ARM_64)
    features |= Q_UINT64_C(1) << CpuFeatureNEON; // NEON is always available on ARMv8 64bit.
#  endif
    int auxv = qt_safe_open("/proc/self/auxv", O_RDONLY);
    if (auxv != -1) {
        unsigned long vector[64];
        int nread;
        while (features == 0) {
            nread = qt_safe_read(auxv, (char *)vector, sizeof vector);
            if (nread <= 0) {
                // EOF or error
                break;
            }

            int max = nread / (sizeof vector[0]);
            for (int i = 0; i < max; i += 2) {
                if (vector[i] == AT_HWCAP) {
#  if defined(Q_PROCESSOR_ARM_V8) && defined(Q_PROCESSOR_ARM_64)
                    // For Aarch64:
                    if (vector[i+1] & HWCAP_CRC32)
                        features |= Q_UINT64_C(1) << CpuFeatureCRC32;
#  endif
                    // Aarch32, or ARMv7 or before:
                    if (vector[i+1] & HWCAP_NEON)
                        features |= Q_UINT64_C(1) << CpuFeatureNEON;
                }
#  if defined(Q_PROCESSOR_ARM_32)
                // For Aarch32:
                if (vector[i] == AT_HWCAP2) {
                    if (vector[i+1] & HWCAP2_CRC32)
                        features |= Q_UINT64_C(1) << CpuFeatureCRC32;
                }
#  endif
            }
        }

        qt_safe_close(auxv);
        return features;
    }
    // fall back if /proc/self/auxv wasn't found
#endif

#if defined(__ARM_NEON__)
    features |= Q_UINT64_C(1) << CpuFeatureNEON;
#endif
#if defined(__ARM_FEATURE_CRC32)
    features |= Q_UINT64_C(1) << CpuFeatureCRC32;
#endif

    return features;
}

#elif defined(Q_PROCESSOR_X86)

#ifdef Q_PROCESSOR_X86_32
# define PICreg "%%ebx"
#else
# define PICreg "%%rbx"
#endif

static int maxBasicCpuidSupported()
{
#if defined(Q_CC_EMSCRIPTEN)
    return 6; // All features supported by Emscripten
#elif defined(Q_CC_GNU)
    qregisterint tmp1;

# if Q_PROCESSOR_X86 < 5
    // check if the CPUID instruction is supported
    long cpuid_supported;
    asm ("pushf\n"
         "pop %0\n"
         "mov %0, %1\n"
         "xor $0x00200000, %0\n"
         "push %0\n"
         "popf\n"
         "pushf\n"
         "pop %0\n"
         "xor %1, %0\n" // %eax is now 0 if CPUID is not supported
         : "=a" (cpuid_supported), "=r" (tmp1)
         );
    if (!cpuid_supported)
        return 0;
# endif

    int result;
    asm ("xchg " PICreg", %1\n"
         "cpuid\n"
         "xchg " PICreg", %1\n"
        : "=&a" (result), "=&r" (tmp1)
        : "0" (0)
        : "ecx", "edx");
    return result;
#elif defined(Q_OS_WIN)
    // Use the __cpuid function; if the CPUID instruction isn't supported, it will return 0
    int info[4];
    __cpuid(info, 0);
    return info[0];
#elif defined(Q_CC_GHS)
    unsigned int info[4];
    __CPUID(0, info);
    return info[0];
#else
    return 0;
#endif
}

static void cpuidFeatures01(uint &ecx, uint &edx)
{
#if defined(Q_CC_GNU) && !defined(Q_CC_EMSCRIPTEN)
    qregisterint tmp1;
    asm ("xchg " PICreg", %2\n"
         "cpuid\n"
         "xchg " PICreg", %2\n"
        : "=&c" (ecx), "=&d" (edx), "=&r" (tmp1)
        : "a" (1));
#elif defined(Q_OS_WIN)
    int info[4];
    __cpuid(info, 1);
    ecx = info[2];
    edx = info[3];
#elif defined(Q_CC_GHS)
    unsigned int info[4];
    __CPUID(1, info);
    ecx = info[2];
    edx = info[3];
#else
    Q_UNUSED(ecx);
    Q_UNUSED(edx);
#endif
}

#ifdef Q_OS_WIN
inline void __cpuidex(int info[4], int, __int64) { memset(info, 0, 4*sizeof(int));}
#endif

static void cpuidFeatures07_00(uint &ebx, uint &ecx, uint &edx)
{
#if defined(Q_CC_GNU) && !defined(Q_CC_EMSCRIPTEN)
    qregisteruint rbx; // in case it's 64-bit
    qregisteruint rcx = 0;
    qregisteruint rdx = 0;
    asm ("xchg " PICreg", %0\n"
         "cpuid\n"
         "xchg " PICreg", %0\n"
        : "=&r" (rbx), "+&c" (rcx), "+&d" (rdx)
        : "a" (7));
    ebx = rbx;
    ecx = rcx;
    edx = rdx;
#elif defined(Q_OS_WIN)
    int info[4];
    __cpuidex(info, 7, 0);
    ebx = info[1];
    ecx = info[2];
    edx = info[3];
#elif defined(Q_CC_GHS)
    unsigned int info[4];
    __CPUIDEX(7, 0, info);
    ebx = info[1];
    ecx = info[2];
    edx = info[3];
#else
    Q_UNUSED(ebx);
    Q_UNUSED(ecx);
    Q_UNUSED(edx);
#endif
}

#if defined(Q_OS_WIN) && !(defined(Q_CC_GNU) || defined(Q_CC_GHS))
// fallback overload in case this intrinsic does not exist: unsigned __int64 _xgetbv(unsigned int);
inline quint64 _xgetbv(__int64) { return 0; }
#endif
static void xgetbv(uint in, uint &eax, uint &edx)
{
#if (defined(Q_CC_GNU) && !defined(Q_CC_EMSCRIPTEN)) || defined(Q_CC_GHS)
    asm (".byte 0x0F, 0x01, 0xD0" // xgetbv instruction
        : "=a" (eax), "=d" (edx)
        : "c" (in));
#elif defined(Q_OS_WIN)
    quint64 result = _xgetbv(in);
    eax = result;
    edx = result >> 32;
#else
    Q_UNUSED(in);
    Q_UNUSED(eax);
    Q_UNUSED(edx);
#endif
}

static quint64 detectProcessorFeatures()
{
    // Flags from the CR0 / XCR0 state register
    enum XCR0Flags {
        X87             = 1 << 0,
        XMM0_15         = 1 << 1,
        YMM0_15Hi128    = 1 << 2,
        BNDRegs         = 1 << 3,
        BNDCSR          = 1 << 4,
        OpMask          = 1 << 5,
        ZMM0_15Hi256    = 1 << 6,
        ZMM16_31        = 1 << 7,

        SSEState        = XMM0_15,
        AVXState        = XMM0_15 | YMM0_15Hi128,
        AVX512State     = AVXState | OpMask | ZMM0_15Hi256 | ZMM16_31
    };
    static const quint64 AllAVX2 = CpuFeatureAVX2 | AllAVX512;
    static const quint64 AllAVX = CpuFeatureAVX | AllAVX2;

    quint64 features = 0;
    int cpuidLevel = maxBasicCpuidSupported();
#if Q_PROCESSOR_X86 < 5
    if (cpuidLevel < 1)
        return 0;
#else
    Q_ASSERT(cpuidLevel >= 1);
#endif

    uint results[X86CpuidMaxLeaf] = {};
    cpuidFeatures01(results[Leaf1ECX], results[Leaf1EDX]);
    if (cpuidLevel >= 7)
        cpuidFeatures07_00(results[Leaf7_0EBX], results[Leaf7_0ECX], results[Leaf7_0EDX]);

    // populate our feature list
    for (uint i = 0; i < sizeof(x86_locators) / sizeof(x86_locators[0]); ++i) {
        uint word = x86_locators[i] / 32;
        uint bit = 1U << (x86_locators[i] % 32);
        quint64 feature = Q_UINT64_C(1) << (i + 1);
        if (results[word] & bit)
            features |= feature;
    }

    // now check the AVX state
    uint xgetbvA = 0, xgetbvD = 0;
    if (results[Leaf1ECX] & (1u << 27)) {
        // XGETBV enabled
        xgetbv(0, xgetbvA, xgetbvD);
    }

    if ((xgetbvA & AVXState) != AVXState) {
        // support for YMM registers is disabled, disable all AVX
        features &= ~AllAVX;
    } else if ((xgetbvA & AVX512State) != AVX512State) {
        // support for ZMM registers or mask registers is disabled, disable all AVX512
        features &= ~AllAVX512;
    }

    return features;
}

#elif defined(Q_PROCESSOR_MIPS_32)

#if defined(Q_OS_LINUX)
//
// Do not use QByteArray: it could use SIMD instructions itself at
// some point, thus creating a recursive dependency. Instead, use a
// QSimpleBuffer, which has the bare minimum needed to use memory
// dynamically and read lines from /proc/cpuinfo of arbitrary sizes.
//
struct QSimpleBuffer {
    static const int chunk_size = 256;
    char *data;
    unsigned alloc;
    unsigned size;

    QSimpleBuffer(): data(0), alloc(0), size(0) {}
    ~QSimpleBuffer() { ::free(data); }

    void resize(unsigned newsize) {
        if (newsize > alloc) {
            unsigned newalloc = chunk_size * ((newsize / chunk_size) + 1);
            if (newalloc < newsize) newalloc = newsize;
            if (newalloc != alloc) {
                data = static_cast<char*>(::realloc(data, newalloc));
                alloc = newalloc;
            }
        }
        size = newsize;
    }
    void append(const QSimpleBuffer &other, unsigned appendsize) {
        unsigned oldsize = size;
        resize(oldsize + appendsize);
        ::memcpy(data + oldsize, other.data, appendsize);
    }
    void popleft(unsigned amount) {
        if (amount >= size) return resize(0);
        size -= amount;
        ::memmove(data, data + amount, size);
    }
    char* cString() {
        if (!alloc) resize(1);
        return (data[size] = '\0', data);
    }
};

//
// Uses a scratch "buffer" (which must be used for all reads done in the
// same file descriptor) to read chunks of data from a file, to read
// one line at a time. Lines include the trailing newline character ('\n').
// On EOF, line.size is zero.
//
static void bufReadLine(int fd, QSimpleBuffer &line, QSimpleBuffer &buffer)
{
    for (;;) {
        char *newline = static_cast<char*>(::memchr(buffer.data, '\n', buffer.size));
        if (newline) {
            unsigned piece_size = newline - buffer.data + 1;
            line.append(buffer, piece_size);
            buffer.popleft(piece_size);
            line.resize(line.size - 1);
            return;
        }
        if (buffer.size + QSimpleBuffer::chunk_size > buffer.alloc) {
            int oldsize = buffer.size;
            buffer.resize(buffer.size + QSimpleBuffer::chunk_size);
            buffer.size = oldsize;
        }
        ssize_t read_bytes = ::qt_safe_read(fd, buffer.data + buffer.size, QSimpleBuffer::chunk_size);
        if (read_bytes > 0) buffer.size += read_bytes;
        else return;
    }
}

//
// Checks if any line with a given prefix from /proc/cpuinfo contains
// a certain string, surrounded by spaces.
//
static bool procCpuinfoContains(const char *prefix, const char *string)
{
    int cpuinfo_fd = ::qt_safe_open("/proc/cpuinfo", O_RDONLY);
    if (cpuinfo_fd == -1)
        return false;

    unsigned string_len = ::strlen(string);
    unsigned prefix_len = ::strlen(prefix);
    QSimpleBuffer line, buffer;
    bool present = false;
    do {
        line.resize(0);
        bufReadLine(cpuinfo_fd, line, buffer);
        char *colon = static_cast<char*>(::memchr(line.data, ':', line.size));
        if (colon && line.size > prefix_len + string_len) {
            if (!::strncmp(prefix, line.data, prefix_len)) {
                // prefix matches, next character must be ':' or space
                if (line.data[prefix_len] == ':' || ::isspace(line.data[prefix_len])) {
                    // Does it contain the string?
                    char *found = ::strstr(line.cString(), string);
                    if (found && ::isspace(found[-1]) &&
                            (::isspace(found[string_len]) || found[string_len] == '\0')) {
                        present = true;
                        break;
                    }
                }
            }
        }
    } while (line.size);

    ::qt_safe_close(cpuinfo_fd);
    return present;
}
#endif

static inline quint64 detectProcessorFeatures()
{
    // NOTE: MIPS 74K cores are the only ones supporting DSPr2.
    quint64 flags = 0;

#if defined __mips_dsp
    flags |= Q_UINT64_C(1) << CpuFeatureDSP;
#  if defined __mips_dsp_rev && __mips_dsp_rev >= 2
    flags |= Q_UINT64_C(1) << CpuFeatureDSPR2;
#  elif defined(Q_OS_LINUX)
    if (procCpuinfoContains("cpu model", "MIPS 74Kc") || procCpuinfoContains("cpu model", "MIPS 74Kf"))
        flags |= Q_UINT64_C(1) << CpuFeatureDSPR2;
#  endif
#elif defined(Q_OS_LINUX)
    if (procCpuinfoContains("ASEs implemented", "dsp")) {
        flags |= Q_UINT64_C(1) << CpuFeatureDSP;
        if (procCpuinfoContains("cpu model", "MIPS 74Kc") || procCpuinfoContains("cpu model", "MIPS 74Kf"))
            flags |= Q_UINT64_C(1) << CpuFeatureDSPR2;
    }
#endif

    return flags;
}

#else
static inline uint detectProcessorFeatures()
{
    return 0;
}
#endif

static const int features_count = (sizeof features_indices) / (sizeof features_indices[0]);

// record what CPU features were enabled by default in this Qt build
static const quint64 minFeature = qCompilerCpuFeatures;

#ifdef Q_ATOMIC_INT64_IS_SUPPORTED
Q_CORE_EXPORT QBasicAtomicInteger<quint64> qt_cpu_features[1] = { Q_BASIC_ATOMIC_INITIALIZER(0) };
#else
Q_CORE_EXPORT QBasicAtomicInteger<unsigned> qt_cpu_features[2] = { Q_BASIC_ATOMIC_INITIALIZER(0), Q_BASIC_ATOMIC_INITIALIZER(0) };
#endif

quint64 qDetectCpuFeatures()
{
    quint64 f = detectProcessorFeatures();
    QByteArray disable = qgetenv("QT_NO_CPU_FEATURE");
    if (!disable.isEmpty()) {
        disable.prepend(' ');
        for (int i = 0; i < features_count; ++i) {
            if (disable.contains(features_string + features_indices[i]))
                f &= ~(Q_UINT64_C(1) << i);
        }
    }

#ifdef RUNNING_ON_VALGRIND
    bool runningOnValgrind = RUNNING_ON_VALGRIND;
#else
    bool runningOnValgrind = false;
#endif
    if (Q_UNLIKELY(!runningOnValgrind && minFeature != 0 && (f & minFeature) != minFeature)) {
        quint64 missing = minFeature & ~f;
        fprintf(stderr, "Incompatible processor. This Qt build requires the following features:\n   ");
        for (int i = 0; i < features_count; ++i) {
            if (missing & (Q_UINT64_C(1) << i))
                fprintf(stderr, "%s", features_string + features_indices[i]);
        }
        fprintf(stderr, "\n");
        fflush(stderr);
        qFatal("Aborted. Incompatible processor: missing feature 0x%llx -%s.", missing,
               features_string + features_indices[qCountTrailingZeroBits(missing)]);
    }

    qt_cpu_features[0].store(f | quint32(QSimdInitialized));
#ifndef Q_ATOMIC_INT64_IS_SUPPORTED
    qt_cpu_features[1].store(f >> 32);
#endif
    return f;
}

void qDumpCPUFeatures()
{
    quint64 features = qCpuFeatures() & ~quint64(QSimdInitialized);
    printf("Processor features: ");
    for (int i = 0; i < features_count; ++i) {
        if (features & (Q_UINT64_C(1) << i))
            printf("%s%s", features_string + features_indices[i],
                   minFeature & (Q_UINT64_C(1) << i) ? "[required]" : "");
    }
    if ((features = (qCompilerCpuFeatures & ~features))) {
        printf("\n!!!!!!!!!!!!!!!!!!!!\n!!! Missing required features:");
        for (int i = 0; i < features_count; ++i) {
            if (features & (Q_UINT64_C(1) << i))
                printf("%s", features_string + features_indices[i]);
        }
        printf("\n!!! Applications will likely crash with \"Invalid Instruction\"\n!!!!!!!!!!!!!!!!!!!!");
    }
    puts("");
}

QT_END_NAMESPACE
