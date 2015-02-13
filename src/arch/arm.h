#ifndef CEPH_ARCH_ARM_H
#define CEPH_ARCH_ARM_H

#ifdef __cplusplus
extern "C" {
#endif

extern int ceph_arch_neon;  /* true if we have ARM NEON or ASIMD abilities */

extern int ceph_arch_arm_probe(void);

#ifdef __cplusplus
}
#endif

#endif
