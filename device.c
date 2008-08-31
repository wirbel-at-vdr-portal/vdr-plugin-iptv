/*
 * device.c: IPTV plugin for the Video Disk Recorder
 *
 * See the README file for copyright information and how to reach the author.
 *
 */

#include "config.h"
#include "device.h"

#define IPTV_MAX_DEVICES 8

cIptvDevice * IptvDevices[IPTV_MAX_DEVICES] = { NULL };

unsigned int cIptvDevice::deviceCount = 0;

cIptvDevice::cIptvDevice(unsigned int Index)
: deviceIndex(Index),
  dvrFd(-1),
  isPacketDelivered(false),
  isOpenDvr(false),
  sidScanEnabled(false),
  pidScanEnabled(false),
  mutex()
{
  //debug("cIptvDevice::cIptvDevice(%d)\n", deviceIndex);
  isyslog("creating IPTV device %d (CardIndex=%d)", deviceIndex, CardIndex());
  tsBuffer = new cRingBufferLinear(MEGABYTE(IptvConfig.GetTsBufferSize()),
                                   (TS_SIZE * IptvConfig.GetReadBufferTsCount()),
                                   false, "IPTV");
  tsBuffer->SetTimeouts(100, 100);
  ResetBuffering();
  pUdpProtocol = new cIptvProtocolUdp();
  pHttpProtocol = new cIptvProtocolHttp();
  pFileProtocol = new cIptvProtocolFile();
  pExtProtocol = new cIptvProtocolExt();
  pIptvStreamer = new cIptvStreamer(tsBuffer, &mutex);
  pPidScanner = new cPidScanner;
  // Initialize filter pointers
  memset(secfilters, '\0', sizeof(secfilters));
  // Start section handler for iptv device
  StartSectionHandler();
  // Sid scanner must be created after the section handler
  pSidScanner = new cSidScanner;
  if (pSidScanner)
     AttachFilter(pSidScanner);
  // Check if dvr fifo exists
  struct stat sb;
  cString filename = cString::sprintf(IPTV_DVR_FILENAME, deviceIndex);
  stat(filename, &sb);
  if (S_ISFIFO(sb.st_mode)) {
     dvrFd = open(filename, O_RDWR | O_NONBLOCK);
     if (dvrFd >= 0)
        dsyslog("IPTV device %d redirecting input stream to '%s'", deviceIndex, *filename);
     }
}

cIptvDevice::~cIptvDevice()
{
  debug("cIptvDevice::~cIptvDevice(%d)\n", deviceIndex);
  // Stop section handler of iptv device
  StopSectionHandler();
  DELETE_POINTER(pIptvStreamer);
  DELETE_POINTER(pUdpProtocol);
  DELETE_POINTER(pHttpProtocol);
  DELETE_POINTER(pFileProtocol);
  DELETE_POINTER(pExtProtocol);
  DELETE_POINTER(tsBuffer);
  DELETE_POINTER(pPidScanner);
  // Detach and destroy sid filter
  if (pSidScanner) {
     Detach(pSidScanner);
     DELETE_POINTER(pSidScanner);
     }
  // Destroy all filters
  for (int i = 0; i < eMaxSecFilterCount; ++i)
      DeleteFilter(i);
  // Close dvr fifo
  if (dvrFd >= 0) {
     int fd = dvrFd;
     dvrFd = -1;
     close(fd);
     }
}

bool cIptvDevice::Initialize(unsigned int DeviceCount)
{
  debug("cIptvDevice::Initialize(): DeviceCount=%d\n", DeviceCount);
  if (DeviceCount > IPTV_MAX_DEVICES)
     DeviceCount = IPTV_MAX_DEVICES;
  for (unsigned int i = 0; i < DeviceCount; ++i)
      IptvDevices[i] = new cIptvDevice(i);
  for (unsigned int i = DeviceCount; i < IPTV_MAX_DEVICES; ++i)
      IptvDevices[i] = NULL;
  return true;
}

unsigned int cIptvDevice::Count(void)
{
  unsigned int count = 0;
  debug("cIptvDevice::Count()\n");
  for (unsigned int i = 0; i < IPTV_MAX_DEVICES; ++i) {
      if (IptvDevices[i] != NULL)
         count++;
      }
  return count;
}

