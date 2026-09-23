/*
 * Copyright (c) 2006, 2019, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2014, 2019, Red Hat Inc. All rights reserved.
 * Copyright (c) 2021, Azul Systems, Inc. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "runtime/java.hpp"
#include "runtime/os.hpp"
#include "runtime/vm_version.hpp"
#include <sys/sysctl.h>
#if defined(__FreeBSD__) || defined(__OpenBSD__)
#include <sys/auxv.h>
#include <machine/armreg.h>
#endif
#if defined(__NetBSD__)
#include <sys/param.h>
#include <sys/sysctl.h>
#include <aarch64/armreg.h>
#include <stdio.h>
#endif

#ifdef __APPLE__
static bool cpu_has(const char* optional) {
  uint32_t val;
  size_t len = sizeof(val);
  if (sysctlbyname(optional, &val, &len, NULL, 0)) {
    return false;
  }
  return val;
}

void VM_Version::get_os_cpu_info() {
  size_t sysctllen;

  // hw.optional.floatingpoint always returns 1, see
  // https://github.com/apple/darwin-xnu/blob/master/bsd/kern/kern_mib.c#L416.
  // ID_AA64PFR0_EL1 describes AdvSIMD always equals to FP field.
  assert(cpu_has("hw.optional.floatingpoint"), "should be");
  assert(cpu_has("hw.optional.neon"), "should be");
  _features = CPU_FP | CPU_ASIMD;

  // Only few features are available via sysctl, see line 614
  // https://opensource.apple.com/source/xnu/xnu-6153.141.1/bsd/kern/kern_mib.c.auto.html
  if (cpu_has("hw.optional.armv8_crc32"))     _features |= CPU_CRC32;
  if (cpu_has("hw.optional.armv8_1_atomics")) _features |= CPU_LSE;

  int cache_line_size;
  int hw_conf_cache_line[] = { CTL_HW, HW_CACHELINE };
  sysctllen = sizeof(cache_line_size);
  if (sysctl(hw_conf_cache_line, 2, &cache_line_size, &sysctllen, NULL, 0)) {
    cache_line_size = 16;
  }
  _icache_line_size = 16; // minimal line lenght CCSIDR_EL1 can hold
  _dcache_line_size = cache_line_size;

  uint64_t dczid_el0;
  __asm__ (
    "mrs %0, DCZID_EL0\n"
    : "=r"(dczid_el0)
  );
  if (!(dczid_el0 & 0x10)) {
    _zva_length = 4 << (dczid_el0 & 0xf);
  }
  int family;
  sysctllen = sizeof(family);
  if (sysctlbyname("hw.cpufamily", &family, &sysctllen, NULL, 0)) {
    family = 0;
   }
  _model = family;
  _cpu = CPU_APPLE;
}

#else // __APPLE__

// Each system answers a different way.  FreeBSD and OpenBSD report through
// the aux vector.  NetBSD has neither <sys/auxv.h> nor AT_HWCAP, and traps
// an MRS of the EL1 identification registers -- measured on NetBSD
// 11.99.7/aarch64, ID_AA64ISAR0_EL1, ID_AA64PFR0_EL1 and MIDR_EL1 all
// raise SIGILL -- but it hands the whole set over through
// sysctl machdep.cpuN.cpu_id, one node per CPU.

void VM_Version::get_os_cpu_info() {
#if defined(__FreeBSD__) || defined(__OpenBSD__)
  // Keep the flags in step with Linux's HWCAP, which is what these two
  // report through the aux vector.
  unsigned long auxv = 0;
  elf_aux_info(AT_HWCAP, &auxv, sizeof(auxv));

  STATIC_ASSERT(CPU_FP      == HWCAP_FP);
  STATIC_ASSERT(CPU_ASIMD   == HWCAP_ASIMD);
  STATIC_ASSERT(CPU_EVTSTRM == HWCAP_EVTSTRM);
  STATIC_ASSERT(CPU_AES     == HWCAP_AES);
  STATIC_ASSERT(CPU_PMULL   == HWCAP_PMULL);
  STATIC_ASSERT(CPU_SHA1    == HWCAP_SHA1);
  STATIC_ASSERT(CPU_SHA2    == HWCAP_SHA2);
  STATIC_ASSERT(CPU_CRC32   == HWCAP_CRC32);
  STATIC_ASSERT(CPU_LSE     == HWCAP_ATOMICS);
  STATIC_ASSERT(CPU_FPHP    == HWCAP_FPHP);
  STATIC_ASSERT(CPU_ASIMDHP == HWCAP_ASIMDHP);

  _features = auxv & (HWCAP_FP      | HWCAP_ASIMD   | HWCAP_EVTSTRM |
                      HWCAP_AES     | HWCAP_PMULL   | HWCAP_SHA1    |
                      HWCAP_SHA2    | HWCAP_CRC32   | HWCAP_ATOMICS |
                      HWCAP_FPHP    | HWCAP_ASIMDHP);

#ifdef __FreeBSD__
  // OpenBSD traps the read; it offers the model through sysctl hw.model
  // instead, which needs a table of implementers this port does not
  // carry, so the implementer is left unknown there.
  uint64_t midr;
  __asm__ ("mrs %0, MIDR_EL1" : "=r"(midr));
  _cpu      = (midr >> 24) & 0xff;
  _model    = (midr >> 4)  & 0xfff;
  _variant  = (midr >> 20) & 0xf;
  _revision = midr & 0xf;
#endif
#endif // __FreeBSD__ || __OpenBSD__

#if defined(__NetBSD__)
  // Take a feature only when every CPU reports it.
  int ncpu;
  int mib[] = { CTL_HW, HW_NCPU };
  size_t len = sizeof(ncpu);
  if (sysctl(mib, 2, &ncpu, &len, NULL, 0) == 0) {
    struct aarch64_sysctl_cpu_id id;
    char path[32];
    int num_fp = 0, num_asimd = 0, num_aes = 0, num_pmull = 0;
    int num_sha1 = 0, num_sha2 = 0, num_crc32 = 0, num_lse = 0;
    int seen = 0;

    for (int cpu = 0; cpu < ncpu; cpu++) {
      len = sizeof(id);
      snprintf(path, sizeof(path), "machdep.cpu%d.cpu_id", cpu);
      if (sysctlbyname(path, &id, &len, NULL, 0) < 0) {
        continue;
      }
      seen++;
      if (__SHIFTOUT(id.ac_aa64pfr0, ID_AA64PFR0_EL1_FP) == ID_AA64PFR0_EL1_FP_IMPL)
        num_fp++;
      if (__SHIFTOUT(id.ac_aa64pfr0, ID_AA64PFR0_EL1_ADVSIMD) == ID_AA64PFR0_EL1_ADV_SIMD_IMPL)
        num_asimd++;
      if (__SHIFTOUT(id.ac_aa64isar0, ID_AA64ISAR0_EL1_AES) >= ID_AA64ISAR0_EL1_AES_AES)
        num_aes++;
      if (__SHIFTOUT(id.ac_aa64isar0, ID_AA64ISAR0_EL1_AES) >= ID_AA64ISAR0_EL1_AES_PMUL)
        num_pmull++;
      if (__SHIFTOUT(id.ac_aa64isar0, ID_AA64ISAR0_EL1_SHA1) >= ID_AA64ISAR0_EL1_SHA1_SHA1CPMHSU)
        num_sha1++;
      if (__SHIFTOUT(id.ac_aa64isar0, ID_AA64ISAR0_EL1_SHA2) >= ID_AA64ISAR0_EL1_SHA2_SHA256HSU)
        num_sha2++;
      if (__SHIFTOUT(id.ac_aa64isar0, ID_AA64ISAR0_EL1_CRC32) >= ID_AA64ISAR0_EL1_CRC32_CRC32X)
        num_crc32++;
#if defined(ID_AA64ISAR0_EL1_ATOMIC_SWP)
      if (__SHIFTOUT(id.ac_aa64isar0, ID_AA64ISAR0_EL1_ATOMIC) >= ID_AA64ISAR0_EL1_ATOMIC_SWP)
        num_lse++;
#endif
    }

    if (seen == ncpu && ncpu > 0) {
      if (num_fp    == ncpu) _features |= CPU_FP;
      if (num_asimd == ncpu) _features |= CPU_ASIMD;
      if (num_aes   == ncpu) _features |= CPU_AES;
      if (num_pmull == ncpu) _features |= CPU_PMULL;
      if (num_sha1  == ncpu) _features |= CPU_SHA1;
      if (num_sha2  == ncpu) _features |= CPU_SHA2;
      if (num_crc32 == ncpu) _features |= CPU_CRC32;
      if (num_lse   == ncpu) _features |= CPU_LSE;

      _cpu      = (id.ac_midr >> 24) & 0xff;
      _model    = (id.ac_midr >> 4)  & 0xfff;
      _variant  = (id.ac_midr >> 20) & 0xf;
      _revision = id.ac_midr & 0xf;
    }
  }
#endif // __NetBSD__

  // CTR_EL0 and DCZID_EL0 are readable from EL0 on every BSD here.
  uint64_t ctr_el0;
  uint64_t dczid_el0;
  __asm__ (
    "mrs %0, CTR_EL0\n"
    "mrs %1, DCZID_EL0"
    : "=r"(ctr_el0), "=r"(dczid_el0)
  );

  _icache_line_size = (1 << (ctr_el0 & 0x0f)) * 4;
  _dcache_line_size = (1 << ((ctr_el0 >> 16) & 0x0f)) * 4;

  if (!(dczid_el0 & 0x10)) {
    _zva_length = 4 << (dczid_el0 & 0xf);
  }
}

// Rosetta is Apple's, and it reports itself through sysctl
// sysctl.proc_translated rather than through anything the CPU says.  On
// an aarch64 host there is nothing to translate, so the answer is false;
// the header only declares this for Darwin, so define it there too.
bool VM_Version::is_cpu_emulated() {
  return false;
}

#endif // __APPLE__
