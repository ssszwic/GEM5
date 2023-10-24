/*
 * Copyright (c) 2018 Inria
 * Copyright (c) 2012-2013, 2015 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2005 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file
 * FDIP Prefetcher template instantiations.
 */

#include "mem/cache/prefetch/fdip.hh"

#include <cassert>

#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/random.hh"
#include "base/trace.hh"
#include "debug/HWIPrefetch.hh"
#include "debug/HWPrefetch.hh"
#include "mem/cache/base.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"
#include "mem/cache/replacement_policies/base.hh"
#include "params/FDIPPrefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

FDIPPrefetcher::FDIPPrefetcher(const FDIPPrefetcherParams &p)
  : Base(p),
    tags(p.tags),
    numPIQEntry(p.numPIQEntry),
    max_used_time(p.max_used_time),
    piq_latency(p.piq_latency)
{
    tags->tagsInit();
}

void
FDIPPrefetcher::insertPrefetchReq(PacketPtr pkt)
{
    if (pkt->cmd == MemCmd::PrefetchFlushReq)
    {
        // flush piq because fsq flushed
        flushPIQ();
        delete pkt;
        return;
    }
    prefetchStats.piqReceive++;
    assert(piq.size() <= numPIQEntry);
    // filter
    bool delate = filter(pkt);
    if (delate)
    {
        delete pkt;
        return;
    }
    prefetchStats.piqInsert++;
    PIQEntry entry = PIQEntry(pkt, curTick() + cyclesToTicks(piq_latency));
    DPRINTF(HWIPrefetch, "insert prefetch req %#lx\n",
                        pkt->getBlockAddr(blkSize));
    if (piq.size() == numPIQEntry) {
        DPRINTF(HWIPrefetch, "flush prefetch req %#lx because piq is full.\n",
                                    piq.front().pkt->getBlockAddr(blkSize));
        prefetchStats.piqFull++;
        delete piq.front().pkt;
        piq.pop_front();
    }
    piq.push_back(entry);
}

void
FDIPPrefetcher::filterReq(PacketPtr pkt)
{
    for (auto iter = piq.begin(); iter != piq.end(); iter++)
    {
        if (iter->pkt->getBlockAddr(blkSize) == pkt->getBlockAddr(blkSize))
        {
            DPRINTF(HWIPrefetch, "flush prefetch req %#lx "
                    "because req is too late.\n",
                    iter->pkt->getBlockAddr(blkSize));
            prefetchStats.piqLate++;
            delete iter->pkt;
            piq.erase(iter);
            break;
        }
    }
}

CacheBlk*
FDIPPrefetcher::insertPrefetchData(PacketPtr pkt)
{
    // should not fina blk in tags, just to makr sure
    CacheBlk *blk = tags->findBlock(pkt->getAddr(), pkt->isSecure());
    assert(!blk);
    // evict_blks is not used because it does not need to be written back
    std::vector<CacheBlk*> evict_blks;
    CacheBlk *victim = tags->findVictim(pkt->getAddr(), pkt->isSecure(), 0,
                                        evict_blks);
    if (victim->isValid())
    {
        assert(evict_blks.size() == 1);
        DPRINTF(HWIPrefetch, "flush prefetch blk %#lx "
            "because pf is full.\n", evict_blks[0]->getTag());
        if (evict_blks[0]->used_time == 0) {
            prefetchUnused();
        }
        tags->invalidate(victim);
    }
    assert(pkt->hasData());
    tags->insertBlock(pkt, victim);
    pkt->writeDataToBlock(victim->data, blkSize);
    victim->setWhenReady(clockEdge(Cycles(1)));
    victim->setCoherenceBits(CacheBlk::ReadableBit);
    DPRINTF(HWIPrefetch, "insert prefetch blk %#lx\n",
            pkt->getBlockAddr(blkSize));
    return victim;
}

PacketPtr
FDIPPrefetcher::getPacket()
{
    PacketPtr pkt = nullptr;
    if (!piq.empty())
    {
        pkt = piq.front().pkt;
        DPRINTF(HWIPrefetch, "issue prefetch req %#lx\n",
                            pkt->getBlockAddr(blkSize));
        prefetchStats.pfIssued++;
        piq.pop_front();
    }
    return pkt;
}

Tick
FDIPPrefetcher::nextPrefetchReadyTime() const
{
    return piq.empty() ? MaxTick : piq.front().tick;
}


CacheBlk*
FDIPPrefetcher::findPrefetchBuffer(const PacketPtr pkt,
                                Cycles &lat, bool &move)
{
    move = false;
    CacheBlk* blk = tags->accessBlock(pkt, lat);
    if (blk)
    {
        if (blk->used_time == 0) {
            prefetchUsed();
        }
        blk->used_time++;
        move = (blk->used_time == max_used_time) ? true : false;
    }
    return blk;
}


void
FDIPPrefetcher::invalidatePrefetchBlk(CacheBlk* blk)
{
    tags->invalidate(blk);
}


bool
FDIPPrefetcher::filter(const PacketPtr pkt)
{
    bool delate = true;
    Addr blk_addr = pkt->getBlockAddr(blkSize);
    if (findPIQ(blk_addr)) {
        DPRINTF(HWPrefetch, "Prefetch %#x has hit in piq, "
                "dropped.\n", blk_addr);
        reqHitInPIQ();
    } else if (tags->findBlock(blk_addr, pkt->isSecure())) {
        DPRINTF(HWPrefetch, "Prefetch %#x has hit in PF, "
                "dropped.\n", blk_addr);
        reqHitInPF();
    } else if (cache->findTags(blk_addr, pkt->isSecure())) {
        DPRINTF(HWPrefetch, "Prefetch %#x has hit in cache, "
                "dropped.\n", blk_addr);
        reqHitInCache();
    } else if (cache->findMSHRQueue(blk_addr, pkt->isSecure())) {
        DPRINTF(HWPrefetch, "Prefetch %#x has hit in a MSHR, "
                "dropped.\n", blk_addr);
        reqHitInMSHR();
    } else if (cache->findWriteQueue(blk_addr, pkt->isSecure())) {
        DPRINTF(HWPrefetch, "Prefetch %#x has hit in the "
                "Write Buffer, dropped.\n", blk_addr);
        reqHitInWB();
    } else {
        delate = false;
    }
    return delate;
}

bool
FDIPPrefetcher::findPIQ(Addr blk_addr)
{
    bool find = false;
    for (auto iter = piq.begin(); iter != piq.end(); iter++)
        {
                if (iter->pkt->getBlockAddr(blkSize) == blk_addr) {
            find = true;
            break;
        }
        }
    return find;
}

void
FDIPPrefetcher::flushPIQ()
{
    piqFlush();
    for (auto iter = piq.begin(); iter != piq.end(); iter++)
    {
        delete iter->pkt;
    }
    piq.clear();
}

Addr
FDIPPrefetcher::regenerateBlkAddr(CacheBlk* blk)
{
    return tags->regenerateBlkAddr(blk);
}

} // namespace prefetch
} // namespace gem5
