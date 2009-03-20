/*
 * sectionfilter.c: IPTV plugin for the Video Disk Recorder
 *
 * See the README file for copyright information and how to reach the author.
 *
 */

#include "sectionfilter.h"

cIptvSectionFilter::cIptvSectionFilter(int DeviceIndex, int Index,
                                       u_short Pid, u_char Tid, u_char Mask)
: pusi_seen(0),
  feedcc(0),
  doneq(0),
  secbuf(NULL),
  secbufp(0),
  seclen(0),
  tsfeedp(0),
  pid(Pid),
  devid(DeviceIndex),
  id(Index),
  pipeName("")
{
  //debug("cIptvSectionFilter::cIptvSectionFilter(%d, %d)\n", devid, id);
  int i;

  memset(secbuf_base, '\0', sizeof(secbuf_base));
  memset(filter_value, '\0', sizeof(filter_value));
  memset(filter_mask, '\0', sizeof(filter_mask));
  memset(filter_mode, '\0', sizeof(filter_mode));
  memset(maskandmode, '\0', sizeof(maskandmode));
  memset(maskandnotmode, '\0', sizeof(maskandnotmode));

  filter_value[0] = Tid;
  filter_mask[0] = Mask;

  // Invert the filter
  for (i = 0; i < DMX_MAX_FILTER_SIZE; ++i)
      filter_value[i] ^= 0xff;

  uint8_t mask, mode, local_doneq = 0;
  for (i = 0; i < DMX_MAX_FILTER_SIZE; ++i) {
      mode = filter_mode[i];
      mask = filter_mask[i];
      maskandmode[i] = mask & mode;
      local_doneq |= maskandnotmode[i] = mask & ~mode;
      }
  doneq = local_doneq ? 1 : 0;

  struct stat sb;
  pipeName = cString::sprintf(IPTV_FILTER_FILENAME, devid, id);
  stat(pipeName, &sb);
  if (S_ISFIFO(sb.st_mode))
     unlink(pipeName);
  i = mknod(pipeName, 0644 | S_IFIFO, 0);
  ERROR_IF_RET(i < 0, "mknod()", return);

  // Create descriptors
  fifoDescriptor = open(pipeName, O_RDWR | O_NONBLOCK);
  readDescriptor = open(pipeName, O_RDONLY | O_NONBLOCK);
}

cIptvSectionFilter::~cIptvSectionFilter()
{
  //debug("cIptvSectionFilter::~cIptvSectionfilter(%d, %d)\n", devid, id);
  close(fifoDescriptor);
  close(readDescriptor);
  unlink(pipeName);
  fifoDescriptor = -1;
  readDescriptor = -1;
  secbuf = NULL;
}

int cIptvSectionFilter::GetReadDesc(void)
{
  return readDescriptor;
}

inline uint16_t cIptvSectionFilter::GetLength(const uint8_t *Data)
{
  return 3 + ((Data[1] & 0x0f) << 8) + Data[2];
}

void cIptvSectionFilter::New(void)
{
  tsfeedp = secbufp = seclen = 0;
  secbuf = secbuf_base;
}

int cIptvSectionFilter::Filter(void)
{
  uint8_t neq = 0;
  int i;

  if (secbuf) {
     for (i = 0; i < DMX_MAX_FILTER_SIZE; ++i) {
         uint8_t local_xor = filter_value[i] ^ secbuf[i];
         if (maskandmode[i] & local_xor)
            return 0;
         neq |= maskandnotmode[i] & local_xor;
         }

     if (doneq && !neq)
        return 0;

     // There is no data in the fifo, more can be written
     if (!select_single_desc(fifoDescriptor, 0, false)) {
        i = write(fifoDescriptor, secbuf, seclen);
        ERROR_IF(i < 0, "write()");
        // Update statistics
        AddSectionStatistic(i, 1);
        }
     }
  return 0;
}

inline int cIptvSectionFilter::Feed(void)
{
  if (Filter() < 0)
     return -1;
  seclen = 0;
  return 0;
}

int cIptvSectionFilter::CopyDump(const uint8_t *buf, uint8_t len)
{
  uint16_t limit, seclen_local, n;

  if (tsfeedp >= DMX_MAX_SECFEED_SIZE)
     return 0;

  if (tsfeedp + len > DMX_MAX_SECFEED_SIZE)
     len = DMX_MAX_SECFEED_SIZE - tsfeedp;

  if (len <= 0)
     return 0;

  memcpy(secbuf_base + tsfeedp, buf, len);
  tsfeedp += len;

  limit = tsfeedp;
  if (limit > DMX_MAX_SECFEED_SIZE)
     return -1; // internal error should never happen

  // Always set secbuf
  secbuf = secbuf_base + secbufp;

  for (n = 0; secbufp + 2 < limit; ++n) {
      seclen_local = GetLength(secbuf);
      if (seclen_local <= 0 || seclen_local > DMX_MAX_SECTION_SIZE ||
         seclen_local + secbufp > limit)
         return 0;
      seclen = seclen_local;
      if (pusi_seen)
         Feed();
      secbufp += seclen_local;
      secbuf += seclen_local;
      }
  return 0;
}

void cIptvSectionFilter::Process(const uint8_t* Data)
{
  if (Data[0] != TS_SYNC_BYTE)
     return;

  // Stop if not the PID this filter is looking for
  if (ts_pid(Data) != pid)
     return;

  uint8_t count = payload(Data);

  // Check if no payload or out of range
  if (count == 0)
     return;

  // Payload start
  uint8_t p = TS_SIZE - count;

  uint8_t cc = Data[3] & 0x0f;
  int ccok = ((feedcc + 1) & 0x0f) == cc;
  feedcc = cc;

  int dc_i = 0;
  if (Data[3] & 0x20) {
     // Adaption field present, check for discontinuity_indicator
     if ((Data[4] > 0) && (Data[5] & 0x80))
        dc_i = 1;
     }

  if (!ccok || dc_i) {
     // Discontinuity detected. Reset pusi_seen = 0 to
     // stop feeding of suspicious data until next PUSI=1 arrives
     pusi_seen = 0;
     New();
     }

  if (Data[1] & 0x40) {
     // PUSI=1 (is set), section boundary is here
     if (count > 1 && Data[p] < count) {
        const uint8_t *before = &Data[p + 1];
        uint8_t before_len = Data[p];
        const uint8_t *after = &before[before_len];
        uint8_t after_len = (count - 1) - before_len;
        CopyDump(before, before_len);

        // Before start of new section, set pusi_seen = 1
        pusi_seen = 1;
        New();
        CopyDump(after, after_len);
        }
     }
  else {
     // PUSI=0 (is not set), no section boundary
     CopyDump(&Data[p], count);
     }
}
