/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "os.h"
#include "tlog.h"
#include "tlog.h"
#include "tsocket.h"
#include "tutil.h"
#include "rpcServer.h"
#include "rpcHead.h"

#define TAOS_IPv4ADDR_LEN 16
#ifndef EPOLLWAKEUP
  #define EPOLLWAKEUP (1u << 29)
#endif

typedef struct _fd_obj {
  void               *signature;
  int                 fd;       // TCP socket FD
  void *              thandle;  // handle from upper layer, like TAOS
  char                ipstr[TAOS_IPv4ADDR_LEN];
  unsigned int        ip;
  uint16_t            port;
  struct _thread_obj *pThreadObj;
  struct _fd_obj *    prev, *next;
} SFdObj;

typedef struct _thread_obj {
  pthread_t       thread;
  SFdObj *        pHead;
  pthread_mutex_t threadMutex;
  pthread_cond_t  fdReady;
  int             pollFd;
  int             numOfFds;
  int             threadId;
  char            label[12];
  void           *shandle;  // handle passed by upper layer during server initialization
  void           *(*processData)(SRecvInfo *pPacket);
} SThreadObj;

typedef struct {
  char        ip[40];
  uint16_t    port;
  char        label[12];
  int         numOfThreads;
  void *      shandle;
  SThreadObj *pThreadObj;
  pthread_t   thread;
} SServerObj;

static void taosCleanUpFdObj(SFdObj *pFdObj);
static void taosProcessTcpData(void *param);
static void taosAcceptTcpConnection(void *arg);

void *taosInitTcpServer(char *ip, uint16_t port, char *label, int numOfThreads, void *fp, void *shandle) {
  int            i;
  SServerObj    *pServerObj;
  pthread_attr_t thattr;
  SThreadObj    *pThreadObj;

  pServerObj = (SServerObj *)malloc(sizeof(SServerObj));
  strcpy(pServerObj->ip, ip);
  pServerObj->port = port;
  strcpy(pServerObj->label, label);
  pServerObj->numOfThreads = numOfThreads;

  pServerObj->pThreadObj = (SThreadObj *)malloc(sizeof(SThreadObj) * (size_t)numOfThreads);
  if (pServerObj->pThreadObj == NULL) {
    tError("TCP:%s no enough memory", label);
    return NULL;
  }
  memset(pServerObj->pThreadObj, 0, sizeof(SThreadObj) * (size_t)numOfThreads);

  pthread_attr_init(&thattr);
  pthread_attr_setdetachstate(&thattr, PTHREAD_CREATE_JOINABLE);

  pThreadObj = pServerObj->pThreadObj;
  for (i = 0; i < numOfThreads; ++i) {
    pThreadObj->processData = fp;
    strcpy(pThreadObj->label, label);
    pThreadObj->shandle = shandle;

    if (pthread_mutex_init(&(pThreadObj->threadMutex), NULL) < 0) {
      tError("%s failed to init TCP process data mutex, reason:%s", label, strerror(errno));
      return NULL;
    }

    if (pthread_cond_init(&(pThreadObj->fdReady), NULL) != 0) {
      tError("%s init TCP condition variable failed, reason:%s\n", label, strerror(errno));
      return NULL;
    }

    pThreadObj->pollFd = epoll_create(10);  // size does not matter
    if (pThreadObj->pollFd < 0) {
      tError("%s failed to create TCP epoll", label);
      return NULL;
    }

    if (pthread_create(&(pThreadObj->thread), &thattr, (void *)taosProcessTcpData, (void *)(pThreadObj)) != 0) {
      tError("%s failed to create TCP process data thread, reason:%s", label, strerror(errno));
      return NULL;
    }

    pThreadObj->threadId = i;
    pThreadObj++;
  }

  if (pthread_create(&(pServerObj->thread), &thattr, (void *)taosAcceptTcpConnection, (void *)(pServerObj)) != 0) {
    tError("%s failed to create TCP accept thread, reason:%s", label, strerror(errno));
    return NULL;
  }

  /*
    if ( pthread_create(&(pServerObj->thread), &thattr,
    (void*)taosAcceptUDConnection, (void *)(pServerObj)) != 0 ) {
      tError("%s failed to create UD accept thread, reason:%s", label,
    strerror(errno));
      return NULL;
    }
  */
  pthread_attr_destroy(&thattr);
  tTrace("%s TCP server is initialized, ip:%s port:%hu numOfThreads:%d", label, ip, port, numOfThreads);

  return (void *)pServerObj;
}