cIptvDevice *cIptvDevice::GetIptvDevice(int CardIndex)
{
  //debug("cIptvDevice::GetIptvDevice(%d)\n", CardIndex);
  for (unsigned int i = 0; i < IPTV_MAX_DEVICES; ++i) {
      if ((IptvDevices[i] != NULL) && (IptvDevices[i]->CardIndex() == CardIndex)) {
         //debug("cIptvDevice::GetIptvDevice(%d): FOUND!\n", CardIndex);
         return IptvDevices[i];
         }
      }
  return NULL;
}

cString cIptvDevice::GetGeneralInformation(void)
{
  //debug("cIptvDevice::GetGeneralInformation(%d)\n", deviceIndex);
  return cString::sprintf("IPTV device: %d\nCardIndex: %d\n%s%s%sChannel: %s",
                          deviceIndex, CardIndex(),
                          pIptvStreamer ? *pIptvStreamer->GetInformation() : "",
                          pIptvStreamer ? *pIptvStreamer->GetStreamerStatistic() : "",
                          *GetBufferStatistic(),
                          *Channels.GetByNumber(cDevice::CurrentChannel())->ToText());
}

cString cIptvDevice::GetPidsInformation(void)
{
  //debug("cIptvDevice::GetPidsInformation(%d)\n", deviceIndex);
  return GetPidStatistic();
}

cString cIptvDevice::GetFiltersInformation(void)
{
  //debug("cIptvDevice::GetFiltersInformation(%d)\n", deviceIndex);
  unsigned int count = 0;
  cString info("Active section filters:\n");
  // loop through active section filters
  for (unsigned int i = 0; i < eMaxSecFilterCount; ++i) {
      if (secfilters[i]) {
         info = cString::sprintf("%sFilter %d: %s Pid=0x%02X (%s)\n", *info, i,
                                 *secfilters[i]->GetSectionStatistic(), secfilters[i]->GetPid(),
                                 id_pid(secfilters[i]->GetPid()));
         if (++count > IPTV_STATS_ACTIVE_FILTERS_COUNT)
            break;
         }
      }
  return info;
}

cString cIptvDevice::GetInformation(unsigned int Page)
{
  // generate information string
  cString info;
  switch (Page) {
    case IPTV_DEVICE_INFO_GENERAL:
         info = GetGeneralInformation();
         break;
    case IPTV_DEVICE_INFO_PIDS:
         info = GetPidsInformation();
         break;
    case IPTV_DEVICE_INFO_FILTERS:
         info = GetFiltersInformation();
         break;
    default:
         info = cString::sprintf("%s%s%s",
                                 *GetGeneralInformation(),
                                 *GetPidsInformation(),
                                 *GetFiltersInformation());
         break;
    }
  return info;
}

cString cIptvDevice::GetChannelSettings(const char *IptvParam, int *Parameter, int *SidScan, int *PidScan, cIptvProtocolIf* *Protocol)
{
  debug("cIptvDevice::GetChannelSettings(%d)\n", deviceIndex);
  char *tag = NULL;
  char *proto = NULL;
  char *loc = NULL;
  if (sscanf(IptvParam, "%a[^|]|S%dP%d|%a[^|]|%a[^|]|%u", &tag, SidScan, PidScan, &proto, &loc, Parameter) == 6) {
     cString tagstr(tag, true);
     cString protostr(proto, true);
     cString locstr(loc, true);
     // check if IPTV tag
     if (strncasecmp(*tagstr, "IPTV", 4) == 0) {
        // check if protocol is supported and update the pointer
        if (strncasecmp(*protostr, "UDP", 3) == 0)
           *Protocol = pUdpProtocol;
        else if (strncasecmp(*protostr, "HTTP", 4) == 0)
           *Protocol = pHttpProtocol;
        else if (strncasecmp(*protostr, "FILE", 4) == 0)
           *Protocol = pFileProtocol;
        else if (strncasecmp(*protostr, "EXT", 3) == 0)
           *Protocol = pExtProtocol;
        else
           return NULL;
        // return location
        return locstr;
        }
     }
  else if (sscanf(IptvParam, "%a[^|]|P%dS%d|%a[^|]|%a[^|]|%u", &tag, PidScan, SidScan, &proto, &loc, Parameter) == 6) {
     cString tagstr(tag, true);
     cString protostr(proto, true);
     cString locstr(loc, true);
     // check if IPTV tag
     if (strncasecmp(*tagstr, "IPTV", 4) == 0) {
        // check if protocol is supported and update the pointer
        if (strncasecmp(*protostr, "UDP", 3) == 0)
           *Protocol = pUdpProtocol;
        else if (strncasecmp(*protostr, "HTTP", 4) == 0)
           *Protocol = pHttpProtocol;
        else if (strncasecmp(*protostr, "FILE", 4) == 0)
           *Protocol = pFileProtocol;
        else if (strncasecmp(*protostr, "EXT", 3) == 0)
           *Protocol = pExtProtocol;
        else
           return NULL;
        // return location
        return locstr;
        }
     }
  return NULL;
}

