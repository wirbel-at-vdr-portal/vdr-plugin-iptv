/*
 * protocolhttp.h: IPTV plugin for the Video Disk Recorder
 *
 * See the README file for copyright information and how to reach the author.
 *
 * $Id: protocolhttp.h,v 1.1 2007/09/16 09:38:01 ajhseppa Exp $
 */

#ifndef __IPTV_PROTOCOLHTTP_H
#define __IPTV_PROTOCOLHTTP_H

#include <arpa/inet.h>
#include "protocolif.h"

class cIptvProtocolHttp : public cIptvProtocolIf {
private:
  char* streamAddr;
  int streamPort;
  int socketDesc;
  unsigned char* readBuffer;
  unsigned int readBufferLen;
  struct sockaddr_in sockAddr;
  bool unicastActive;

private:
  bool OpenSocket(const int Port);
  void CloseSocket(void);
  bool Connect(void);
  bool Disconnect(void);

public:
  cIptvProtocolHttp();
  virtual ~cIptvProtocolHttp();
  virtual int Read(unsigned char* *BufferAddr);
  virtual bool Set(const char* Address, const int Port);
  virtual bool Open(void);
  virtual bool Close(void);
};

#endif // __IPTV_PROTOCOLHTTP_H
