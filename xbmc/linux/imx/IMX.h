#pragma once
/*
 *      Copyright (C) 2005-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "threads/CriticalSection.h"
#include "threads/Event.h"
#include "threads/Thread.h"
#include "guilib/DispResource.h"
#include "utils/log.h"
#include "cores/VideoPlayer/DVDClock.h"

#include <linux/ipu.h>
#include <sys/mman.h>

#include <mutex>
#include <queue>
#include <condition_variable>
#include <algorithm>
#include <atomic>
#include <thread>
#include <map>
#include <cstring>
#include <stdlib.h>

#define DIFFRINGSIZE 120

#define FB_DEVICE "/dev/fb0"

#define Align(ptr,align)  (((unsigned int)ptr + (align) - 1)/(align)*(align))
#define Align2(ptr,align)  (((unsigned int)ptr)/(align)*(align))

#define PAGE_SIZE sysconf(_SC_PAGESIZE)

typedef uint8_t BYTE;

class CIMX;
class CDVDVideoCodecIMXBuffer;

extern CIMX g_IMX;

class CIMX : public CThread, IDispResource
{
public:
  CIMX(void);
  ~CIMX(void);

  bool          Initialize();
  void          Deinitialize();

  int           WaitVsync();
  virtual void  OnResetDisplay();

private:
  virtual void  Process();
  bool          UpdateDCIC();

  int           m_fddcic;
  bool          m_change;
  unsigned long m_counter;
  unsigned long m_counterLast;
  CEvent        m_VblankEvent;

  int           m_frameTime;
  CCriticalSection m_critSection;

  uint32_t      m_lastSyncFlag;
};

// A blocking FIFO buffer
template <typename T>
class lkFIFO
{
public:
  lkFIFO() { m_size = queue.max_size(); queue.clear(); m_abort = false; }

public:
  T pop()
  {
    std::unique_lock<std::mutex> m_lock(lkqueue);
    m_abort = false;
    while (!queue.size() && !m_abort)
      read.wait(m_lock);

    T val;
    if (!queue.empty())
    {
      val = queue.front();
      queue.pop_front();
    }
    else
    {
      val = {};
    }

    m_lock.unlock();
    write.notify_one();
    return val;
  }

  bool push(const T& item)
  {
    std::unique_lock<std::mutex> m_lock(lkqueue);
    m_abort = false;
    while (queue.size() >= m_size && !m_abort)
      write.wait(m_lock);

    if (m_abort)
      return false;

    queue.push_back(item);
    m_lock.unlock();
    read.notify_one();
    return true;
  }

  void signal()
  {
    m_abort = true;
    read.notify_one();
    write.notify_one();
  }

  void setquotasize(size_t newsize)
  {
    m_size = newsize;
    write.notify_one();
  }

  size_t getquotasize()
  {
    return m_size;
  }

  void for_each(void (*fn)(T &t), bool clear = true)
  {
    std::unique_lock<std::mutex> m_lock(lkqueue);
    std::for_each(queue.begin(), queue.end(), fn);

    if (clear)
      queue.clear();

    write.notify_one();
  }

  size_t size()
  {
    return queue.size();
  }

  void clear()
  {
    std::unique_lock<std::mutex> m_lock(lkqueue);
    queue.clear();
    write.notify_one();
  }

  bool full()  { return queue.size() >= m_size; }

private:
  std::deque<T>           queue;
  std::mutex              lkqueue;
  std::condition_variable write;
  std::condition_variable read;

  size_t                  m_size;
  volatile bool           m_abort;
};

class CIMXCircular
{
public:
  CIMXCircular(int size)
    : m_size(Align(size, PAGE_SIZE))
    , m_used(0)
    , m_stillDecoding(0)
    , m_buffer(nullptr)
  {
    m_fd = fileno(tmpfile());

    ftruncate(m_fd, m_size * 2);

    m_buffer = (BYTE*) mmap(nullptr, m_size * 2, PROT_NONE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

    mmap(m_buffer, m_size, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED, m_fd, 0);
    mmap(m_buffer + m_size, m_size, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED, m_fd, 0);
  }

  ~CIMXCircular()
  {
    munmap(m_buffer, m_size * 2);
    close(m_fd);
  }

  BYTE *pop(unsigned long count)
  {
    std::unique_lock<std::mutex> m_lock(lkqueue);
    m_abort = false;
    while (count && empty() && !m_abort)
      read.wait(m_lock);

    BYTE *ret = nullptr;
    if (m_abort || !count)
      return ret;

    ret = readfrom();
    p_read = (p_read + count) % m_size;
    m_used -= count;
    m_stillDecoding = count;

    m_lock.unlock();
    write.notify_one();
    return ret;
  }

  bool push(BYTE *data, unsigned long count)
  {
    std::unique_lock<std::mutex> m_lock(lkqueue);
    m_abort = false;
    while (free() < count && !m_abort)
      write.wait(m_lock);

    if (m_abort)
      return false;

    std::memcpy(writeto(), data, count);
    p_write = (p_write + count) % m_size;
    m_used += count;

    m_lock.unlock();
    read.notify_one();
    return true;
  }

  void reset()
  {
    std::unique_lock<std::mutex> m_lock(lkqueue);
    m_used = p_write = p_read = m_stillDecoding = 0;
  }

  void signal()
  {
    m_abort = true;
    notify();
  }

  unsigned long used()
  {
    std::unique_lock<std::mutex> m_lock(lkqueue);
    return m_used;
  }

  unsigned long free()
  {
    return m_size - m_used - m_stillDecoding;
  }

  unsigned long size()
  {
    std::unique_lock<std::mutex> m_lock(lkqueue);
    return m_size;
  }

private:
  void notify()
  {
    read.notify_one();
    write.notify_one();
  }

  BYTE *writeto()
  {
    return m_buffer + p_write;
  }

  BYTE *readfrom()
  {
    return m_buffer + p_read;
  }

  bool empty()
  {
    return p_write == p_read;
  }

  unsigned long m_size;
  unsigned long p_read;
  unsigned long p_write;
  unsigned long m_used;
  unsigned long m_stillDecoding;

  int m_fd;
  BYTE *m_buffer;

  std::mutex lkqueue;
  std::condition_variable write;
  std::condition_variable read;

  volatile bool m_abort;
};

typedef std::shared_ptr<CIMXCircular> CIMXCircularPtr;

// Generell description of a buffer used by
// the IMX context, e.g. for blitting
class CIMXBuffer
{
public:
  CIMXBuffer() : m_iRefs(0) {}

  // Shared pointer interface
  virtual void Lock() = 0;
  virtual long Release() = 0;

  int          GetFormat()  { return iFormat; }

public:
  uint32_t     iWidth;
  uint32_t     iHeight;
  int          pPhysAddr;
  uint8_t     *pVirtAddr;
  int          iFormat;
  double       m_fps;
  unsigned int nQ16ShiftWidthDivHeightRatio;

protected:
  std::atomic<long> m_iRefs;
};

struct CIMXIPUTask
{
  CIMXIPUTask(CIMXBuffer *buffer, CIMXBuffer *buffer_p = nullptr, int p = 0);
  ~CIMXIPUTask();

  // Kept for reference
  CDVDVideoCodecIMXBuffer *pb;
  CDVDVideoCodecIMXBuffer *cb;

  // The actual task
  struct ipu_task task;
  unsigned int page;
};

class CIMXFps
{
  public:
    CIMXFps() { Flush(); }
    void   Add(double pts);
    void   Flush();
    double GetFrameDuration(bool raw = false) { return m_frameDuration; }
    bool   Recalc();

  private:
    std::map<unsigned long,int>  m_hgraph;
    std::deque<double>   m_ts;
    double               m_frameDuration;
};