void taosCleanUpTcpServer(void *handle) {
  int         i;
  SThreadObj *pThreadObj;
  SServerObj *pServerObj = (SServerObj *)handle;

  if (pServerObj == NULL) return;

  pthread_cancel(pServerObj->thread);
  pthread_join(pServerObj->thread, NULL);

  for (i = 0; i < pServerObj->numOfThreads; ++i) {
    pThreadObj = pServerObj->pThreadObj + i;

    while (pThreadObj->pHead) {
      taosCleanUpFdObj(pThreadObj->pHead);
      pThreadObj->pHead = pThreadObj->pHead;
    }

    close(pThreadObj->pollFd);
    pthread_cancel(pThreadObj->thread);
    pthread_join(pThreadObj->thread, NULL);
    pthread_cond_destroy(&(pThreadObj->fdReady));
    pthread_mutex_destroy(&(pThreadObj->threadMutex));
  }

  tfree(pServerObj->pThreadObj);
  tTrace("TCP:%s, TCP server is cleaned up", pServerObj->label);

  tfree(pServerObj);
}

void taosCloseTcpServerConnection(void *chandle) {
  SFdObj *pFdObj = (SFdObj *)chandle;

  if (pFdObj == NULL) return;

  taosCleanUpFdObj(pFdObj);
}

int taosSendTcpServerData(uint32_t ip, uint16_t port, void *data, int len, void *chandle) {
  SFdObj *pFdObj = (SFdObj *)chandle;

  if (chandle == NULL) return -1;

  return (int)send(pFdObj->fd, data, (size_t)len, 0);
}

#define maxEvents 10

static void taosProcessTcpData(void *param) {
  SThreadObj *       pThreadObj;
  int                i, fdNum;
  SFdObj *           pFdObj;
  struct epoll_event events[maxEvents];
  SRecvInfo          recvInfo;
  pThreadObj = (SThreadObj *)param;
  SRpcHead           rpcHead;

  while (1) {
    pthread_mutex_lock(&pThreadObj->threadMutex);
    if (pThreadObj->numOfFds < 1) {
      pthread_cond_wait(&pThreadObj->fdReady, &pThreadObj->threadMutex);
    }
    pthread_mutex_unlock(&pThreadObj->threadMutex);

    fdNum = epoll_wait(pThreadObj->pollFd, events, maxEvents, -1);
    if (fdNum < 0) continue;

    for (i = 0; i < fdNum; ++i) {
      pFdObj = events[i].data.ptr;

      if (events[i].events & EPOLLERR) {
        tTrace("%s TCP thread:%d, error happened on FD", pThreadObj->label, pThreadObj->threadId);
        taosCleanUpFdObj(pFdObj);
        continue;
      }

      if (events[i].events & EPOLLHUP) {
        tTrace("%s TCP thread:%d, FD hang up", pThreadObj->label, pThreadObj->threadId);
        taosCleanUpFdObj(pFdObj);
        continue;
      }

      int32_t headLen = taosReadMsg(pFdObj->fd, &rpcHead, sizeof(SRpcHead));
      if (headLen != sizeof(SRpcHead)) {
        tError("%s read error, headLen:%d, errno:%d", pThreadObj->label, headLen, errno);
        taosCleanUpFdObj(pFdObj);
        continue;
      }

      int32_t msgLen = (int32_t)htonl((uint32_t)rpcHead.msgLen);
      char   *buffer = malloc(msgLen + tsRpcOverhead);
      if ( NULL == buffer) {
        tError("%s TCP malloc(size:%d) fail\n", pThreadObj->label, msgLen);
        taosCleanUpFdObj(pFdObj);
        continue;
      }

      char   *msg = buffer + tsRpcOverhead;
      int32_t leftLen = msgLen - headLen;
      int32_t retLen = taosReadMsg(pFdObj->fd, msg + headLen, leftLen);

      if (leftLen != retLen) {
        tError("%s read error, leftLen:%d retLen:%d", pThreadObj->label, leftLen, retLen);
        taosCleanUpFdObj(pFdObj);
        tfree(buffer);
        continue;
      }

      // tTrace("%s TCP data is received, ip:%s:%u len:%d", pTcp->label, pFdObj->ipstr, pFdObj->port, msgLen);

      memcpy(msg, &rpcHead, sizeof(SRpcHead));
      recvInfo.msg = msg;
      recvInfo.msgLen = msgLen;
      recvInfo.ip = pFdObj->ip;
      recvInfo.port = pFdObj->port;
      recvInfo.shandle = pThreadObj->shandle;
      recvInfo.thandle = pFdObj->thandle;;
      recvInfo.chandle = pFdObj;
      recvInfo.connType = RPC_CONN_TCP;

      pFdObj->thandle = (*(pThreadObj->processData))(&recvInfo);
      if (pFdObj->thandle == NULL) taosCleanUpFdObj(pFdObj);
    }
  }
}