bool cIptvDevice::ProvidesIptv(const char *Param) const
{
  debug("cIptvDevice::ProvidesIptv(%d)\n", deviceIndex);
  return (strncasecmp(Param, "IPTV", 4) == 0);
}

bool cIptvDevice::ProvidesSource(int Source) const
{
  debug("cIptvDevice::ProvidesSource(%d)\n", deviceIndex);
  return (cSource::IsPlug(Source));
}

bool cIptvDevice::ProvidesTransponder(const cChannel *Channel) const
{
  debug("cIptvDevice::ProvidesTransponder(%d)\n", deviceIndex);
  return (ProvidesSource(Channel->Source()) && ProvidesIptv(Channel->PluginParam()));
}

bool cIptvDevice::ProvidesChannel(const cChannel *Channel, int Priority, bool *NeedsDetachReceivers) const
{
  bool result = false;
  bool needsDetachReceivers = false;

  debug("cIptvDevice::ProvidesChannel(%d)\n", deviceIndex);
  if (ProvidesTransponder(Channel))
     result = true;
  if (NeedsDetachReceivers)
     *NeedsDetachReceivers = needsDetachReceivers;
  return result;
}

int cIptvDevice::NumProvidedSystems(void) const
{
  return 1;
}

bool cIptvDevice::SetChannelDevice(const cChannel *Channel, bool LiveView)
{
  int parameter, sidscan, pidscan;
  cString location;
  cIptvProtocolIf *protocol;

  debug("cIptvDevice::SetChannelDevice(%d)\n", deviceIndex);
  location = GetChannelSettings(Channel->PluginParam(), &parameter, &sidscan, &pidscan, &protocol);
  if (isempty(location)) {
     error("ERROR: Unrecognized IPTV channel settings: %s", Channel->PluginParam());
     return false;
     }
  sidScanEnabled = sidscan ? true : false;
  pidScanEnabled = pidscan ? true : false;
  if (pIptvStreamer->Set(location, parameter, deviceIndex, protocol)) {
     if (sidScanEnabled && pSidScanner && IptvConfig.GetSectionFiltering())
        pSidScanner->SetChannel(Channel);
     if (pidScanEnabled && pPidScanner)
        pPidScanner->SetChannel(Channel);
     }
  return true;
}

bool cIptvDevice::SetPid(cPidHandle *Handle, int Type, bool On)
{
  debug("cIptvDevice::SetPid(%d) Pid=%d Type=%d On=%d\n", deviceIndex, Handle->pid, Type, On);
  return true;
}

bool cIptvDevice::DeleteFilter(unsigned int Index)
{
  if ((Index < eMaxSecFilterCount) && secfilters[Index]) {
     //debug("cIptvDevice::DeleteFilter(%d) Index=%d\n", deviceIndex, Index);
     cIptvSectionFilter *tmp = secfilters[Index];
     secfilters[Index] = NULL;
     delete tmp;
     return true;
     }
  return false;
}

bool cIptvDevice::IsBlackListed(u_short Pid, u_char Tid, u_char Mask)
{
  //debug("cIptvDevice::IsBlackListed(%d) Pid=%d Tid=%02X Mask=%02X\n", deviceIndex, Pid, Tid, Mask);
  // loop through section filter table
  for (int i = 0; i < SECTION_FILTER_TABLE_SIZE; ++i) {
      int index = IptvConfig.GetDisabledFilters(i);
      // check if matches
      if ((index >= 0) && (index < SECTION_FILTER_TABLE_SIZE) &&
          (section_filter_table[index].pid == Pid) && (section_filter_table[index].tid == Tid) &&
          (section_filter_table[index].mask == Mask)) {
         //debug("cIptvDevice::IsBlackListed(%d) Found=%s\n", deviceIndex, section_filter_table[index].description);
         return true;
         }
      }
  return false;
}

