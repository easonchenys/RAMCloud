/* Copyright (c) 2009 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <BackupClient.h>
#include <backuprpc.h>

#include <cstdio>

namespace RAMCloud {

enum { debug_noisy = false };

/**
 * NOTICE:  The BackupHost takes care of deleting the Net object
 * once it is no longer needed.  The host should be considered to
 * have full ownership of it and the caller should discontinue any use
 * or responbility for it.
 */
BackupHost::BackupHost(Net *netimpl)
    : net(netimpl)
{
}

BackupHost::~BackupHost()
{
    // We delete the net we were handed from the constructor so the
    // creator doesn't need to worry about it.
    delete net;
}

void
BackupHost::sendRPC(struct backup_rpc *rpc)
{
    net->Send(rpc, rpc->hdr.len);
}

void
BackupHost::recvRPC(struct backup_rpc **rpc)
{
    size_t len = net->Recv(reinterpret_cast<void**>(rpc));
    if (len != (*rpc)->hdr.len)
        printf("got %lu, expected %lu\n", len, (*rpc)->hdr.len);
    assert(len == (*rpc)->hdr.len);
    if ((*rpc)->hdr.type == BACKUP_RPC_ERROR_RESP) {
        char *m = (*rpc)->error_resp.message;
        printf("Exception on backup operation >>> %s\n", m);
        throw BackupRPCException(m);
    }
}

void
BackupHost::heartbeat()
{
    backup_rpc req;
    req.hdr.type = BACKUP_RPC_HEARTBEAT_REQ;
    req.hdr.len = static_cast<uint32_t>(BACKUP_RPC_HEARTBEAT_REQ_LEN);

    printf("Sending Heartbeat to backup\n");
    sendRPC(&req);

    backup_rpc *resp;
    recvRPC(&resp);

    printf("Heartbeat ok\n");
}

void
BackupHost::writeSegment(uint64_t segNum,
                         uint32_t offset,
                         const void *data,
                         uint32_t len)
{
    // TODO(stutsman) For the moment we don't have a choice here
    // we have to build this thing up in memory until the network
    // interface is changed so we can gather the header and the
    // data from two different places
    char reqbuf[MAX_RPC_LEN];
    backup_rpc *req = reinterpret_cast<backup_rpc *>(reqbuf);
    if (debug_noisy)
        printf("Sending Write to backup\n");

    req->hdr.type = BACKUP_RPC_WRITE_REQ;
    req->hdr.len = BACKUP_RPC_WRITE_REQ_LEN_WODATA + len;
    if (req->hdr.len > MAX_RPC_LEN)
        throw BackupRPCException("Write RPC would be too long");

    req->write_req.seg_num = segNum;
    req->write_req.off = offset;
    req->write_req.len = len;
    memcpy(&req->write_req.data[0], data, len);

    sendRPC(req);

    backup_rpc *resp;
    recvRPC(&resp);
}

void
BackupHost::commitSegment(uint64_t segNum)
{
    backup_rpc req;
    req.hdr.type = BACKUP_RPC_COMMIT_REQ;
    req.hdr.len = static_cast<uint32_t>(BACKUP_RPC_COMMIT_REQ_LEN);

    req.commit_req.seg_num = segNum;

    printf("Sending Commit to backup\n");
    sendRPC(&req);

    backup_rpc *resp;
    recvRPC(&resp);

    printf("Commit ok\n");
}

void
BackupHost::freeSegment(uint64_t segNum)
{
    backup_rpc req;
    req.hdr.type = BACKUP_RPC_FREE_REQ;
    req.hdr.len = static_cast<uint32_t>(BACKUP_RPC_FREE_REQ_LEN);

    req.free_req.seg_num = segNum;

    printf("Sending Free to backup\n");
    sendRPC(&req);

    backup_rpc *resp;
    recvRPC(&resp);

    printf("Free ok\n");
}

void
BackupHost::getSegmentList(uint64_t *list,
                           uint64_t *count)
{
    backup_rpc req;
    req.hdr.type = BACKUP_RPC_GETSEGMENTLIST_REQ;
    req.hdr.len = static_cast<uint32_t>(BACKUP_RPC_GETSEGMENTLIST_REQ_LEN);

    printf("Sending GetSegmentList to backup\n");
    sendRPC(&req);

    backup_rpc *resp;
    recvRPC(&resp);

    uint64_t *tmp_list = &resp->getsegmentlist_resp.seg_list[0];
    uint64_t tmp_count = resp->getsegmentlist_resp.seg_list_count;
    printf("Backup wants to restore %llu segments\n", tmp_count);

    if (*count < tmp_count)
        throw BackupRPCException("Provided a segment id buffer "
                                 "that was too small");
    // TODO(stutsman) we need to return this sorted and merged with
    // segs from other backups
    memcpy(list, tmp_list, tmp_count * sizeof(uint64_t));
    *count = tmp_count;

    printf("GetSegmentList ok\n");
}

size_t
BackupHost::getSegmentMetadata(uint64_t segNum,
                               RecoveryObjectMetadata *list,
                               size_t maxSize)
{
    return 0;
}

void
BackupHost::retrieveSegment(uint64_t segNum, void *buf)
{
    backup_rpc req;
    req.hdr.type = BACKUP_RPC_RETRIEVE_REQ;
    req.hdr.len = static_cast<uint32_t>(BACKUP_RPC_RETRIEVE_REQ_LEN);

    req.retrieve_req.seg_num = segNum;

    printf("Sending Retrieve to backup\n");
    sendRPC(&req);

    backup_rpc *resp;
    recvRPC(&resp);

    printf("Retrieved segment %llu of length %llu\n",
           segNum, resp->retrieve_resp.data_len);
    memcpy(buf, resp->retrieve_resp.data, resp->retrieve_resp.data_len);

    printf("Retrieve ok\n");
}

// --- BackupClient ---

MultiBackupClient::MultiBackupClient()
    : host(0)
{
}

MultiBackupClient::~MultiBackupClient()
{
    if (host)
        delete host;
}

/**
 * NOTICE:  The BackupClient takes care of deleting the Net object
 * once it is no longer needed.  The client should be considered to
 * have full ownership of it and the caller should discontinue any use
 * or responbility for it.
 */
void
MultiBackupClient::addHost(Net *net)
{
    if (host)
        throw BackupRPCException("Only one backup host currently supported");
    host = new BackupHost(net);
}

void
MultiBackupClient::heartbeat()
{
    if (host)
        host->heartbeat();
}

void
MultiBackupClient::writeSegment(uint64_t segNum,
                                uint32_t offset,
                                const void *data,
                                uint32_t len)
{
    if (host)
        host->writeSegment(segNum, offset, data, len);
}

void
MultiBackupClient::commitSegment(uint64_t segNum)
{
    if (host)
        host->commitSegment(segNum);
}

void
MultiBackupClient::freeSegment(uint64_t segNum)
{
    if (host)
        host->freeSegment(segNum);
}

void
MultiBackupClient::getSegmentList(uint64_t *list,
                                 uint64_t *count)
{
    if (host) {
        host->getSegmentList(list, count);
        return;
    }
    *count = 0;
}

size_t
MultiBackupClient::getSegmentMetadata(uint64_t segNum,
                                      RecoveryObjectMetadata *list,
                                      size_t maxSize)
{
    if (host)
        return host->getSegmentMetadata(segNum, list, maxSize);
    return 0;
}

void
MultiBackupClient::retrieveSegment(uint64_t segNum,
                                   void *buf)
{
    if (host)
        host->retrieveSegment(segNum, buf);
}

} // namespace RAMCloud