static void taosAcceptTcpConnection(void *arg) {
  int                connFd = -1;
  struct sockaddr_in clientAddr;
  int                sockFd;
  int                threadId = 0;
  SThreadObj *       pThreadObj;
  SServerObj *       pServerObj;
  SFdObj *           pFdObj;
  struct epoll_event event;

  pServerObj = (SServerObj *)arg;

  sockFd = taosOpenTcpServerSocket(pServerObj->ip, pServerObj->port);

  if (sockFd < 0) {
    tError("%s failed to open TCP socket, ip:%s, port:%hu", pServerObj->label, pServerObj->ip, pServerObj->port);
    return;
  } else {
    tTrace("%s TCP server is ready, ip:%s, port:%hu", pServerObj->label, pServerObj->ip, pServerObj->port);
  }

  while (1) {
    socklen_t addrlen = sizeof(clientAddr);
    connFd = accept(sockFd, (struct sockaddr *)&clientAddr, &addrlen);

    if (connFd < 0) {
      tError("%s TCP accept failure, errno:%d, reason:%s", pServerObj->label, errno, strerror(errno));
      continue;
    }

    tTrace("%s TCP connection from ip:%s port:%hu", pServerObj->label, inet_ntoa(clientAddr.sin_addr),
           htons(clientAddr.sin_port));
    taosKeepTcpAlive(connFd);

    // pick up the thread to handle this connection
    pThreadObj = pServerObj->pThreadObj + threadId;

    pFdObj = (SFdObj *)malloc(sizeof(SFdObj));
    if (pFdObj == NULL) {
      tError("%s no enough resource to allocate TCP FD IDs", pServerObj->label);
      close(connFd);
      continue;
    }

    memset(pFdObj, 0, sizeof(SFdObj));
    pFdObj->fd = connFd;
    strcpy(pFdObj->ipstr, inet_ntoa(clientAddr.sin_addr));
    pFdObj->ip = clientAddr.sin_addr.s_addr;
    pFdObj->port = htons(clientAddr.sin_port);
    pFdObj->pThreadObj = pThreadObj;
    pFdObj->signature = pFdObj;

    event.events = EPOLLIN | EPOLLPRI | EPOLLWAKEUP;
    event.data.ptr = pFdObj;
    if (epoll_ctl(pThreadObj->pollFd, EPOLL_CTL_ADD, connFd, &event) < 0) {
      tError("%s failed to add TCP FD for epoll, error:%s", pServerObj->label, strerror(errno));
      tfree(pFdObj);
      close(connFd);
      continue;
    }

    // notify the data process, add into the FdObj list
    pthread_mutex_lock(&(pThreadObj->threadMutex));
    pFdObj->next = pThreadObj->pHead;
    if (pThreadObj->pHead) (pThreadObj->pHead)->prev = pFdObj;
    pThreadObj->pHead = pFdObj;
    pThreadObj->numOfFds++;
    pthread_cond_signal(&pThreadObj->fdReady);
    pthread_mutex_unlock(&(pThreadObj->threadMutex));

    tTrace("%s TCP thread:%d, a new connection from %s:%hu, FD:%p, numOfFds:%d", pServerObj->label, 
           pThreadObj->threadId, pFdObj->ipstr, pFdObj->port, pFdObj, pThreadObj->numOfFds);

    // pick up next thread for next connection
    threadId++;
    threadId = threadId % pServerObj->numOfThreads;
  }
}

static void taosCleanUpFdObj(SFdObj *pFdObj) {
  SThreadObj *pThreadObj;

  if (pFdObj == NULL) return;
  if (pFdObj->signature != pFdObj) return;

  pThreadObj = pFdObj->pThreadObj;
  if (pThreadObj == NULL) {
    tError("FdObj double clean up!!!");
    return;
  }

  epoll_ctl(pThreadObj->pollFd, EPOLL_CTL_DEL, pFdObj->fd, NULL);
  close(pFdObj->fd);

  pthread_mutex_lock(&pThreadObj->threadMutex);

  pThreadObj->numOfFds--;

  if (pThreadObj->numOfFds < 0)
    tError("%s TCP thread:%d, number of FDs shall never be negative", pThreadObj->label, pThreadObj->threadId);

  // remove from the FdObject list

  if (pFdObj->prev) {
    (pFdObj->prev)->next = pFdObj->next;
  } else {
    pThreadObj->pHead = pFdObj->next;
  }

  if (pFdObj->next) {
    (pFdObj->next)->prev = pFdObj->prev;
  }

  pthread_mutex_unlock(&pThreadObj->threadMutex);

  // notify the upper layer, so it will clean the associated context
  SRecvInfo recvInfo;
  recvInfo.msg = NULL;
  recvInfo.msgLen = 0;
  recvInfo.ip = 0;
  recvInfo.port = 0;
  recvInfo.shandle = pThreadObj->shandle;
  recvInfo.thandle = pFdObj->thandle;;
  recvInfo.chandle = NULL;
  recvInfo.connType = RPC_CONN_TCP;

  if (pFdObj->thandle) (*(pThreadObj->processData))(&recvInfo);

  tTrace("%s TCP thread:%d, FD:%p is cleaned up, numOfFds:%d", pThreadObj->label, pThreadObj->threadId,
         pFdObj, pThreadObj->numOfFds);

  memset(pFdObj, 0, sizeof(SFdObj));

  tfree(pFdObj);
}