int cIptvDevice::OpenFilter(u_short Pid, u_char Tid, u_char Mask)
{
  // Check if disabled by user
  if (!IptvConfig.GetSectionFiltering())
     return -1;
  // Blacklist check, refuse certain filters
  if (IsBlackListed(Pid, Tid, Mask))
     return -1;
  // Search the next free filter slot
  for (unsigned int i = 0; i < eMaxSecFilterCount; ++i) {
      if (!secfilters[i]) {
         //debug("cIptvDevice::OpenFilter(%d): Pid=%d Tid=%02X Mask=%02X Index=%d\n", deviceIndex, Pid, Tid, Mask, i);
         secfilters[i] = new cIptvSectionFilter(i, deviceIndex, Pid, Tid, Mask);
         return secfilters[i]->GetReadDesc();
         }
      }
  // No free filter slot found
  return -1;
}

void cIptvDevice::CloseFilter(int Handle)
{
  for (unsigned int i = 0; i < eMaxSecFilterCount; ++i) {
      if (secfilters[i] && (Handle == secfilters[i]->GetReadDesc())) {
         //debug("cIptvDevice::CloseFilter(%d): %d\n", deviceIndex, Handle);
         DeleteFilter(i);
         break;
         }
      }
}

bool cIptvDevice::OpenDvr(void)
{
  debug("cIptvDevice::OpenDvr(%d)\n", deviceIndex);
  mutex.Lock();
  isPacketDelivered = false;
  tsBuffer->Clear();
  mutex.Unlock();
  ResetBuffering();
  pIptvStreamer->Open();
  if (sidScanEnabled && pSidScanner && IptvConfig.GetSectionFiltering())
     pSidScanner->SetStatus(true);
  isOpenDvr = true;
  return true;
}

void cIptvDevice::CloseDvr(void)
{
  debug("cIptvDevice::CloseDvr(%d)\n", deviceIndex);
  if (sidScanEnabled && pSidScanner && IptvConfig.GetSectionFiltering())
     pSidScanner->SetStatus(false);
  if (pIptvStreamer)
     pIptvStreamer->Close();
  isOpenDvr = false;
}

bool cIptvDevice::HasLock(int TimeoutMs)
{
  //debug("cIptvDevice::HasLock(%d): %d\n", deviceIndex, TimeoutMs);
  return (!IsBuffering());
}

void cIptvDevice::ResetBuffering(void)
{
  debug("cIptvDevice::ResetBuffering(%d)\n", deviceIndex);
  // pad prefill to multiple of TS_SIZE
  tsBufferPrefill = MEGABYTE(IptvConfig.GetTsBufferSize()) *
                    IptvConfig.GetTsBufferPrefillRatio() / 100;
  tsBufferPrefill -= (tsBufferPrefill % TS_SIZE);
}

bool cIptvDevice::IsBuffering(void)
{
  //debug("cIptvDevice::IsBuffering(%d): %d\n", deviceIndex);
  if (tsBufferPrefill && tsBuffer->Available() < tsBufferPrefill)
     return true;
  else
     tsBufferPrefill = 0;
  return false;
}

bool cIptvDevice::GetTSPacket(uchar *&Data)
{
  int Count = 0;
  //debug("cIptvDevice::GetTSPacket(%d)\n", deviceIndex);
  if (!IsBuffering()) {
     if (isPacketDelivered) {
        tsBuffer->Del(TS_SIZE);
        isPacketDelivered = false;
        // Update buffer statistics
        AddBufferStatistic(TS_SIZE, tsBuffer->Available());
        }
     uchar *p = tsBuffer->Get(Count);
     if (p && Count >= TS_SIZE) {
        if (*p != TS_SYNC_BYTE) {
           for (int i = 1; i < Count; i++) {
               if (p[i] == TS_SYNC_BYTE) {
                  Count = i;
                  break;
                  }
               }
           tsBuffer->Del(Count);
           error("ERROR: skipped %d bytes to sync on TS packet\n", Count);
           return false;
           }
        isPacketDelivered = true;
        Data = p;
        // Update pid statistics 
        AddPidStatistic(ts_pid(p), payload(p));
        // Send data also to dvr fifo
        if (dvrFd >= 0)
           write(dvrFd, p, TS_SIZE);
        // Analyze incomplete streams with built-in pid analyzer
        if (pidScanEnabled && pPidScanner)
           pPidScanner->Process(p);
        // Run the data through all filters
        for (unsigned int i = 0; i < eMaxSecFilterCount; ++i) {
            if (secfilters[i])
               secfilters[i]->ProcessData(p);
            }
        return true;
        }
     }
  // Reduce cpu load by preventing busylooping
  cCondWait::SleepMs(100);
  Data = NULL;
  return true;
}
