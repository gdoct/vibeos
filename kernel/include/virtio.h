#ifndef MYOS_VIRTIO_H
#define MYOS_VIRTIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Legacy virtio PCI register offsets (BAR0, IO space). */
#define VIRTIO_PCI_HOST_FEATURES   0x00   /* R   32 */
#define VIRTIO_PCI_GUEST_FEATURES  0x04   /* W   32 */
#define VIRTIO_PCI_QUEUE_PFN       0x08   /* RW  32 (page frame number) */
#define VIRTIO_PCI_QUEUE_NUM       0x0C   /* R   16 */
#define VIRTIO_PCI_QUEUE_SEL       0x0E   /* RW  16 */
#define VIRTIO_PCI_QUEUE_NOTIFY    0x10   /* W   16 */
#define VIRTIO_PCI_STATUS          0x12   /* RW  8  */
#define VIRTIO_PCI_ISR             0x13   /* R   8  (clear-on-read) */
#define VIRTIO_PCI_CONFIG_NOMSI    0x14   /* device-specific config starts here */

/* Device status bits. */
#define VIRTIO_STATUS_ACK          0x01
#define VIRTIO_STATUS_DRIVER       0x02
#define VIRTIO_STATUS_DRIVER_OK    0x04
#define VIRTIO_STATUS_FEATURES_OK  0x08
#define VIRTIO_STATUS_FAILED       0x80

/* Descriptor flags. */
#define VIRTQ_DESC_F_NEXT          0x1
#define VIRTQ_DESC_F_WRITE         0x2   /* device writes into this buffer */

/* virtio-blk request types. */
#define VIRTIO_BLK_T_IN            0
#define VIRTIO_BLK_T_OUT           1

/* virtio-blk request status. */
#define VIRTIO_BLK_S_OK            0
#define VIRTIO_BLK_S_IOERR         1
#define VIRTIO_BLK_S_UNSUPP        2

typedef struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed)) virtq_desc_t;

typedef struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed)) virtq_used_elem_t;

typedef struct virtio_blk_req_hdr {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed)) virtio_blk_req_hdr_t;

/* PCI vendor/device range for legacy virtio. QEMU emits 0x1001 for
   virtio-blk legacy and 0x1042 in the modern range. */
#define VIRTIO_PCI_VENDOR          0x1AF4
#define VIRTIO_PCI_DEVICE_LO       0x1000
#define VIRTIO_PCI_DEVICE_HI       0x107F
#define VIRTIO_SUBSYSTEM_BLOCK     2     /* PCI subsystem id for blk */

#ifdef __cplusplus
}
#endif

#endif
