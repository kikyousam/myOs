// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKET 13 // 使用质数作为桶数量

struct {
  struct buf buf[NBUF];
  struct {
    struct spinlock lock;
    struct buf *head;
  } bucket[NBUCKET];
} bcache;

void binit(void) {
  struct buf *b;
  char lockname[16];

  // 初始化每个桶的锁和链表
  for (int i = 0; i < NBUCKET; i++) {
    snprintf(lockname, sizeof(lockname), "bcache.bucket.%d", i);
    initlock(&bcache.bucket[i].lock, lockname);
    bcache.bucket[i].head = 0; // 初始化为空链表
  }

  // 初始化所有缓冲区，全部放入桶0
  for (b = bcache.buf; b < bcache.buf + NBUF; b++) {
    initsleeplock(&b->lock, "buffer");
    b->lastuse = 0;
    b->refcnt = 0;
    b->valid = 0;
    
    // 添加到桶0
    b->next = bcache.bucket[0].head;
    bcache.bucket[0].head = b;
  }
}

// 在全局范围内查找LRU缓冲区（必须持有所有桶锁）
static struct buf* find_lru(void) {
  struct buf *lrub = 0;
  
  // 在所有桶中查找refcnt==0且lastuse最小的缓冲区
  for (int i = 0; i < NBUCKET; i++) {
    struct buf *b = bcache.bucket[i].head;
    while (b) {
      if (b->refcnt == 0) {
        // 找到更旧的未使用缓冲区
        if (lrub == 0 || b->lastuse < lrub->lastuse) {
          lrub = b;
        }
      }
      b = b->next;
    }
  }
  
  return lrub;
}

// 从桶中移除缓冲区（必须持有桶锁）
static void remove_from_bucket(struct buf *b, int bucket_idx) {
  struct buf *prev = 0;
  struct buf *curr = bcache.bucket[bucket_idx].head;
  
  // 查找缓冲区在链表中的位置
  while (curr && curr != b) {
    prev = curr;
    curr = curr->next;
  }
  
  if (!curr) {
    panic("remove_from_bucket: buffer not found");
  }
  
  // 从链表中移除
  if (prev) {
    prev->next = b->next;
  } else {
    bcache.bucket[bucket_idx].head = b->next;
  }
  
  b->next = 0;
}

// 添加缓冲区到桶（必须持有桶锁）
static void add_to_bucket(struct buf *b, int bucket_idx) {
  b->next = bcache.bucket[bucket_idx].head;
  bcache.bucket[bucket_idx].head = b;
}

// 查找缓冲区所在桶的索引
static int find_bucket(struct buf *b) {
  for (int i = 0; i < NBUCKET; i++) {
    struct buf *curr = bcache.bucket[i].head;
    while (curr) {
      if (curr == b) {
        return i;
      }
      curr = curr->next;
    }
  }
  panic("find_bucket: buffer not found in any bucket");
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
struct buf* bget(uint dev, uint blockno) {
  struct buf *b;
  int bucket_idx = blockno % NBUCKET;
  
  // 尝试在目标桶中查找
  acquire(&bcache.bucket[bucket_idx].lock);
  
  // 在目标桶中查找缓冲区
  for (b = bcache.bucket[bucket_idx].head; b != 0; b = b->next) {
    if (b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      release(&bcache.bucket[bucket_idx].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  
  // 未找到，准备查找其他桶
  release(&bcache.bucket[bucket_idx].lock);
  
  // 按顺序获取所有桶锁（避免死锁）
  for (int i = 0; i < NBUCKET; i++) {
    acquire(&bcache.bucket[i].lock);
  }
  
  // 重新检查目标桶（可能在等待锁时被添加）
  for (b = bcache.bucket[bucket_idx].head; b != 0; b = b->next) {
    if (b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      // 释放所有桶锁
      for (int i = NBUCKET-1; i >= 0; i--) {
        release(&bcache.bucket[i].lock);
      }
      acquiresleep(&b->lock);
      return b;
    }
  }
  
  // 仍然未找到，需要分配新缓冲区
  b = find_lru();
  if (!b) {
    panic("bget: no buffers");
  }
  
  // 如果缓冲区有效，需要写回磁盘
  if (b->valid) {
    // 注意：这里我们只处理缓存，实际写回由上层处理
  }
  
  // 获取缓冲区当前所在的桶
  int orig_bucket = find_bucket(b);
  
  // 如果缓冲区不在目标桶，需要移动
  if (orig_bucket != bucket_idx) {
    remove_from_bucket(b, orig_bucket);
    add_to_bucket(b, bucket_idx);
  }
  
  // 设置新缓冲区属性
  b->dev = dev;
  b->blockno = blockno;
  b->valid = 0; // 数据无效，需要从磁盘读取
  b->refcnt = 1;
  
  // 释放所有桶锁（逆序释放）
  for (int i = NBUCKET-1; i >= 0; i--) {
    release(&bcache.bucket[i].lock);
  }
  
  acquiresleep(&b->lock);
  return b;
}

// Return a locked buf with the contents of the indicated block.
struct buf* bread(uint dev, uint blockno) {
  struct buf *b;
  
  b = bget(dev, blockno);
  if (!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk. Must be locked.
void bwrite(struct buf *b) {
  if (!holdingsleep(&b->lock)) {
    panic("bwrite");
  }
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
void brelse(struct buf *b) {
  if (!holdingsleep(&b->lock)) {
    panic("brelse");
  }
  
  releasesleep(&b->lock);
  
  int bucket_idx = b->blockno % NBUCKET;
  acquire(&bcache.bucket[bucket_idx].lock);
  b->refcnt--;
  
  if (b->refcnt == 0) {
    // 更新最后使用时间
    b->lastuse = ticks;
  }
  
  release(&bcache.bucket[bucket_idx].lock);
}

void bpin(struct buf *b) {
  int bucket_idx = b->blockno % NBUCKET;
  acquire(&bcache.bucket[bucket_idx].lock);
  b->refcnt++;
  release(&bcache.bucket[bucket_idx].lock);
}

void bunpin(struct buf *b) {
  int bucket_idx = b->blockno % NBUCKET;
  acquire(&bcache.bucket[bucket_idx].lock);
  b->refcnt--;
  release(&bcache.bucket[bucket_idx].lock);
}