#if 0
static void taosAcceptUDConnection(void *arg) {
  int                connFd = -1;
  int                sockFd;
  int                threadId = 0;
  SThreadObj *       pThreadObj;
  SServerObj *       pServerObj;
  SFdObj *           pFdObj;
  struct epoll_event event;

  pServerObj = (SServerObj *)arg;
  sockFd = taosOpenUDServerSocket(pServerObj->ip, pServerObj->port);

  if (sockFd < 0) {
    tError("%s failed to open UD socket, ip:%s, port:%hu", pServerObj->label, pServerObj->ip, pServerObj->port);
    return;
  } else {
    tTrace("%s UD server is ready, ip:%s, port:%hu", pServerObj->label, pServerObj->ip, pServerObj->port);
  }

  while (1) {
    connFd = accept(sockFd, NULL, NULL);

    if (connFd < 0) {
      tError("%s UD accept failure, errno:%d, reason:%s", pServerObj->label, errno, strerror(errno));
      continue;
    }

    // pick up the thread to handle this connection
    pThreadObj = pServerObj->pThreadObj + threadId;

    pFdObj = (SFdObj *)malloc(sizeof(SFdObj));
    if (pFdObj == NULL) {
      tError("%s no enough resource to allocate TCP FD IDs", pServerObj->label);
      close(connFd);
      continue;
    }

    memset(pFdObj, 0, sizeof(SFdObj));
    pFdObj->fd = connFd;
    pFdObj->pThreadObj = pThreadObj;

    event.events = EPOLLIN | EPOLLPRI | EPOLLWAKEUP;
    event.data.ptr = pFdObj;
    if (epoll_ctl(pThreadObj->pollFd, EPOLL_CTL_ADD, connFd, &event) < 0) {
      tError("%s failed to add UD FD for epoll, error:%s", pServerObj->label, strerror(errno));
      tfree(pFdObj);
      close(connFd);
      continue;
    }

    // notify the data process, add into the FdObj list
    pthread_mutex_lock(&(pThreadObj->threadMutex));
    pFdObj->next = pThreadObj->pHead;
    if (pThreadObj->pHead) (pThreadObj->pHead)->prev = pFdObj;
    pThreadObj->pHead = pFdObj;
    pThreadObj->numOfFds++;
    pthread_cond_signal(&pThreadObj->fdReady);
    pthread_mutex_unlock(&(pThreadObj->threadMutex));

    tTrace("%s UD thread:%d, a new connection, numOfFds:%d", pServerObj->label, pThreadObj->threadId,
           pThreadObj->numOfFds);

    // pick up next thread for next connection
    threadId++;
    threadId = threadId % pServerObj->numOfThreads;
  }
}
#endif

#if 0
void taosListTcpConnection(void *handle, char *buffer) {
  SServerObj *pServerObj;
  SThreadObj *pThreadObj;
  SFdObj *    pFdObj;
  int         i, numOfFds, numOfConns;
  char *      msg;

  pServerObj = (SServerObj *)handle;
  buffer[0] = 0;
  msg = buffer;
  numOfConns = 0;

  pThreadObj = pServerObj->pThreadObj;

  for (i = 0; i < pServerObj->numOfThreads; ++i) {
    numOfFds = 0;
    sprintf(msg, "TCP:%s Thread:%d number of connections:%d\n", pServerObj->label, pThreadObj->threadId,
            pThreadObj->numOfFds);
    msg = msg + strlen(msg);
    pFdObj = pThreadObj->pHead;
    while (pFdObj) {
      sprintf(msg, "   ip:%s port:%hu\n", pFdObj->ipstr, pFdObj->port);
      msg = msg + strlen(msg);
      numOfFds++;
      numOfConns++;
      pFdObj = pFdObj->next;
    }

    if (numOfFds != pThreadObj->numOfFds)
      tError("TCP:%s thread:%d BIG error, numOfFds:%d actual numOfFds:%d", pServerObj->label, pThreadObj->threadId,
             pThreadObj->numOfFds, numOfFds);

    pThreadObj++;
  }

  sprintf(msg, "TCP:%s total connections:%d\n", pServerObj->label, numOfConns);

  return;
}
#endif

