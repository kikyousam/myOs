// On-disk file system format.
// Both the kernel and user programs use this header file.


#define ROOTINO  1   // root i-number
#define BSIZE 1024  // block size

// Disk layout:
// [ boot block | super block | log | inode blocks |
//                                          free bit map | data blocks]
//
// mkfs computes the super block and builds an initial file system. The
// super block describes the disk layout:
struct superblock {
  uint magic;        // Must be FSMAGIC
  uint size;         // Size of file system image (blocks)
  uint nblocks;      // Number of data blocks
  uint ninodes;      // Number of inodes.
  uint nlog;         // Number of log blocks
  uint logstart;     // Block number of first log block
  uint inodestart;   // Block number of first inode block
  uint bmapstart;    // Block number of first free map block
};

#define FSMAGIC 0x10203040

#define NDIRECT 11
#define NINDIRECT (BSIZE / sizeof(uint))  //一个磁盘所能存储的指针数量，1024/4=256
#define NINDIRECTDOUBLE NINDIRECT*NINDIRECT
#define MAXFILE (NDIRECT + NINDIRECT + NINDIRECTDOUBLE)

// On-disk inode structure
struct dinode {
  short type;           // File type 表示inode的对象类型：T_FILE 普通文件， T_DIR 目录， T_DEVICE 设备文件 ；若type==0,表示inode空闲 
  short major;          // Major device number (T_DEVICE only)
  short minor;          // Minor device number (T_DEVICE only)
  short nlink;          // Number of links to inode in file system 一个文件可能出现在多个目录下
  uint size;            // Size of file (bytes) 该文件的实际内容大小
  uint addrs[NDIRECT+2];   // Data block addresses addrs[i] 是该文件第 i 个数据块在磁盘上的位置，支持直接块和一级间接块（通过 addrs[NADDRS-1] 实现更大文件支持）
};

// Inodes per block. 每个磁盘块能存储的inode数量，BSIZE磁盘块大小1024字节，inode的结构体大小是64字节
#define IPB           (BSIZE / sizeof(struct dinode))

// Block containing inode i 计算 inode 号 i 所在的物理磁盘块号
#define IBLOCK(i, sb)     ((i) / IPB + sb.inodestart)

// Bitmap bits per block
#define BPB           (BSIZE*8)

// Block of free map containing bit for block b
#define BBLOCK(b, sb) ((b)/BPB + sb.bmapstart)

// Directory is a file containing a sequence of dirent structures.
#define DIRSIZ 14

struct dirent {
  ushort inum;          // inode编号（0表示空闲条目）
  char name[DIRSIZ];    // 文件名（最多14字符）
};

// 查询软链接最大递归深度
#define ELOOP 10